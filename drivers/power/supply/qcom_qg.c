// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, Danila Tikhonov <danila@jiaxyga.com>
 */

#include <linux/bitfield.h>
#include <linux/devm-helpers.h>
#include <linux/iio/consumer.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#define QG_PERPH_SUBTYPE_REG		0x05
#define QG_SUBTYPE_IBAT_10A		0x04

#define QG_STATUS2_REG			0x09
#define QG_GOOD_OCV_BIT			BIT(1)

#define QG_STATUS3_REG			0x0a
#define QG_COUNT_FIFO_RT_MASK		GENMASK(3, 0)

#define QG_DATA_CTL1_REG		0x41
#define QG_MASTER_HOLD_BIT		BIT(0)

#define QG_S2_NORMAL_MEAS_CTL2_REG	0x51
#define QG_FIFO_LENGTH_MASK		GENMASK(5, 3)
#define QG_NUM_OF_ACCUM_MASK		GENMASK(2, 0)

#define QG_S2_NORMAL_MEAS_CTL3_REG	0x52

#define QG_S3_SLEEP_OCV_IBAT_CTL1_REG	0x5d
#define QG_SLEEP_IBAT_QUALIFIED_LENGTH_MASK	GENMASK(2, 0)

/* BATT offsets */
#define QG_S7_PON_OCV_V_DATA0_REG	0x70 /* 2-byte 0x70-0x71 */
#define QG_S3_GOOD_OCV_V_DATA0_REG	0x74 /* 2-byte 0x74-0x75 */
#define QG_S2_NORMAL_AVG_V_DATA0_REG	0x80 /* 2-byte 0x80-0x81 */
#define QG_S2_NORMAL_AVG_I_DATA0_REG	0x82 /* 2-byte 0x82-0x83 */
#define QG_I_ACCUM_DATA0_RT_REG		0x8b /* 3-byte 0x8b-0x8d */
#define QG_ACCUM_CNT_RT_REG		0x8e
#define QG_V_FIFO0_DATA0_REG		0x90 /* 2-byte entries 0x90-0x9f */
#define QG_I_FIFO0_DATA0_REG		0xa0 /* 2-byte entries 0xa0-0xaf */
#define QG_SOC_MONOTONIC_REG		0xbf
#define QG_LAST_ADC_V_DATA0_REG		0xc0 /* 2-byte 0xc0-0xc1 */
#define QG_LAST_ADC_I_DATA0_REG		0xc2 /* 2-byte 0xc2-0xc3 */

#define QG_FIFO_MAX_LENGTH		8
#define QG_FIFO_RESET_VAL		0x8000

#define QG_S2_FIFO_LENGTH		5
#define QG_S2_ACC_LENGTH		128
#define QG_S2_ACC_INTERVAL_MS		100
#define QG_S3_ENTRY_FIFO_LENGTH		2

/* SRAM offsets */
#define QG_SDAM_VALID_OFFSET		0x46 /* 1-byte 0x46 */
#define QG_SDAM_SOC_OFFSET		0x47 /* 1-byte 0x47 */
#define QG_SDAM_TEMP_OFFSET		0x48 /* 2-byte 0x48-0x49 */
#define QG_SDAM_OCV_OFFSET		0x4c /* 4-byte 0x4c-0x4f */
#define QG_SDAM_TIME_OFFSET		0x54 /* 4-byte 0x54-0x57 */
#define QG_SDAM_LEARNED_CAPACITY_OFFSET	0x68 /* 2-byte 0x68-0x69 */
#define QG_SDAM_MAGIC_OFFSET		0x80 /* 4-byte 0x80-0x83 */
#define QG_SDAM_MAGIC			0x12345678

#define QG_V_LSB_NV			194637
#define QG_I_LSB_5A_NA			152588
#define QG_I_LSB_10A_NA			305176
#define QG_CLK_RATE			32000
#define QG_ACTUAL_CLK_RATE		32764

#define QG_OCV_MIN_UV			2000000
#define QG_OCV_MAX_UV			5000000
#define QG_SHUTDOWN_SOC_MAX_OFF_S	360
#define QG_SHUTDOWN_SOC_MAX_DELTA	10
#define QG_POLL_INTERVAL_MS		30000
#define QG_DISCHARGE_THRESHOLD_UA	50000
#define QG_RELAX_CURRENT_UA		50000
#define QG_RELAX_TIME_NS		(30ULL * 60 * NSEC_PER_SEC)
#define QG_UAUS_PER_UAH			3600000000LL

struct qcom_qg_chip {
	struct device *dev;
	struct regmap *regmap;
	unsigned int base;

	struct iio_channel *batt_therm_chan;

	struct nvmem_device *sdam;

	struct power_supply *batt_psy;
	struct power_supply_battery_info *batt_info;

	struct mutex lock;
	struct delayed_work poll_work;
	bool initialized;
	unsigned int i_lsb_na;
	s64 charge_uaus;
	s64 charge_full_uaus;
	int ocv_uv;
	int soc;
	int status;
	u64 active_ns;
};

static int qcom_qg_raw_to_uv(u32 raw)
{
	return div_u64((u64)raw * QG_V_LSB_NV, 1000);
}

static int qcom_qg_raw_to_ua(struct qcom_qg_chip *chip, s32 raw)
{
	return div_s64((s64)raw * chip->i_lsb_na, 1000);
}

static int qcom_qg_get_current(struct qcom_qg_chip *chip, u8 offset, int *val)
{
	s16 temp;
	u8 readval[2];
	int ret;

	ret = regmap_bulk_read(chip->regmap, chip->base + offset, readval, 2);
	if (ret) {
		dev_err(chip->dev, "Failed to read current: %d\n", ret);
		return ret;
	}

	temp = (s16)(readval[1] << 8 | readval[0]);
	*val = qcom_qg_raw_to_ua(chip, temp);

	/*
	 * PSY API expects charging batteries to report a positive current, which is inverted
	 * to what the PMIC reports.
	 */
	*val = -*val;

	return 0;
}

static int qcom_qg_get_voltage(struct qcom_qg_chip *chip, u8 offset, int *val)
{
	int ret, temp;
	u8 readval[2];

	ret = regmap_bulk_read(chip->regmap, chip->base + offset, readval, 2);
	if (ret) {
		dev_err(chip->dev, "Failed to read voltage: %d\n", ret);
		return ret;
	}

	temp = readval[1] << 8 | readval[0];
	*val = qcom_qg_raw_to_uv(temp);

	return 0;
}

static int qcom_qg_read_u16(struct qcom_qg_chip *chip, u8 offset, u16 *val)
{
	u8 readval[2];
	int ret;

	ret = regmap_bulk_read(chip->regmap, chip->base + offset, readval, 2);
	if (ret)
		return ret;

	*val = readval[1] << 8 | readval[0];

	return 0;
}

static int qcom_qg_get_temp(struct qcom_qg_chip *chip, int *decidegc)
{
	int ret, val;

	ret = iio_read_channel_processed(chip->batt_therm_chan, &val);
	if (ret < 0)
		return ret;

	*decidegc = val / 100;

	return 0;
}

static int qcom_qg_ocv_to_soc(struct qcom_qg_chip *chip, int ocv_uv)
{
	int decidegc = 250;

	qcom_qg_get_temp(chip, &decidegc);

	return clamp(power_supply_batinfo_ocv2cap(chip->batt_info, ocv_uv,
						  decidegc / 10), 0, 100);
}

static void qcom_qg_set_soc(struct qcom_qg_chip *chip, int soc)
{
	chip->charge_uaus = div_s64(chip->charge_full_uaus * soc, 100);
}

static s64 qcom_qg_sample_interval_us(u8 ctl3)
{
	return div_u64((u64)ctl3 * 10000 * QG_CLK_RATE + QG_ACTUAL_CLK_RATE / 2,
		       QG_ACTUAL_CLK_RATE);
}

static void qcom_qg_integrate(struct qcom_qg_chip *chip, s64 i_raw, s64 sample_us)
{
	chip->charge_uaus -= div_s64(i_raw * chip->i_lsb_na, 1000) * sample_us;
	chip->charge_uaus = clamp(chip->charge_uaus, 0LL, chip->charge_full_uaus);
}

static int qcom_qg_master_hold(struct qcom_qg_chip *chip, bool hold)
{
	int ret;

	ret = regmap_clear_bits(chip->regmap, chip->base + QG_DATA_CTL1_REG,
				QG_MASTER_HOLD_BIT);
	if (ret || !hold)
		return ret;

	return regmap_set_bits(chip->regmap, chip->base + QG_DATA_CTL1_REG,
			       QG_MASTER_HOLD_BIT);
}

static int qcom_qg_read_fifo(struct qcom_qg_chip *chip, unsigned int length,
			     s64 entry_us)
{
	u8 v_fifo[QG_FIFO_MAX_LENGTH * 2], i_fifo[QG_FIFO_MAX_LENGTH * 2];
	unsigned int i;
	int ret;

	length = min(length, QG_FIFO_MAX_LENGTH);
	if (!length)
		return 0;

	ret = regmap_bulk_read(chip->regmap, chip->base + QG_V_FIFO0_DATA0_REG,
			       v_fifo, length * 2);
	if (ret)
		return ret;

	ret = regmap_bulk_read(chip->regmap, chip->base + QG_I_FIFO0_DATA0_REG,
			       i_fifo, length * 2);
	if (ret)
		return ret;

	for (i = 0; i < length; i++) {
		u16 v_raw = v_fifo[2 * i + 1] << 8 | v_fifo[2 * i];
		u16 i_raw = i_fifo[2 * i + 1] << 8 | i_fifo[2 * i];

		if (v_raw == QG_FIFO_RESET_VAL || i_raw == QG_FIFO_RESET_VAL)
			continue;

		qcom_qg_integrate(chip, (s16)i_raw, entry_us);
	}

	dev_dbg(chip->dev, "fifo %u entries of %lld us, charge %lld uAh\n", length,
		entry_us, div64_s64(chip->charge_uaus, QG_UAUS_PER_UAH));

	return 0;
}

static int qcom_qg_read_accumulator(struct qcom_qg_chip *chip, s64 interval_us)
{
	unsigned int count;
	u8 acc_i[3];
	s32 sum;
	int ret;

	ret = regmap_read(chip->regmap, chip->base + QG_ACCUM_CNT_RT_REG, &count);
	if (ret || !count)
		return ret;

	ret = regmap_bulk_read(chip->regmap, chip->base + QG_I_ACCUM_DATA0_RT_REG,
			       acc_i, 3);
	if (ret)
		return ret;

	sum = sign_extend32(acc_i[2] << 16 | acc_i[1] << 8 | acc_i[0], 23);
	qcom_qg_integrate(chip, sum, interval_us);
	dev_dbg(chip->dev, "accumulator count %u sum %d\n", count, sum);

	return 0;
}

static int qcom_qg_collect(struct qcom_qg_chip *chip, bool realtime)
{
	unsigned int status3;
	s64 interval_us, entry_us;
	u8 ctl[2];
	int ret;

	lockdep_assert_held(&chip->lock);

	ret = regmap_bulk_read(chip->regmap, chip->base + QG_S2_NORMAL_MEAS_CTL2_REG,
			       ctl, 2);
	if (ret)
		return ret;

	interval_us = qcom_qg_sample_interval_us(ctl[1]);
	entry_us = interval_us << (FIELD_GET(QG_NUM_OF_ACCUM_MASK, ctl[0]) + 1);

	if (!realtime)
		return qcom_qg_read_fifo(chip, FIELD_GET(QG_FIFO_LENGTH_MASK, ctl[0]) + 1,
					 entry_us);

	ret = qcom_qg_master_hold(chip, true);
	if (ret)
		goto release;

	ret = regmap_read(chip->regmap, chip->base + QG_STATUS3_REG, &status3);
	if (ret)
		goto release;

	ret = qcom_qg_read_fifo(chip, FIELD_GET(QG_COUNT_FIFO_RT_MASK, status3), entry_us);
	if (ret)
		goto release;

	ret = qcom_qg_read_accumulator(chip, interval_us);

release:
	qcom_qg_master_hold(chip, false);
	return ret;
}

static int qcom_qg_config_s2(struct qcom_qg_chip *chip)
{
	u8 ctl[2];
	int ret;

	ret = qcom_qg_master_hold(chip, true);
	if (ret)
		goto release;

	ret = regmap_update_bits(chip->regmap, chip->base + QG_S2_NORMAL_MEAS_CTL2_REG,
				 QG_FIFO_LENGTH_MASK | QG_NUM_OF_ACCUM_MASK,
				 FIELD_PREP(QG_FIFO_LENGTH_MASK, QG_S2_FIFO_LENGTH - 1) |
				 FIELD_PREP(QG_NUM_OF_ACCUM_MASK, ilog2(QG_S2_ACC_LENGTH) - 1));
	if (ret)
		goto release;

	ret = regmap_write(chip->regmap, chip->base + QG_S2_NORMAL_MEAS_CTL3_REG,
			   QG_S2_ACC_INTERVAL_MS / 10);
	if (ret)
		goto release;

	ret = regmap_update_bits(chip->regmap, chip->base + QG_S3_SLEEP_OCV_IBAT_CTL1_REG,
				 QG_SLEEP_IBAT_QUALIFIED_LENGTH_MASK,
				 QG_S3_ENTRY_FIFO_LENGTH - 1);
	if (ret)
		goto release;

	ret = regmap_bulk_read(chip->regmap, chip->base + QG_S2_NORMAL_MEAS_CTL2_REG, ctl, 2);
	if (ret)
		goto release;

	dev_info(chip->dev, "S2 sample interval %lld us, %u samples per entry, %lu entries per FIFO\n",
		 qcom_qg_sample_interval_us(ctl[1]),
		 2U << FIELD_GET(QG_NUM_OF_ACCUM_MASK, ctl[0]),
		 FIELD_GET(QG_FIFO_LENGTH_MASK, ctl[0]) + 1);

release:
	qcom_qg_master_hold(chip, false);
	return ret;
}

static int qcom_qg_charger_status(struct qcom_qg_chip *chip)
{
	union power_supply_propval val;
	int avg_ua, last_ua;

	if (power_supply_get_property_from_supplier(chip->batt_psy,
						    POWER_SUPPLY_PROP_STATUS, &val))
		return POWER_SUPPLY_STATUS_UNKNOWN;

	if ((val.intval == POWER_SUPPLY_STATUS_CHARGING ||
	     val.intval == POWER_SUPPLY_STATUS_NOT_CHARGING) &&
	    !qcom_qg_get_current(chip, QG_S2_NORMAL_AVG_I_DATA0_REG, &avg_ua) &&
	    !qcom_qg_get_current(chip, QG_LAST_ADC_I_DATA0_REG, &last_ua) &&
	    avg_ua < -QG_DISCHARGE_THRESHOLD_UA &&
	    last_ua < -QG_DISCHARGE_THRESHOLD_UA)
		return POWER_SUPPLY_STATUS_DISCHARGING;

	return val.intval;
}

static void qcom_qg_store(struct qcom_qg_chip *chip)
{
	u32 ocv = chip->ocv_uv, time = ktime_get_real_seconds();
	u8 soc = chip->soc, valid = 1;
	int decidegc;
	s16 temp;

	if (!qcom_qg_get_temp(chip, &decidegc)) {
		temp = decidegc;
		nvmem_device_write(chip->sdam, QG_SDAM_TEMP_OFFSET, sizeof(temp), &temp);
	}
	nvmem_device_write(chip->sdam, QG_SDAM_OCV_OFFSET, sizeof(ocv), &ocv);
	nvmem_device_write(chip->sdam, QG_SDAM_TIME_OFFSET, sizeof(time), &time);
	nvmem_device_write(chip->sdam, QG_SDAM_SOC_OFFSET, sizeof(soc), &soc);
	nvmem_device_write(chip->sdam, QG_SDAM_VALID_OFFSET, sizeof(valid), &valid);
}

static void qcom_qg_update(struct qcom_qg_chip *chip)
{
	int soc, status, current_ua;

	lockdep_assert_held(&chip->lock);

	status = qcom_qg_charger_status(chip);
	if (status == POWER_SUPPLY_STATUS_FULL)
		chip->charge_uaus = chip->charge_full_uaus;

	if (status == POWER_SUPPLY_STATUS_CHARGING ||
	    qcom_qg_get_current(chip, QG_S2_NORMAL_AVG_I_DATA0_REG, &current_ua) ||
	    abs(current_ua) > QG_RELAX_CURRENT_UA)
		chip->active_ns = ktime_get_boottime_ns();

	soc = div64_s64(chip->charge_uaus * 100 + chip->charge_full_uaus / 2,
			chip->charge_full_uaus);
	if (status == POWER_SUPPLY_STATUS_CHARGING)
		soc = min(soc, 99);

	if (soc == chip->soc && status == chip->status)
		return;

	if (soc != chip->soc) {
		chip->soc = soc;
		regmap_write(chip->regmap, chip->base + QG_SOC_MONOTONIC_REG,
			     DIV_ROUND_CLOSEST(soc * 255, 100));
		qcom_qg_store(chip);
	}
	chip->status = status;

	power_supply_changed(chip->batt_psy);
}

static void qcom_qg_poll_work(struct work_struct *work)
{
	struct qcom_qg_chip *chip = container_of(work, struct qcom_qg_chip,
						 poll_work.work);

	mutex_lock(&chip->lock);
	if (!chip->initialized) {
		mutex_unlock(&chip->lock);
		return;
	}
	qcom_qg_update(chip);
	mutex_unlock(&chip->lock);

	schedule_delayed_work(&chip->poll_work, msecs_to_jiffies(QG_POLL_INTERVAL_MS));
}

static irqreturn_t qcom_qg_fifo_done_irq(int irq, void *data)
{
	struct qcom_qg_chip *chip = data;

	mutex_lock(&chip->lock);
	if (!qcom_qg_collect(chip, false))
		qcom_qg_update(chip);
	mutex_unlock(&chip->lock);

	return IRQ_HANDLED;
}

static int qcom_qg_good_ocv(struct qcom_qg_chip *chip)
{
	unsigned int status2;
	u16 raw;
	int ret;

	lockdep_assert_held(&chip->lock);

	ret = regmap_read(chip->regmap, chip->base + QG_STATUS2_REG, &status2);
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, chip->base + QG_STATUS2_REG, 0);
	if (ret)
		return ret;

	if (!(status2 & QG_GOOD_OCV_BIT))
		return 0;

	ret = qcom_qg_read_u16(chip, QG_S3_GOOD_OCV_V_DATA0_REG, &raw);
	if (ret)
		return ret;

	chip->ocv_uv = qcom_qg_raw_to_uv(raw);
	if (ktime_get_boottime_ns() - chip->active_ns < QG_RELAX_TIME_NS) {
		dev_dbg(chip->dev, "good OCV %d uV ignored, battery not relaxed\n", chip->ocv_uv);
		return 0;
	}

	qcom_qg_set_soc(chip, qcom_qg_ocv_to_soc(chip, chip->ocv_uv));
	dev_dbg(chip->dev, "good OCV %d uV\n", chip->ocv_uv);

	return 0;
}

static irqreturn_t qcom_qg_good_ocv_irq(int irq, void *data)
{
	struct qcom_qg_chip *chip = data;

	mutex_lock(&chip->lock);
	if (!qcom_qg_good_ocv(chip))
		qcom_qg_update(chip);
	mutex_unlock(&chip->lock);

	return IRQ_HANDLED;
}

static irqreturn_t qcom_qg_vbat_empty_irq(int irq, void *data)
{
	struct qcom_qg_chip *chip = data;

	dev_warn(chip->dev, "battery empty\n");

	mutex_lock(&chip->lock);
	chip->charge_uaus = 0;
	qcom_qg_update(chip);
	mutex_unlock(&chip->lock);

	return IRQ_HANDLED;
}

static int qcom_qg_request_irq(struct qcom_qg_chip *chip, const char *name,
			       irq_handler_t handler)
{
	int irq, ret;

	irq = platform_get_irq_byname_optional(to_platform_device(chip->dev), name);
	if (irq == -ENXIO)
		return 0;
	if (irq < 0)
		return irq;

	ret = devm_request_threaded_irq(chip->dev, irq, NULL, handler, IRQF_ONESHOT,
					name, chip);
	if (ret)
		return dev_err_probe(chip->dev, ret, "Failed to request %s IRQ\n", name);

	return 0;
}

static int qcom_qg_init_charge(struct qcom_qg_chip *chip)
{
	int capacity_uah = chip->batt_info->charge_full_design_uah;
	u32 magic = 0, sdam_time = 0, now = ktime_get_real_seconds();
	u8 valid = 0, sdam_soc = 0;
	s16 learned_mah = 0;
	int pon_soc, ret;
	bool use_sdam;
	u16 raw;

	nvmem_device_read(chip->sdam, QG_SDAM_LEARNED_CAPACITY_OFFSET,
			  sizeof(learned_mah), &learned_mah);
	if (capacity_uah <= 0 ||
	    (learned_mah * 1000 >= capacity_uah / 2 &&
	     learned_mah * 1000 <= capacity_uah / 10 * 11))
		capacity_uah = learned_mah * 1000;
	if (capacity_uah <= 0)
		return dev_err_probe(chip->dev, -EINVAL, "Unknown battery capacity\n");
	chip->charge_full_uaus = capacity_uah * QG_UAUS_PER_UAH;

	ret = qcom_qg_read_u16(chip, QG_S7_PON_OCV_V_DATA0_REG, &raw);
	if (ret)
		return ret;
	chip->ocv_uv = qcom_qg_raw_to_uv(raw);
	if (chip->ocv_uv < QG_OCV_MIN_UV || chip->ocv_uv > QG_OCV_MAX_UV)
		nvmem_device_read(chip->sdam, QG_SDAM_OCV_OFFSET, sizeof(chip->ocv_uv),
				  &chip->ocv_uv);
	pon_soc = qcom_qg_ocv_to_soc(chip, chip->ocv_uv);

	nvmem_device_read(chip->sdam, QG_SDAM_VALID_OFFSET, sizeof(valid), &valid);
	nvmem_device_read(chip->sdam, QG_SDAM_SOC_OFFSET, sizeof(sdam_soc), &sdam_soc);
	nvmem_device_read(chip->sdam, QG_SDAM_TIME_OFFSET, sizeof(sdam_time), &sdam_time);
	nvmem_device_read(chip->sdam, QG_SDAM_MAGIC_OFFSET, sizeof(magic), &magic);

	use_sdam = valid == 1 && magic == QG_SDAM_MAGIC && sdam_soc <= 100 &&
		   ((sdam_time <= now && now - sdam_time < QG_SHUTDOWN_SOC_MAX_OFF_S) ||
		    abs(pon_soc - sdam_soc) <= QG_SHUTDOWN_SOC_MAX_DELTA);

	qcom_qg_set_soc(chip, use_sdam ? sdam_soc : pon_soc);
	chip->soc = -1;
	chip->status = POWER_SUPPLY_STATUS_UNKNOWN;

	dev_info(chip->dev,
		 "%d mAh, PON OCV %d uV = %d%%, shutdown SOC %u%% valid %u magic %#x age %lld s, using %s\n",
		 capacity_uah / 1000, chip->ocv_uv, pon_soc, sdam_soc, valid, magic,
		 (s64)now - sdam_time, use_sdam ? "shutdown SOC" : "PON OCV");

	return 0;
}

static enum power_supply_property qcom_qg_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
};

static int qcom_qg_get_state(struct qcom_qg_chip *chip,
			     enum power_supply_property psp, int *val)
{
	guard(mutex)(&chip->lock);

	if (!chip->initialized)
		return -ENODATA;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		*val = chip->status;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		*val = chip->ocv_uv;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		*val = div64_s64(chip->charge_full_uaus, QG_UAUS_PER_UAH);
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		*val = div64_s64(chip->charge_uaus, QG_UAUS_PER_UAH);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		*val = chip->soc;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int qcom_qg_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct qcom_qg_chip *chip = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
	case POWER_SUPPLY_PROP_CHARGE_FULL:
	case POWER_SUPPLY_PROP_CHARGE_NOW:
	case POWER_SUPPLY_PROP_CAPACITY:
		return qcom_qg_get_state(chip, psp, &val->intval);
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = chip->batt_info->voltage_max_design_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = chip->batt_info->voltage_min_design_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = qcom_qg_get_voltage(chip,
				QG_LAST_ADC_V_DATA0_REG, &val->intval);
		if (ret)
			return ret;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		ret = qcom_qg_get_voltage(chip,
				QG_S2_NORMAL_AVG_V_DATA0_REG, &val->intval);
		if (ret)
			return ret;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = qcom_qg_get_current(chip,
				QG_LAST_ADC_I_DATA0_REG, &val->intval);
		if (ret)
			return ret;
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		ret = qcom_qg_get_current(chip,
				QG_S2_NORMAL_AVG_I_DATA0_REG, &val->intval);
		if (ret)
			return ret;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = chip->batt_info->charge_full_design_uah;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = iio_read_channel_processed
					(chip->batt_therm_chan, &val->intval);
		if (ret < 0)
			return ret;
		val->intval /= 100; /* 1/1000 °C (millidegC) to 1/10 °C */
		break;
	default:
		dev_err(chip->dev, "invalid property: %d\n", psp);
		return -EINVAL;
	}
	return 0;
}

static void qcom_qg_external_power_changed(struct power_supply *psy)
{
	struct qcom_qg_chip *chip = power_supply_get_drvdata(psy);

	mod_delayed_work(system_wq, &chip->poll_work, 0);
}

static struct power_supply_desc batt_psy_desc = {
	.name = "qcom_qg",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = qcom_qg_props,
	.num_properties = ARRAY_SIZE(qcom_qg_props),
	.get_property = qcom_qg_get_property,
	.external_power_changed = qcom_qg_external_power_changed,
};

static int qcom_qg_probe(struct platform_device *pdev)
{
	struct qcom_qg_chip *chip;
	struct power_supply_config psy_cfg = {};
	int ret;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;

	ret = devm_mutex_init(chip->dev, &chip->lock);
	if (ret)
		return ret;

	ret = devm_delayed_work_autocancel(chip->dev, &chip->poll_work, qcom_qg_poll_work);
	if (ret)
		return ret;

	/* Regmap */
	chip->regmap = dev_get_regmap(chip->dev->parent, NULL);
	if (!chip->regmap)
		return dev_err_probe(chip->dev, -ENODEV,
				     "Failed to locate the regmap\n");

	/* Get base address */
	ret = device_property_read_u32(chip->dev, "reg", &chip->base);
	if (ret < 0)
		return dev_err_probe(chip->dev, ret,
				     "Couldn't read base address\n");

	ret = regmap_read(chip->regmap, chip->base + QG_PERPH_SUBTYPE_REG, &chip->i_lsb_na);
	if (ret)
		return dev_err_probe(chip->dev, ret, "Couldn't read subtype\n");
	chip->i_lsb_na = chip->i_lsb_na == QG_SUBTYPE_IBAT_10A ? QG_I_LSB_10A_NA :
								  QG_I_LSB_5A_NA;

	/* ADC for thermal channel */
	chip->batt_therm_chan = devm_iio_channel_get(chip->dev, "batt-therm");
	if (IS_ERR(chip->batt_therm_chan))
		return dev_err_probe(chip->dev, PTR_ERR(chip->batt_therm_chan),
				     "Couldn't get batt-therm IIO channel\n");

	/* NVMEM for SDAM access */
	chip->sdam = devm_nvmem_device_get(chip->dev, NULL);
	if (IS_ERR(chip->sdam))
		return dev_err_probe(chip->dev, PTR_ERR(chip->sdam),
				     "Couldn't get SDAM nvmem device\n");

	psy_cfg.drv_data = chip;
	psy_cfg.fwnode = dev_fwnode(chip->dev);

	/* Power supply */
	chip->batt_psy =
		devm_power_supply_register(chip->dev, &batt_psy_desc, &psy_cfg);
	if (IS_ERR(chip->batt_psy))
		return dev_err_probe(chip->dev, PTR_ERR(chip->batt_psy),
				     "Failed to register power supply\n");

	/* Battery info */
	ret = power_supply_get_battery_info(chip->batt_psy, &chip->batt_info);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "Failed to get battery info\n");

	platform_set_drvdata(pdev, chip);

	mutex_lock(&chip->lock);
	chip->active_ns = ktime_get_boottime_ns();
	ret = qcom_qg_init_charge(chip);
	if (!ret)
		ret = qcom_qg_collect(chip, true);
	if (!ret)
		ret = qcom_qg_config_s2(chip);
	if (!ret) {
		chip->initialized = true;
		qcom_qg_update(chip);
	}
	mutex_unlock(&chip->lock);
	if (ret)
		return ret;

	ret = qcom_qg_request_irq(chip, "fifo-done", qcom_qg_fifo_done_irq);
	if (ret)
		return ret;

	ret = qcom_qg_request_irq(chip, "good-ocv", qcom_qg_good_ocv_irq);
	if (ret)
		return ret;

	ret = qcom_qg_request_irq(chip, "vbat-empty", qcom_qg_vbat_empty_irq);
	if (ret)
		return ret;

	schedule_delayed_work(&chip->poll_work, msecs_to_jiffies(QG_POLL_INTERVAL_MS));

	return 0;
}

static void qcom_qg_shutdown(struct platform_device *pdev)
{
	struct qcom_qg_chip *chip = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&chip->poll_work);

	guard(mutex)(&chip->lock);

	if (chip->initialized && !qcom_qg_collect(chip, true)) {
		qcom_qg_update(chip);
		qcom_qg_store(chip);
	}
}

static int qcom_qg_suspend(struct device *dev)
{
	struct qcom_qg_chip *chip = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&chip->poll_work);

	guard(mutex)(&chip->lock);

	if (chip->initialized && !qcom_qg_collect(chip, true)) {
		qcom_qg_update(chip);
		qcom_qg_store(chip);
	}

	return 0;
}

static int qcom_qg_resume(struct device *dev)
{
	struct qcom_qg_chip *chip = dev_get_drvdata(dev);

	mutex_lock(&chip->lock);
	if (chip->initialized)
		qcom_qg_good_ocv(chip);
	mutex_unlock(&chip->lock);

	mod_delayed_work(system_wq, &chip->poll_work, 0);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(qcom_qg_pm_ops, qcom_qg_suspend, qcom_qg_resume);

static const struct of_device_id qcom_qg_of_match[] = {
	{ .compatible = "qcom,pm6150-qg", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, qcom_qg_of_match);

static struct platform_driver qcom_qg_driver = {
	.driver = {
		.name = "qcom,qcom_qg",
		.of_match_table = qcom_qg_of_match,
		.pm = pm_sleep_ptr(&qcom_qg_pm_ops),
	},
	.probe = qcom_qg_probe,
	.shutdown = qcom_qg_shutdown,
};

module_platform_driver(qcom_qg_driver);

MODULE_AUTHOR("Danila Tikhonov <danila@jiaxyga.com>");
MODULE_DESCRIPTION("Qualcomm PMIC QGauge (QG) driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_CONSUMER");
