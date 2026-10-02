// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bits.h>
#include <linux/iopoll.h>
#include <linux/pm_domain.h>

#include "iris_core.h"
#include "iris_instance.h"
#include "iris_vpu_common.h"

#define VPU1_CPU_CS_BASE_OFFS			0x000D2000
#define VPU1_CPU_IC_BASE_OFFS			0x000DF000
#define VPU1_WRAPPER_BASE_OFFS			0x000E0000

#define VPU1_CPU_CS_A2HSOFTINTCLR		(VPU1_CPU_CS_BASE_OFFS + 0x1C)
#define VPU1_DSP_QTBL_ADDR			(VPU1_CPU_CS_BASE_OFFS + 0x34)
#define VPU1_DSP_UC_REGION_ADDR			(VPU1_CPU_CS_BASE_OFFS + 0x38)
#define VPU1_DSP_UC_REGION_SIZE			(VPU1_CPU_CS_BASE_OFFS + 0x3C)
#define VPU1_CTRL_INIT				(VPU1_CPU_CS_BASE_OFFS + 0x48)
#define VPU1_CTRL_STATUS			(VPU1_CPU_CS_BASE_OFFS + 0x4C)
#define VPU1_QTBL_INFO				(VPU1_CPU_CS_BASE_OFFS + 0x50)
#define VPU1_QTBL_ADDR				(VPU1_CPU_CS_BASE_OFFS + 0x54)
#define VPU1_SFR_ADDR				(VPU1_CPU_CS_BASE_OFFS + 0x5C)
#define VPU1_UC_REGION_ADDR			(VPU1_CPU_CS_BASE_OFFS + 0x64)
#define VPU1_UC_REGION_SIZE			(VPU1_CPU_CS_BASE_OFFS + 0x68)

#define VPU1_CTRL_INIT_ENABLE			BIT(0)
#define VPU1_CTRL_ERROR_STATUS			GENMASK(7, 1)
#define VPU1_CTRL_ERROR_UC_REGION		0x4
#define VPU1_CTRL_STATUS_PC_READY		BIT(8)
#define VPU1_CTRL_INIT_IDLE_MSG			BIT(30)
#define VPU1_QTBL_ENABLE			BIT(0)

#define VPU1_CPU_IC_SOFTINT			(VPU1_CPU_IC_BASE_OFFS + 0x18)
#define VPU1_CPU_IC_SOFTINT_H2A			BIT(15)

#define VPU1_WRAPPER_INTR_STATUS		(VPU1_WRAPPER_BASE_OFFS + 0x0C)
#define VPU1_WRAPPER_INTR_MASK			(VPU1_WRAPPER_BASE_OFFS + 0x10)
#define VPU1_WRAPPER_INTR_CLEAR			(VPU1_WRAPPER_BASE_OFFS + 0x14)
#define VPU1_WRAPPER_INTR_A2H			BIT(2)
#define VPU1_WRAPPER_INTR_A2HWD			BIT(4)

#define VPU1_WRAPPER_CPU_CLOCK_CONFIG		(VPU1_WRAPPER_BASE_OFFS + 0x2000)
#define VPU1_WRAPPER_CPU_CGC_DIS		(VPU1_WRAPPER_BASE_OFFS + 0x2010)
#define VPU1_WRAPPER_CPU_STATUS			(VPU1_WRAPPER_BASE_OFFS + 0x2014)
#define VPU1_WRAPPER_CPU_STATUS_WFI		BIT(0)

#define VPU1_CVP_POWER_DOMAIN			IRIS_VPP0_HW_POWER_DOMAIN

static struct device *iris_vpu1_pd(struct iris_core *core, u32 idx)
{
	return core->pmdomain_tbl->pd_devs[idx];
}

static int iris_vpu1_power_off_controller(struct iris_core *core)
{
	iris_disable_unprepare_clock(core, IRIS_AHB_CLK);
	iris_disable_unprepare_clock(core, IRIS_CTRL_CLK);
	iris_disable_unprepare_clock(core, IRIS_AXI_CLK);
	iris_disable_power_domains(core, iris_vpu1_pd(core, IRIS_CTRL_POWER_DOMAIN));

	return 0;
}

static void iris_vpu1_power_off_hw(struct iris_core *core)
{
	iris_disable_unprepare_clock(core, IRIS_CVP_HW_CLK);
	iris_disable_unprepare_clock(core, IRIS_CVP_HW_AHB_CLK);
	iris_disable_unprepare_clock(core, IRIS_HW_CLK);
	iris_disable_unprepare_clock(core, IRIS_HW_AHB_CLK);

	dev_pm_genpd_set_hwmode(iris_vpu1_pd(core, VPU1_CVP_POWER_DOMAIN), false);
	dev_pm_genpd_set_hwmode(iris_vpu1_pd(core, IRIS_HW_POWER_DOMAIN), false);

	iris_disable_power_domains(core, iris_vpu1_pd(core, VPU1_CVP_POWER_DOMAIN));
	iris_disable_power_domains(core, iris_vpu1_pd(core, IRIS_HW_POWER_DOMAIN));
}

static int iris_vpu1_power_on_hw(struct iris_core *core)
{
	struct device *hw_pd = iris_vpu1_pd(core, IRIS_HW_POWER_DOMAIN);
	struct device *cvp_pd = iris_vpu1_pd(core, VPU1_CVP_POWER_DOMAIN);
	int ret;

	ret = iris_enable_power_domains(core, hw_pd);
	if (ret)
		return ret;

	ret = iris_enable_power_domains(core, cvp_pd);
	if (ret)
		goto err_disable_hw_pd;

	ret = iris_prepare_enable_clock(core, IRIS_HW_AHB_CLK);
	if (ret)
		goto err_disable_cvp_pd;

	ret = iris_prepare_enable_clock(core, IRIS_HW_CLK);
	if (ret)
		goto err_disable_hw_ahb_clk;

	ret = iris_prepare_enable_clock(core, IRIS_CVP_HW_AHB_CLK);
	if (ret)
		goto err_disable_hw_clk;

	ret = iris_prepare_enable_clock(core, IRIS_CVP_HW_CLK);
	if (ret)
		goto err_disable_cvp_ahb_clk;

	ret = dev_pm_genpd_set_hwmode(hw_pd, true);
	if (ret)
		goto err_disable_cvp_clk;

	ret = dev_pm_genpd_set_hwmode(cvp_pd, true);
	if (ret)
		goto err_hw_swmode;

	return 0;

err_hw_swmode:
	dev_pm_genpd_set_hwmode(hw_pd, false);
err_disable_cvp_clk:
	iris_disable_unprepare_clock(core, IRIS_CVP_HW_CLK);
err_disable_cvp_ahb_clk:
	iris_disable_unprepare_clock(core, IRIS_CVP_HW_AHB_CLK);
err_disable_hw_clk:
	iris_disable_unprepare_clock(core, IRIS_HW_CLK);
err_disable_hw_ahb_clk:
	iris_disable_unprepare_clock(core, IRIS_HW_AHB_CLK);
err_disable_cvp_pd:
	iris_disable_power_domains(core, cvp_pd);
err_disable_hw_pd:
	iris_disable_power_domains(core, hw_pd);

	return ret;
}

static int iris_vpu1_set_hwmode(struct iris_core *core)
{
	return 0;
}

static void iris_vpu1_set_preset_registers(struct iris_core *core)
{
	writel(0x0, core->reg_base + VPU1_WRAPPER_CPU_CGC_DIS);
	writel(0x0, core->reg_base + VPU1_WRAPPER_CPU_CLOCK_CONFIG);
}

static void iris_vpu1_interrupt_init(struct iris_core *core)
{
	u32 mask_val;

	mask_val = readl(core->reg_base + VPU1_WRAPPER_INTR_MASK);
	mask_val &= ~(VPU1_WRAPPER_INTR_A2HWD | VPU1_WRAPPER_INTR_A2H);
	writel(mask_val, core->reg_base + VPU1_WRAPPER_INTR_MASK);
}

static int iris_vpu1_boot_firmware(struct iris_core *core)
{
	u32 qtbl = (u32)core->iface_q_table_daddr;
	u32 uc_size = iris_hfi_queue_uc_region_size();
	u32 ctrl_status = 0, count = 0, max_tries = 1000;

	writel(qtbl, core->reg_base + VPU1_UC_REGION_ADDR);
	writel(uc_size, core->reg_base + VPU1_UC_REGION_SIZE);
	writel(qtbl, core->reg_base + VPU1_QTBL_ADDR);
	writel(VPU1_QTBL_ENABLE, core->reg_base + VPU1_QTBL_INFO);

	if (core->sfr_daddr)
		writel((u32)core->sfr_daddr + core->iris_firmware_data->core_arch,
		       core->reg_base + VPU1_SFR_ADDR);

	writel(qtbl, core->reg_base + VPU1_DSP_QTBL_ADDR);
	writel(qtbl, core->reg_base + VPU1_DSP_UC_REGION_ADDR);
	writel(uc_size, core->reg_base + VPU1_DSP_UC_REGION_SIZE);

	writel(VPU1_CTRL_INIT_ENABLE, core->reg_base + VPU1_CTRL_INIT);

	while (!ctrl_status && count < max_tries) {
		ctrl_status = readl(core->reg_base + VPU1_CTRL_STATUS);
		if ((ctrl_status & VPU1_CTRL_ERROR_STATUS) == VPU1_CTRL_ERROR_UC_REGION) {
			dev_err(core->dev, "invalid setting for uc_region\n");
			break;
		}

		usleep_range(50, 100);
		count++;
	}

	if (count >= max_tries) {
		dev_err(core->dev, "error booting up iris firmware, ctrl status %#x\n",
			ctrl_status);
		return -ETIME;
	}

	return 0;
}

static void iris_vpu1_raise_interrupt(struct iris_core *core)
{
	writel(VPU1_CPU_IC_SOFTINT_H2A, core->reg_base + VPU1_CPU_IC_SOFTINT);
}

static void iris_vpu1_clear_interrupt(struct iris_core *core)
{
	u32 intr_status, mask;

	intr_status = readl(core->reg_base + VPU1_WRAPPER_INTR_STATUS);
	mask = VPU1_WRAPPER_INTR_A2H | VPU1_WRAPPER_INTR_A2HWD | VPU1_CTRL_INIT_IDLE_MSG;

	if (intr_status & mask)
		core->intr_status |= intr_status;

	writel(1, core->reg_base + VPU1_CPU_CS_A2HSOFTINTCLR);
	writel(intr_status, core->reg_base + VPU1_WRAPPER_INTR_CLEAR);
}

static int iris_vpu1_watchdog(struct iris_core *core, u32 intr_status)
{
	if (intr_status & VPU1_WRAPPER_INTR_A2HWD) {
		dev_err(core->dev, "received watchdog interrupt\n");
		return -ETIME;
	}

	return 0;
}

static int iris_vpu1_prepare_pc(struct iris_core *core)
{
	u32 wfi_status, idle_status, pc_ready;
	u32 ctrl_status, val = 0;
	int ret;

	ctrl_status = readl(core->reg_base + VPU1_CTRL_STATUS);
	pc_ready = ctrl_status & VPU1_CTRL_STATUS_PC_READY;
	idle_status = ctrl_status & VPU1_CTRL_INIT_IDLE_MSG;
	if (pc_ready)
		return 0;

	wfi_status = readl(core->reg_base + VPU1_WRAPPER_CPU_STATUS);
	wfi_status &= VPU1_WRAPPER_CPU_STATUS_WFI;
	if (!wfi_status || !idle_status)
		goto skip_power_off;

	ret = core->hfi_sys_ops->sys_pc_prep(core);
	if (ret)
		goto skip_power_off;

	ret = readl_poll_timeout(core->reg_base + VPU1_CTRL_STATUS, val,
				 val & VPU1_CTRL_STATUS_PC_READY, 250, 2500);
	if (ret)
		goto skip_power_off;

	ret = readl_poll_timeout(core->reg_base + VPU1_WRAPPER_CPU_STATUS, val,
				 val & VPU1_WRAPPER_CPU_STATUS_WFI, 250, 2500);
	if (ret)
		goto skip_power_off;

	return 0;

skip_power_off:
	ctrl_status = readl(core->reg_base + VPU1_CTRL_STATUS);
	wfi_status = readl(core->reg_base + VPU1_WRAPPER_CPU_STATUS);
	wfi_status &= VPU1_WRAPPER_CPU_STATUS_WFI;
	dev_err(core->dev, "skip power collapse, wfi=%#x, idle=%#x, pcr=%#x, ctrl=%#x)\n",
		wfi_status, idle_status, pc_ready, ctrl_status);

	return -EAGAIN;
}

const struct vpu_ops iris_vpu1_ops = {
	.power_off_hw = iris_vpu1_power_off_hw,
	.power_on_hw = iris_vpu1_power_on_hw,
	.power_off_controller = iris_vpu1_power_off_controller,
	.power_on_controller = iris_vpu_power_on_controller,
	.calc_freq = iris_vpu2_calc_freq,
	.set_hwmode = iris_vpu1_set_hwmode,
	.set_preset_registers = iris_vpu1_set_preset_registers,
	.interrupt_init = iris_vpu1_interrupt_init,
	.boot_firmware = iris_vpu1_boot_firmware,
	.raise_interrupt = iris_vpu1_raise_interrupt,
	.clear_interrupt = iris_vpu1_clear_interrupt,
	.watchdog = iris_vpu1_watchdog,
	.prepare_pc = iris_vpu1_prepare_pc,
};
