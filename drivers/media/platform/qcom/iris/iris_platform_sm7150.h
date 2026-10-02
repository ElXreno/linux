/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __IRIS_PLATFORM_SM7150_H__
#define __IRIS_PLATFORM_SM7150_H__

static const struct bw_info sm7150_bw_table_dec[] = {
	{ ((4096 * 2160) / 256) * 60, 1896000, },
	{ ((4096 * 2160) / 256) * 30,  968000, },
	{ ((1920 * 1080) / 256) * 60,  618000, },
	{ ((1920 * 1080) / 256) * 30,  318000, },
};

static const char * const sm7150_opp_pd_table[] = { "cx" };

static const struct platform_clk_data sm7150_clk_table[] = {
	{IRIS_AXI_CLK,        "bus"          },
	{IRIS_CTRL_CLK,       "core"         },
	{IRIS_AHB_CLK,        "iface"        },
	{IRIS_HW_CLK,         "vcodec0_core" },
	{IRIS_HW_AHB_CLK,     "vcodec0_bus"  },
	{IRIS_CVP_HW_CLK,     "cvp_core"     },
	{IRIS_CVP_HW_AHB_CLK, "cvp_bus"      },
};

static const char * const sm7150_opp_clk_table[] = {
	"vcodec0_core",
	NULL,
};

#endif
