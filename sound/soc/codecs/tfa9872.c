/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Based of tfa9895.c
 * Register definitions taken from tfa98xx kernel driver:
 * Copyright (C) 2014-2020 NXP Semiconductors, All Rights Reserved.
 */

#include <linux/bitfield.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/soc.h>

#define TFA987X_SYS_CTRL0		0x00
#define TFA987X_SYS_CTRL0_PWDN_MSK	BIT(0)
#define TFA987X_SYS_CTRL0_I2CR_MSK	BIT(1)
#define TFA987X_SYS_CTRL0_AMPE_MSK	BIT(3)
#define TFA987X_SYS_CTRL0_DCDC_MSK	BIT(4)

#define TFA987X_SYS_CTRL1		0x01
#define TFA987X_SYS_CTRL1_MANSCONF_MSK	BIT(2)
#define TFA987X_SYS_CTRL1_MANSAOOSC_MSK	BIT(4)

#define TFA987X_SYS_CTRL2		0x02
#define TFA987X_SYS_CTRL2_AUDFS_MSK	GENMASK(3, 0)
#define TFA987X_SYS_CTRL2_FRACTDEL_MSK	GENMASK(10, 5)

#define TFA987X_REV			0x03
#define TFA9873_SYS_CTRL4		0x04
#define TFA9873_SYS_CTRL4_FSSYNCEN_MSK	BIT(8)
#define TFA987X_CLK_GATING_CTRL		0x05
#define TFA9873_KEY_HIDE		0x0f
#define TFA9873_KEY_HIDE_UNLOCK		0x5a6b

#define TFA987X_TDM_CFG0		0x20
#define TFA9873_TDM_CFG0_NSLOTS_MSK	GENMASK(4, 1)
#define TFA987X_TDM_CFG0_FSBCLKS_MSK	GENMASK(15, 12)
#define TFA987X_TDM_CFG1		0x21
#define TFA987X_TDM_CFG1_NSLOTS_MSK	GENMASK(3,  0)
#define TFA987X_TDM_CFG1_SLOTBITS_MSK	GENMASK(8,  4)
#define TFA987X_TDM_CFG2		0x22
#define TFA987X_TDM_CFG2_SWIDTH_MSK	GENMASK(6,  2)
#define TFA987X_TDM_CFG3		0x23
#define TFA987X_TDM_CFG3_SPKE_MSK	BIT(0)
#define TFA987X_TDM_CFG3_DCE_MSK	BIT(1)
#define TFA987X_TDM_CFG3_CSE_MSK	BIT(3)
#define TFA987X_TDM_CFG3_VSE_MSK	BIT(4)
#define TFA987X_TDM_CFG6		0x26
#define TFA987X_TDM_CFG6_SPKS_MSK	GENMASK(3,  0)
#define TFA987X_TDM_CFG6_DCS_MSK	GENMASK(4,  7)
#define TFA987X_TDM_CFG6_CSS_MSK	GENMASK(15, 12)
#define TFA987X_TDM_CFG7		0x27
#define TFA987X_TDM_CFG7_VSS_MSK	GENMASK(3,  0)

#define TFA987X_AUDIO_CTRL		0x51
#define TFA987X_AUDIO_CTRL_BSSS_MSK	BIT(0)
#define TFA9872_AUDIO_CTRL_INTSMUTE_MSK	BIT(1)
#define TFA987X_AUDIO_CTRL_HPFBYP_MSK	BIT(5)
#define TFA987X_AUDIO_CTRL_DPSA_MSK     BIT(7)

#define TFA987X_AMP_CFG			0x52
#define TFA987X_AMP_CFG_CLIPCTRL_MSK	GENMASK(4, 2)
#define TFA987X_AMP_CFG_GAIN_MSK	GENMASK(12, 5)
#define TFA987X_AMP_CFG_SLOPEE_MSK	BIT(13)
#define TFA987X_AMP_CFG_SLOPESET_MSK	BIT(14)

#define TFA987X_KEY1_PWM_CFG		0x58
#define TFA987X_TDM_CFG8		0x61
#define TFA987X_TDM_CFG8_DCG_MSK	GENMASK(5, 2)
#define TFA987X_TDM_CFG8_SPKG_MSK	GENMASK(9, 6)

#define TFA987X_LOW_NOISE_GAIN1		0x62
#define TFA987X_LOW_NOISE_GAIN2		0x63
#define TFA987X_MODE1_DET1		0x64
#define TFA987X_MODE1_DET1_LPM1MODE_MSK	GENMASK(15, 14)
#define TFA987X_MODE1_DETECTOR2		0x65
#define TFA987X_TDM_SRC			0x68
#define TFA987X_CURSENSE_COMP		0x6f
#define TFA987X_DCDC_CTRL0		0x70
#define TFA9872_DCDC_CTRL0_DCVOS_MSK	GENMASK(2,  0)
#define TFA987X_DCDC_CTRL0_MCC_MSK	GENMASK(6,  3)
#define TFA987X_DCDC_CTRL0_DCIE_MSK	BIT(9)
#define TFA987X_DCDC_CTRL1		0x71
#define TFA987X_DCDC_CTRL4		0x74
#define TFA9872_DCDC_CTRL4_DCVOF_MSK	GENMASK(2,  0)
#define TFA987X_DCDC_CTRL4_DCTRIP_MSK	GENMASK(8,  4)
#define TFA987X_DCDC_CTRL5		0x75
#define TFA987X_DCDC_CTRL5_DCTRIP2_MSK	GENMASK(7,  3)
#define TFA987X_DCDC_CTRL6		0x76
#define TFA9874_DCDC_CTRL6_DCVOF_MSK	GENMASK(8,  3)
#define TFA9874_DCDC_CTRL6_DCVOS_MSK	GENMASK(14,  9)

#define TFA9873_KEY1			0xa0
#define TFA9873_KEY1_SEED		0xfb
#define TFA9873_KEY1_XOR		0x5a
#define TFA9873_KEY2			0xa1
#define TFA9873_KEY2_UNLOCK		0x5a

static const struct reg_sequence tfa9873_n1a_init[] = {
	{ 0x02, 0x0628 }, { 0x4c, 0x00e9 }, { 0x52, 0x17d0 },
	{ 0x56, 0x0011 }, { 0x58, 0x0200 }, { 0x59, 0x0001 },
	{ 0x5f, 0x0180 }, { 0x61, 0x0183 }, { 0x63, 0x055a },
	{ 0x65, 0x0542 }, { 0x6f, 0x00a3 }, { 0x70, 0xa3fb },
	{ 0x71, 0x007e }, { 0x83, 0x009a }, { 0x84, 0x0211 },
	{ 0x85, 0x0382 }, { 0x8c, 0x0210 }, { 0xd5, 0x0000 },
};

static const struct reg_sequence tfa9873_n1b_init[] = {
	{ 0x02, 0x0628 }, { 0x61, 0x0183 }, { 0x63, 0x005a },
	{ 0x6f, 0x0083 }, { 0x70, 0xa3fb }, { 0x73, 0x0187 },
	{ 0x83, 0x009a }, { 0x85, 0x0380 }, { 0xd5, 0x004d },
};

static int tfa987x_digital_mute(struct snd_soc_dai *codec_dai, int mute, int stream)
{
	struct snd_soc_component *component = codec_dai->component;
	int val = mute ? 0 : TFA987X_SYS_CTRL0_AMPE_MSK;

	if (stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	snd_soc_component_update_bits(component, TFA987X_SYS_CTRL0,
						 TFA987X_SYS_CTRL0_AMPE_MSK, val);

	return 0;
}

static const struct snd_soc_dapm_widget tfa987x_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("Speaker"),
	SND_SOC_DAPM_OUT_DRV_E("PWUP", TFA987X_SYS_CTRL0, 0, 1, NULL, 0, NULL, 0),
};

static const struct snd_soc_dapm_route tfa987x_dapm_routes[] = {
	{"PWUP", NULL, "HiFi Playback"},
	{"Speaker", NULL, "PWUP"},
};

static const struct snd_soc_component_driver tfa987x_component = {
	.dapm_widgets		= tfa987x_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(tfa987x_dapm_widgets),
	.dapm_routes		= tfa987x_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(tfa987x_dapm_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static const struct snd_soc_dai_ops tfa987x_dai_ops = {
	.mute_stream = tfa987x_digital_mute,
};

static struct snd_soc_dai_driver tfa987x_dai = {
	.name = "tfa987x-hifi",
	.playback = {
		.stream_name	= "HiFi Playback",
		.formats	= SNDRV_PCM_FMTBIT_S16_LE,
		.rates		= SNDRV_PCM_RATE_48000,
		.rate_min	= 48000,
		.rate_max	= 48000,
		.channels_min	= 1,
		.channels_max	= 2,
	},
	.ops = &tfa987x_dai_ops,
};

static const struct regmap_config tfa987x_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = 0xff,
};

static bool tfa987x_setup_dcdc(struct device *dev, struct regmap *rmap, u16 rev)
{
	u32 mcc, dcvof, dcvos, dctrip, dctrip2 = 0;
	int i;
	u32 *dest[] = { &mcc, &dcvof, &dcvos, &dctrip, &dctrip2 };
	const char *props[] = { "max-coil-current",
	      "first-boost-voltage", "second-boost-voltage",
	      "first-boost-trip-lvl", "second-boost-trip-lvl" };

	for (i = 0; i < ARRAY_SIZE(props); i++) {
		int ret;
		ret = of_property_read_u32(dev->of_node, props[i], dest[i]);
		if (ret < 0 && ret != -EINVAL)
			return false;
	}

	if (!FIELD_FIT(TFA987X_DCDC_CTRL0_MCC_MSK, mcc) ||
	    !FIELD_FIT(TFA987X_DCDC_CTRL4_DCTRIP_MSK, dctrip) ||
	    !FIELD_FIT(TFA987X_DCDC_CTRL5_DCTRIP2_MSK, dctrip2))
		return false;

	switch (rev & 0xff) {
	case 0x72:
		if (!FIELD_FIT(TFA9872_DCDC_CTRL4_DCVOF_MSK, dcvof) ||
		    !FIELD_FIT(TFA9872_DCDC_CTRL0_DCVOS_MSK, dcvos))
			return false;

		if (dcvof)
			regmap_update_bits(rmap, TFA987X_DCDC_CTRL4,
					TFA9872_DCDC_CTRL4_DCVOF_MSK,
					FIELD_PREP(TFA9872_DCDC_CTRL4_DCVOF_MSK, dcvof));
		if (dcvos)
			regmap_update_bits(rmap, TFA987X_DCDC_CTRL0,
					TFA9872_DCDC_CTRL0_DCVOS_MSK,
					FIELD_PREP(TFA9872_DCDC_CTRL0_DCVOS_MSK, dcvos));
		break;
	case 0x74:
		if (!FIELD_FIT(TFA9874_DCDC_CTRL6_DCVOF_MSK, dcvof) ||
		    !FIELD_FIT(TFA9874_DCDC_CTRL6_DCVOS_MSK, dcvos))
			return false;

		if (dcvof)
			regmap_update_bits(rmap, TFA987X_DCDC_CTRL6,
					TFA9874_DCDC_CTRL6_DCVOF_MSK,
					FIELD_PREP(TFA9874_DCDC_CTRL6_DCVOF_MSK, dcvof));

		if (dcvos)
			regmap_update_bits(rmap, TFA987X_DCDC_CTRL0,
					TFA9874_DCDC_CTRL6_DCVOS_MSK,
					FIELD_PREP(TFA9874_DCDC_CTRL6_DCVOS_MSK, dcvos));
		break;
	default:
		return false;
	}

	if (dctrip)
		regmap_update_bits(rmap, TFA987X_DCDC_CTRL4,
					 TFA987X_DCDC_CTRL4_DCTRIP_MSK,
					 FIELD_PREP(TFA987X_DCDC_CTRL4_DCTRIP_MSK, dctrip));
	if (dctrip2)
		regmap_update_bits(rmap, TFA987X_DCDC_CTRL5,
					 TFA987X_DCDC_CTRL5_DCTRIP2_MSK,
					 FIELD_PREP(TFA987X_DCDC_CTRL5_DCTRIP2_MSK, dctrip2));
	if (mcc)
		regmap_update_bits(rmap, TFA987X_DCDC_CTRL0,
					 TFA987X_DCDC_CTRL0_MCC_MSK |
					 TFA987X_DCDC_CTRL0_DCIE_MSK,
					 FIELD_PREP(TFA987X_DCDC_CTRL0_MCC_MSK, mcc) |
					 TFA987X_DCDC_CTRL0_DCIE_MSK);
	return true;
}

static int tfa9873_init(struct regmap *rmap, unsigned int rev)
{
	unsigned int seed;
	int ret;

	ret = regmap_write(rmap, TFA9873_KEY_HIDE, TFA9873_KEY_HIDE_UNLOCK);
	if (ret)
		return ret;

	ret = regmap_read(rmap, TFA9873_KEY1_SEED, &seed);
	if (ret)
		return ret;

	ret = regmap_write(rmap, TFA9873_KEY1, seed ^ TFA9873_KEY1_XOR);
	if (ret)
		return ret;

	ret = regmap_write(rmap, TFA9873_KEY2, TFA9873_KEY2_UNLOCK);
	if (ret)
		return ret;

	switch (rev) {
	case 0x0a73:
		ret = regmap_multi_reg_write(rmap, tfa9873_n1a_init,
					     ARRAY_SIZE(tfa9873_n1a_init));
		break;
	case 0x0b73:
		ret = regmap_multi_reg_write(rmap, tfa9873_n1b_init,
					     ARRAY_SIZE(tfa9873_n1b_init));
		break;
	}
	if (ret)
		return ret;

	return regmap_update_bits(rmap, TFA9873_SYS_CTRL4,
				  TFA9873_SYS_CTRL4_FSSYNCEN_MSK, 0);
}

static int tfa987x_i2c_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct regmap *rmap;
	unsigned int rev;
	u32 spk_slot = 0;
	int ret;

	rmap = devm_regmap_init_i2c(i2c, &tfa987x_regmap_config);
	if (IS_ERR(rmap))
		return PTR_ERR(rmap);

	ret = regmap_read(rmap, TFA987X_REV, &rev);
	if (ret < 0) {
		dev_err(dev, "Failed to read revision register: %d\n", ret);
		return ret;
	}

	switch (rev) {
		case 0x1b72:
		case 0x2b72:
		case 0x3b72:
		case 0x0a73:
		case 0x0b73:
		case 0x1a73:
		case 0x0c74:
			dev_info(dev, "Chip revision: 0x%04x\n", rev);
			break;
		default:
			dev_err(dev, "Unsupported chip revision: 0x%04x\n", rev);
			return -ENODEV;
	}

	of_property_read_u32(dev->of_node, "nxp,tdm-speaker-slot", &spk_slot);
	if (!FIELD_FIT(TFA987X_TDM_CFG6_SPKS_MSK, spk_slot))
		return dev_err_probe(dev, -EINVAL, "Invalid speaker slot %u\n", spk_slot);

	/* Perform soft reset */
	regmap_write(rmap, TFA987X_SYS_CTRL0, TFA987X_SYS_CTRL0_I2CR_MSK);

	if ((rev & 0xff) == 0x73) {
		ret = tfa9873_init(rmap, rev);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to initialize TFA9873\n");
	}

	/* Setup DC-DC Converter if we have configuration */
	regmap_update_bits(rmap, TFA987X_SYS_CTRL0, TFA987X_SYS_CTRL0_DCDC_MSK,
			   FIELD_PREP(TFA987X_SYS_CTRL0_DCDC_MSK,
				   (u16) tfa987x_setup_dcdc(dev, rmap, rev)));

	/* Disable DPSA */
	regmap_update_bits(rmap, TFA987X_AUDIO_CTRL,
				 TFA987X_AUDIO_CTRL_DPSA_MSK, 0);

	/* Setup TDM 16 bit 1 slot config */
	if ((rev & 0xff) == 0x73) {
		regmap_update_bits(rmap, TFA987X_TDM_CFG0,
					 TFA987X_TDM_CFG0_FSBCLKS_MSK |
					 TFA9873_TDM_CFG0_NSLOTS_MSK,
					 FIELD_PREP(TFA987X_TDM_CFG0_FSBCLKS_MSK, 0) |
					 FIELD_PREP(TFA9873_TDM_CFG0_NSLOTS_MSK, 1));
		regmap_update_bits(rmap, TFA987X_TDM_CFG1,
					 TFA987X_TDM_CFG1_SLOTBITS_MSK,
					 FIELD_PREP(TFA987X_TDM_CFG1_SLOTBITS_MSK, 15));
	} else {
		regmap_update_bits(rmap, TFA987X_TDM_CFG0,
					 TFA987X_TDM_CFG0_FSBCLKS_MSK,
					 FIELD_PREP(TFA987X_TDM_CFG0_FSBCLKS_MSK, 0));
		regmap_update_bits(rmap, TFA987X_TDM_CFG1,
					 TFA987X_TDM_CFG1_NSLOTS_MSK |
					 TFA987X_TDM_CFG1_SLOTBITS_MSK,
					 FIELD_PREP(TFA987X_TDM_CFG1_NSLOTS_MSK, 1) |
					 FIELD_PREP(TFA987X_TDM_CFG1_SLOTBITS_MSK, 15));
	}
	regmap_update_bits(rmap, TFA987X_TDM_CFG2,
				 TFA987X_TDM_CFG2_SWIDTH_MSK,
				 FIELD_PREP(TFA987X_TDM_CFG2_SWIDTH_MSK, 15));

	/* No current/voltage sense over TDM */
	regmap_update_bits(rmap, TFA987X_TDM_CFG3,
				 TFA987X_TDM_CFG3_SPKE_MSK |
				 TFA987X_TDM_CFG3_DCE_MSK |
				 TFA987X_TDM_CFG3_CSE_MSK |
				 TFA987X_TDM_CFG3_VSE_MSK,
				 TFA987X_TDM_CFG3_SPKE_MSK);
	regmap_update_bits(rmap, TFA987X_TDM_CFG6,
				 TFA987X_TDM_CFG6_SPKS_MSK,
				 FIELD_PREP(TFA987X_TDM_CFG6_SPKS_MSK, spk_slot));

	if ((rev & 0xff) == 0x72)
		regmap_update_bits(rmap, TFA987X_MODE1_DET1,
				 TFA987X_MODE1_DET1_LPM1MODE_MSK,
				 FIELD_PREP(TFA987X_MODE1_DET1_LPM1MODE_MSK, 1));

	/* Turn on this thing */
	regmap_update_bits(rmap, TFA987X_SYS_CTRL0,
				 TFA987X_SYS_CTRL0_PWDN_MSK, 0);

	regmap_update_bits(rmap, TFA987X_SYS_CTRL1,
				 TFA987X_SYS_CTRL1_MANSCONF_MSK,
				 TFA987X_SYS_CTRL1_MANSCONF_MSK);

	return devm_snd_soc_register_component(dev, &tfa987x_component,
						    &tfa987x_dai, 1);
}

static const struct of_device_id tfa987x_of_match[] = {
	{ .compatible = "nxp,tfa9872" },
	{ .compatible = "nxp,tfa9873" },
	{ .compatible = "nxp,tfa9874" },
	{ }
};
MODULE_DEVICE_TABLE(of, tfa987x_of_match);

static struct i2c_driver tfa987x_i2c_driver = {
	.driver = {
		.name = "tfa987x",
		.of_match_table = tfa987x_of_match,
	},
	.probe = tfa987x_i2c_probe,
};
module_i2c_driver(tfa987x_i2c_driver);

MODULE_DESCRIPTION("ASoC NXP Semiconductors TFA9872/TFA9874 driver");
MODULE_LICENSE("GPL v2");
