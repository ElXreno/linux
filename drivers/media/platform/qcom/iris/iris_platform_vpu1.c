// SPDX-License-Identifier: GPL-2.0-only

#include "iris_core.h"
#include "iris_ctrls.h"
#include "iris_platform_common.h"
#include "iris_resources.h"
#include "iris_hfi_gen1.h"
#include "iris_hfi_gen1_defines.h"
#include "iris_vpu_buffer.h"
#include "iris_vpu_common.h"
#include "iris_instance.h"

#include "iris_platform_sm7150.h"

static u32 iris_vpu1_buf_size(struct iris_inst *inst, enum iris_buffer_type buffer_type)
{
	if (inst->fw_buf_size[buffer_type])
		return inst->fw_buf_size[buffer_type];

	return iris_vpu_buf_size(inst, buffer_type);
}

static const struct iris_firmware_desc iris_vpu10_p2_gen1_desc = {
	.firmware_data = &iris_hfi_gen1_data,
	.get_vpu_buffer_size = iris_vpu1_buf_size,
	.fwname = "qcom/vpu/vpu10_p2.mbn",
};

static const u32 iris_fmts_vpu1_dec[] = {
	[IRIS_FMT_H264] = V4L2_PIX_FMT_H264,
	[IRIS_FMT_HEVC] = V4L2_PIX_FMT_HEVC,
	[IRIS_FMT_VP9] = V4L2_PIX_FMT_VP9,
	[IRIS_FMT_VP8] = V4L2_PIX_FMT_VP8,
	[IRIS_FMT_MPEG1] = V4L2_PIX_FMT_MPEG1,
};

static const u32 iris_fmts_vpu1_enc[] = {
	[IRIS_FMT_H264] = V4L2_PIX_FMT_H264,
	[IRIS_FMT_HEVC] = V4L2_PIX_FMT_HEVC,
	[IRIS_FMT_VP8] = V4L2_PIX_FMT_VP8,
};

static struct platform_inst_caps platform_inst_cap_vpu1 = {
	.min_frame_width = 96,
	.max_frame_width = 4096,
	.min_frame_height = 96,
	.max_frame_height = 4096,
	.max_mbpf = (4096 * 2304) / 256,
	.mb_cycles_vsp = 10,
	.mb_cycles_vpp = 200,
	.max_frame_rate = MAXIMUM_FPS,
	.max_operating_rate = MAXIMUM_FPS,
};

static const struct icc_info iris_icc_info_vpu1[] = {
	{ "cpu-cfg",    1000, 1000    },
	{ "video-mem",  1000, 6533000 },
};

static const char * const iris_pmdomain_table_vpu1[] = { "venus", "vcodec0", "cvp" };

static const struct tz_cp_config tz_cp_config_vpu1[] = {
	{
		.cp_start = 0,
		.cp_size = 0x25800000,
		.cp_nonpixel_start = 0x01000000,
		.cp_nonpixel_size = 0x24800000,
	},
};

const struct iris_platform_data sm7150_data = {
	.firmware_desc_gen1 = &iris_vpu10_p2_gen1_desc,
	.vpu_ops = &iris_vpu1_ops,
	.icc_tbl = iris_icc_info_vpu1,
	.icc_tbl_size = ARRAY_SIZE(iris_icc_info_vpu1),
	.bw_tbl_dec = sm7150_bw_table_dec,
	.bw_tbl_dec_size = ARRAY_SIZE(sm7150_bw_table_dec),
	.pmdomain_tbl = iris_pmdomain_table_vpu1,
	.pmdomain_tbl_size = ARRAY_SIZE(iris_pmdomain_table_vpu1),
	.opp_pd_tbl = sm7150_opp_pd_table,
	.opp_pd_tbl_size = ARRAY_SIZE(sm7150_opp_pd_table),
	.clk_tbl = sm7150_clk_table,
	.clk_tbl_size = ARRAY_SIZE(sm7150_clk_table),
	.opp_clk_tbl = sm7150_opp_clk_table,
	.dma_mask = 0xe0000000 - 1,
	.inst_iris_fmts = iris_fmts_vpu1_dec,
	.inst_iris_fmts_size = ARRAY_SIZE(iris_fmts_vpu1_dec),
	.inst_iris_fmts_enc = iris_fmts_vpu1_enc,
	.inst_iris_fmts_enc_size = ARRAY_SIZE(iris_fmts_vpu1_enc),
	.inst_caps = &platform_inst_cap_vpu1,
	.tz_cp_config_data = tz_cp_config_vpu1,
	.tz_cp_config_data_size = ARRAY_SIZE(tz_cp_config_vpu1),
	.num_vpp_pipe = 2,
	.no_aon = true,
	.hfi_4xx = true,
	.max_session_count = 16,
	.max_core_mbpf = (4096 * 2304) / 256 * 2,
	.max_core_mbps = ((4096 * 2160) / 256) * 90,
};
