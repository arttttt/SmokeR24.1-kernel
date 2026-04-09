/*
 * NVIDIA Tegra Video Input Device
 *
 * Copyright (c) 2015-2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author: Bryan Wu <pengw@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/nvhost.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/lcm.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>
#include <media/camera_common.h>
#include <media/tegra_camera_platform.h>

#include <mach/clk.h>
#include <mach/io_dpd.h>

#include "camera/mc_common.h"
#include "vi/vi.h"
#include "nvhost_acm.h"
#include "mipi_cal.h"
#include "isp_t124.h"
#include "t124_registers.h"
#include "t210_registers.h"
#include "vi_capture.h"

#if defined(CONFIG_ARCH_TEGRA_12x_SOC) || defined(CONFIG_ARCH_TEGRA_13x_SOC)
int t124_csi_tpg = 0;
module_param(t124_csi_tpg, int, 0644);
MODULE_PARM_DESC(t124_csi_tpg, "Enable T124 CSI Test Pattern Generator (bypasses sensor MIPI)");

int isp_reprocess = 0;
module_param(isp_reprocess, int, 0644);
MODULE_PARM_DESC(isp_reprocess, "ISP reprocess mode: VI->mem->ISP (1=on, 0=streaming)");

int t124_use_isp = 0;
module_param(t124_use_isp, int, 0644);
MODULE_PARM_DESC(t124_use_isp, "Enable hardware ISP pipeline (default: 0=off, 1=on)");

int t124_single_shot = 1;
module_param(t124_single_shot, int, 0644);
MODULE_PARM_DESC(t124_single_shot, "Single-shot capture (default: 1, no tearing)");
#endif


#define FRAMERATE	120
#define BPP_MEM		2

#if defined(CONFIG_ARCH_TEGRA_12x_SOC) || defined(CONFIG_ARCH_TEGRA_13x_SOC)
unsigned int tegra_channel_get_sizeimage(struct tegra_channel *chan)
{
	if (t124_csi_tpg)
		return chan->format.width * chan->format.height * 4;
	return chan->format.sizeimage;
}
int tegra_channel_get_bytesperline(struct tegra_channel *chan)
{
	if (t124_csi_tpg)
		return chan->format.width * 4;
	return chan->format.bytesperline;
}
#else
unsigned int tegra_channel_get_sizeimage(struct tegra_channel *chan)
{
	return chan->format.sizeimage;
}
int tegra_channel_get_bytesperline(struct tegra_channel *chan)
{
	return chan->format.bytesperline;
}
#endif

extern int _vb2_fop_release(struct file *file, struct mutex *lock);
static void tegra_channel_queued_buf_done(struct tegra_channel *chan,
					  enum vb2_buffer_state state);
static void tegra_channel_stop_kthreads(struct tegra_channel *chan);
static int tegra_channel_set_stream(struct tegra_channel *chan, bool on);
static int tegra_channel_mipi_cal(struct tegra_channel *chan, char is_bypass);

u32 tegra_channel_read(struct tegra_channel *chan,
			unsigned int addr)
{
	return readl(chan->vi->iomem + addr);
}

void tegra_channel_write(struct tegra_channel *chan,
			unsigned int addr, u32 val)
{
	writel(val, chan->vi->iomem + addr);
}

/* CSI registers */
void csi_write(struct tegra_channel *chan, unsigned int index,
			unsigned int addr, u32 val)
{
	writel(val, chan->csibase[index] + addr);
}

u32 csi_read(struct tegra_channel *chan, unsigned int index,
					unsigned int addr)
{
	return readl(chan->csibase[index] + addr);
}

static void gang_buffer_offsets(struct tegra_channel *chan)
{
	int i;
	u32 offset = 0;

	for (i = 0; i < chan->total_ports; i++) {
		switch (chan->gang_mode) {
		case CAMERA_NO_GANG_MODE:
		case CAMERA_GANG_L_R:
		case CAMERA_GANG_R_L:
			offset = chan->gang_bytesperline;
			break;
		case CAMERA_GANG_T_B:
		case CAMERA_GANG_B_T:
			offset = chan->gang_sizeimage;
			break;
		default:
			offset = 0;
		}
		offset = ((offset + TEGRA_SURFACE_ALIGNMENT - 1) &
					~(TEGRA_SURFACE_ALIGNMENT - 1));
		chan->buffer_offset[i] = i * offset;
	}
}

static u32 gang_mode_width(enum camera_gang_mode gang_mode,
					unsigned int width)
{
	if ((gang_mode == CAMERA_GANG_L_R) ||
		(gang_mode == CAMERA_GANG_R_L))
		return width >> 1;
	else
		return width;
}

static u32 gang_mode_height(enum camera_gang_mode gang_mode,
					unsigned int height)
{
	if ((gang_mode == CAMERA_GANG_T_B) ||
		(gang_mode == CAMERA_GANG_B_T))
		return height >> 1;
	else
		return height;
}

static void update_gang_mode_params(struct tegra_channel *chan)
{
	chan->gang_width = gang_mode_width(chan->gang_mode,
						chan->format.width);
	chan->gang_height = gang_mode_height(chan->gang_mode,
						chan->format.height);
	chan->gang_bytesperline = chan->gang_width *
					chan->fmtinfo->bpp;
	chan->gang_sizeimage = chan->gang_bytesperline *
					chan->format.height;
	gang_buffer_offsets(chan);
}

static void update_gang_mode(struct tegra_channel *chan)
{
	int width = chan->format.width;
	int height = chan->format.height;

	/*
	 * At present only 720p, 1080p and 4k resolutions
	 * are supported and only 4K requires gang mode
	 * Update this code with CID for future extensions
	 * Also, validate width and height of images based
	 * on gang mode and surface stride alignment
	 */
	if ((width > 1920) && (height > 1080)) {
		chan->gang_mode = CAMERA_GANG_L_R;
		chan->valid_ports = chan->total_ports;
	} else {
		chan->gang_mode = CAMERA_NO_GANG_MODE;
		chan->valid_ports = 1;
	}

	update_gang_mode_params(chan);
}

static void tegra_channel_fmts_bitmap_init(struct tegra_channel *chan)
{
	int ret, pixel_format_index = 0, init_code = 0;
	struct v4l2_subdev *subdev = chan->subdev_on_csi;
	struct v4l2_mbus_framefmt mbus_fmt;
	struct v4l2_subdev_mbus_code_enum code = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};

	bitmap_zero(chan->fmts_bitmap, MAX_FORMAT_NUM);

	/*
	 * Initialize all the formats available from
	 * the sub-device and extract the corresponding
	 * index from the pre-defined video formats and initialize
	 * the channel default format with the active code
	 * Index zero as the only sub-device is sensor
	 */
	while (1) {
		ret = v4l2_subdev_call(subdev, pad, enum_mbus_code,
				       NULL, &code);
		if (ret < 0)
			/* no more formats */
			break;

		pixel_format_index = tegra_core_get_idx_by_code(code.code);
		if (pixel_format_index >= 0) {
			bitmap_set(chan->fmts_bitmap, pixel_format_index, 1);
			if (!init_code)
				init_code = code.code;
		}

		code.index++;
	}

	if (!init_code) {
		pixel_format_index = tegra_core_get_idx_by_code(TEGRA_VF_DEF);
		if (pixel_format_index >= 0) {
			bitmap_set(chan->fmts_bitmap, pixel_format_index, 1);
			init_code = TEGRA_VF_DEF;
		}
	}

	/* Get the format based on active code of the sub-device */
	ret = v4l2_subdev_call(subdev, video, g_mbus_fmt, &mbus_fmt);
	if (ret)
		return;

	chan->fmtinfo = tegra_core_get_format_by_code(mbus_fmt.code);
	chan->format.pixelformat = chan->fmtinfo->fourcc;
	chan->format.colorspace = mbus_fmt.colorspace;
	chan->format.field = mbus_fmt.field;
	chan->format.width = mbus_fmt.width;
	chan->format.height = mbus_fmt.height;
	chan->format.bytesperline = chan->format.width *
		chan->fmtinfo->bpp;
	chan->format.sizeimage = chan->format.bytesperline *
		chan->format.height;
	if (chan->total_ports > 1)
		update_gang_mode(chan);
}

/*
 * -----------------------------------------------------------------------------
 * Tegra channel frame setup and capture operations
 * -----------------------------------------------------------------------------
 */

int tegra_channel_capture_setup(struct tegra_channel *chan)
{
	u32 height = chan->format.height;
	u32 width = chan->format.width;
	u32 format = chan->fmtinfo->img_fmt;
	u32 data_type = chan->fmtinfo->img_dt;
	u32 word_count = tegra_core_get_word_count(width, chan->fmtinfo);
	u32 bypass_pixel_transform = 1;
	int index;

	if (chan->valid_ports > 1) {
		height = chan->gang_height;
		width = chan->gang_width;
		word_count = tegra_core_get_word_count(width, chan->fmtinfo);
	}

	if (chan->vi->pg_mode ||
	   chan->use_isp ||
	   (chan->fmtinfo->vf_code == TEGRA_VF_YUV422) ||
	   (chan->fmtinfo->vf_code == TEGRA_VF_RGB888))
		bypass_pixel_transform = 0;

	for (index = 0; index < chan->valid_ports; index++) {
		csi_write(chan, index, TEGRA_VI_CSI_ERROR_STATUS, 0xFFFFFFFF);
		csi_write(chan, index, TEGRA_VI_CSI_IMAGE_DEF,
		  (bypass_pixel_transform << BYPASS_PXL_TRANSFORM_OFFSET) |
		  (format << IMAGE_DEF_FORMAT_OFFSET));
		csi_write(chan, index, TEGRA_VI_CSI_IMAGE_DT, data_type);
		csi_write(chan, index, TEGRA_VI_CSI_IMAGE_SIZE_WC, word_count);
		csi_write(chan, index, TEGRA_VI_CSI_IMAGE_SIZE,
			  (height << IMAGE_SIZE_HEIGHT_OFFSET) | width);
		dev_dbg(&chan->video.dev,
			 "capture_setup[%d]: port=%d %ux%u fmt=0x%x dt=0x%x wc=%u bypass=%u\n",
			 index, chan->port[index], width, height,
			 format, data_type, word_count, bypass_pixel_transform);
	}

	return 0;
}

int tegra_channel_enable_stream(struct tegra_channel *chan)
{
	int ret = 0, i;

	/*
	 * enable pad power and perform calibration before arming
	 * single shot for first frame after the HW setup is complete
	 */
	/* enable pad power */
	tegra_csi_pad_control(chan->vi->csi, chan->port, ENABLE);
	/* start streaming */
	if (chan->vi->pg_mode) {
		for (i = 0; i < chan->valid_ports; i++)
			tegra_csi_tpg_start_streaming(chan->vi->csi,
						      chan->port[i]);
		atomic_set(&chan->is_streaming, ENABLE);
	} else {
#if defined(CONFIG_ARCH_TEGRA_12x_SOC) || defined(CONFIG_ARCH_TEGRA_13x_SOC)
		if (!t124_csi_tpg) {
#endif
		ret = tegra_channel_set_stream(chan, true);
		if (ret < 0)
			return ret;
#if defined(CONFIG_ARCH_TEGRA_12x_SOC) || defined(CONFIG_ARCH_TEGRA_13x_SOC)
		} else {
			dev_dbg(&chan->video.dev,
				 "T124 CSI TPG: skipping sensor s_stream\n");
		}
#endif
	}

	#if defined(CONFIG_ARCH_TEGRA_12x_SOC) || defined(CONFIG_ARCH_TEGRA_13x_SOC)
	/*
	 * T124: CSI PHY init AFTER sensor s_stream(1) — matches R21.5 vi2 order.
	 * In R21.5, soc_camera calls s_stream during STREAMON, then capture_setup
	 * programs CSI registers on first frame. The sensor must already be outputting
	 * MIPI LP signals before CIL_COMMAND enables the PHY.
	 *
	 * Port-aware: supports PORT_A (CSI-A/B, 4-lane) and PORT_B (CSI-C/D/E, 1-lane)
	 */
	if (!chan->vi->pg_mode) {
		u32 val;
		int width = chan->format.width;
		int height = chan->format.height;
		u32 format = chan->fmtinfo->img_fmt;
		u32 data_type = chan->fmtinfo->img_dt;
		u32 word_count = tegra_core_get_word_count(width,
							   chan->fmtinfo);
		int port_num = chan->port[0];	/* Primary port for this channel */
		u32 pp_cmd_reg, pp_int_mask_reg, pp_status_reg;
		u32 stream_ctrl0_reg, stream_ctrl1_reg, stream_gap_reg;
		u32 stream_expected_reg, input_stream_reg;
		u32 vi_csi_base;
		u32 pg_ctrl_reg;
		u32 cil_pad_cfg0, cil_pad_cfg1, cil_pad_cfg2;
		u32 phy_cil_control0, phy_cil_control1;
		u32 cil_int_mask0, cil_int_mask1;
		const char *port_name;

		/* Select register bases based on port */
		if (port_num == 0) {
			/* PORT_A: CSI-A (4-lane), PP_A, VI_CSI_0 */
			pp_cmd_reg = T124_PP_A_PIXEL_STREAM_PP_COMMAND;
			pp_int_mask_reg = T124_PP_A_PIXEL_STREAM_PP_INT_MASK;
			pp_status_reg = T124_PP_A_PIXEL_PARSER_STATUS;
			stream_ctrl0_reg = T124_PP_A_PIXEL_STREAM_CONTROL0;
			stream_ctrl1_reg = T124_PP_A_PIXEL_STREAM_CONTROL1;
			stream_gap_reg = T124_PP_A_PIXEL_STREAM_GAP;
			stream_expected_reg = T124_PP_A_PIXEL_STREAM_EXPECTED_FRAME;
			input_stream_reg = T124_PP_A_INPUT_STREAM_CONTROL;
			vi_csi_base = TEGRA_VI_CSI_BASE(0);
			pg_ctrl_reg = T124_CSI_PG_CTRL_A;
			cil_pad_cfg0 = T124_CILA_PAD_CONFIG0;
			cil_pad_cfg1 = T124_CILB_PAD_CONFIG0;
			cil_pad_cfg2 = 0;
			phy_cil_control0 = T124_PHY_CILA_CONTROL0;
			phy_cil_control1 = T124_PHY_CILB_CONTROL0;
			cil_int_mask0 = T124_CSI_CIL_A_INT_MASK;
			cil_int_mask1 = T124_CSI_CIL_B_INT_MASK;
			port_name = "CSI_A";
		} else {
			/* PORT_B: CSI-C (1-lane), PP_B, VI_CSI_1 */
			pp_cmd_reg = T124_PP_B_PIXEL_STREAM_PP_COMMAND;
			pp_int_mask_reg = T124_PP_B_PIXEL_STREAM_PP_INT_MASK;
			pp_status_reg = T124_PP_B_PIXEL_PARSER_STATUS;
			stream_ctrl0_reg = T124_PP_B_PIXEL_STREAM_CONTROL0;
			stream_ctrl1_reg = T124_PP_B_PIXEL_STREAM_CONTROL1;
			stream_gap_reg = T124_PP_B_PIXEL_STREAM_GAP;
			stream_expected_reg = T124_PP_B_PIXEL_STREAM_EXPECTED_FRAME;
			input_stream_reg = T124_PP_B_INPUT_STREAM_CONTROL;
			vi_csi_base = TEGRA_VI_CSI_BASE(1);
			pg_ctrl_reg = T124_CSI_PG_CTRL_B;
			cil_pad_cfg0 = T124_CILC_PAD_CONFIG0;
			cil_pad_cfg1 = T124_CILD_PAD_CONFIG0;
			cil_pad_cfg2 = T124_CILE_PAD_CONFIG0;
			phy_cil_control0 = T124_PHY_CILC_CONTROL0;
			phy_cil_control1 = T124_PHY_CILD_CONTROL0;
			cil_int_mask0 = T124_CSI_CIL_C_INT_MASK;
			cil_int_mask1 = T124_CSI_CIL_D_INT_MASK;
			port_name = "CSI_C";
		}

		/*
		 * Clear all CIL and PP status registers.
		 * Always clear all for safety, regardless of port.
		 */
		tegra_channel_write(chan, T124_CSI_CIL_A_STATUS, 0xFFFFFFFF);
		tegra_channel_write(chan, T124_CSI_CIL_B_STATUS, 0xFFFFFFFF);
		tegra_channel_write(chan, T124_CSI_CIL_C_STATUS, 0xFFFFFFFF);
		tegra_channel_write(chan, T124_CSI_CIL_D_STATUS, 0xFFFFFFFF);
		tegra_channel_write(chan, T124_CSI_CIL_E_STATUS, 0xFFFFFFFF);
		tegra_channel_write(chan, T124_CSI_CILA_STATUS, 0xFFFFFFFF);
		tegra_channel_write(chan, T124_CSI_CILB_STATUS, 0xFFFFFFFF);
		tegra_channel_write(chan, T124_CSI_CILC_STATUS, 0xFFFFFFFF);
		tegra_channel_write(chan, T124_CSI_CILD_STATUS, 0xFFFFFFFF);
		tegra_channel_write(chan, T124_PP_A_PIXEL_PARSER_STATUS, 0xFFFFFFFF);
		tegra_channel_write(chan, T124_PP_B_PIXEL_PARSER_STATUS, 0xFFFFFFFF);
		tegra_channel_write(chan, TEGRA_VI_CSI_BASE(0) + TEGRA_VI_CSI_ERROR_STATUS, 0xFFFFFFFF);
		tegra_channel_write(chan, TEGRA_VI_CSI_BASE(1) + TEGRA_VI_CSI_ERROR_STATUS, 0xFFFFFFFF);

		/* CIL pad config — port specific */
		if (port_num == 0) {
			/* PORT_A: CILA = CD_BK_MODE, CILB = 0 */
			tegra_channel_write(chan, cil_pad_cfg0, 0x10000);  /* CILA_PAD_CONFIG0 */
			tegra_channel_write(chan, cil_pad_cfg1, 0x0);      /* CILB_PAD_CONFIG0 */
		} else {
			/* PORT_B: CILC = CD_BK_MODE, CILD = 0, CILE = 0 */
			tegra_channel_write(chan, cil_pad_cfg0, 0x10000);  /* CILC_PAD_CONFIG0 */
			tegra_channel_write(chan, cil_pad_cfg1, 0x0);      /* CILD_PAD_CONFIG0 */
			tegra_channel_write(chan, cil_pad_cfg2, 0x0);      /* CILE_PAD_CONFIG0 */
		}

		/* CIL interrupt masks */
		tegra_channel_write(chan, cil_int_mask0, 0x0);
		tegra_channel_write(chan, cil_int_mask1, 0x0);
		if (port_num != 0)
			tegra_channel_write(chan, 0xa14, 0x0);  /* CIL_E_INT_MASK for port 1 */

		/* PHY control — R21.5 stock value (THS=9) */
		if (port_num == 0) {
			tegra_channel_write(chan, phy_cil_control0, 0x09);  /* PHY_CILA_CONTROL0 */
			tegra_channel_write(chan, phy_cil_control1, 0x09);  /* PHY_CILB_CONTROL0 */
		} else {
			tegra_channel_write(chan, 0xa10, 0x09);  /* PHY_CILE_CONTROL0 for port 1 */
		}

		/* Pixel Parser setup: RST to clear state */
		tegra_channel_write(chan, pp_cmd_reg,
			t124_single_shot ? 0xf007 : 0xf003);
		tegra_channel_write(chan, pp_int_mask_reg, 0x0); /* PP_INT_MASK = 0 */
		if (port_num == 0)
			tegra_channel_write(chan, stream_ctrl0_reg, 0x280301f0); /* STREAM_A_CONTROL0 (4-lane) */
		else
			tegra_channel_write(chan, stream_ctrl0_reg, 0x280301f1); /* STREAM_B_CONTROL0 (1-lane) */
		tegra_channel_write(chan, pp_cmd_reg,
			t124_single_shot ? 0xf005 : 0xf001); /* ENABLE (+SS if single_shot) */
		tegra_channel_write(chan, stream_ctrl1_reg, 0x11); /* STREAM_CONTROL1 */
		tegra_channel_write(chan, stream_gap_reg, 0x140000); /* STREAM_GAP */
		tegra_channel_write(chan, stream_expected_reg, 0x0); /* STREAM_EXPECTED_FRAME */
		if (port_num == 0)
			tegra_channel_write(chan, input_stream_reg, 0x3f0000 | 0x3); /* INPUT_STREAM_A_CONTROL (4-lane) */
		else
			tegra_channel_write(chan, input_stream_reg, 0x3f0000); /* INPUT_STREAM_B_CONTROL (1-lane) */

		/* PHY CIL command — port and lane specific */
		val = tegra_channel_read(chan, T124_CSI_PHY_CIL_COMMAND);
		if (port_num == 0) {
			/* PORT_A: 4-lane, enable CILA + CILB */
			tegra_channel_write(chan, T124_CSI_PHY_CIL_COMMAND,
			       (val & T124_CIL_CMD_HI_MASK) | T124_CIL_AB_4LANE);
		} else {
			/* PORT_B: 1-lane, enable CILE */
			tegra_channel_write(chan, T124_CSI_PHY_CIL_COMMAND,
			       (val & T124_CIL_CMD_LO_MASK) | T124_CIL_C_1LANE);
		}

		if (t124_csi_tpg) {
			/*
			 * CSI TPG clock setup — from R21.5 vi2_clks_enable().
			 * TPG needs PLL_D as active clock source, not just a toggle.
			 */
			struct clk *tpg_clk = clk_get(chan->vi->dev, "pll_d");
			if (!IS_ERR(tpg_clk)) {
				clk_prepare_enable(tpg_clk);
				tegra_clk_cfg_ex(tpg_clk,
						 TEGRA_CLK_PLLD_CSI_OUT_ENB, 1);
				tegra_clk_cfg_ex(tpg_clk,
						 TEGRA_CLK_PLLD_DSI_OUT_ENB, 1);
				tegra_clk_cfg_ex(tpg_clk,
						 TEGRA_CLK_MIPI_CSI_OUT_ENB, 0);
				/* Keep tpg_clk enabled — don't disable */
				dev_dbg(&chan->video.dev,
					 "T124 TPG: PLL_D held ON (CSI=1 DSI=1 MIPI_CSI=0)\n");
				/* Note: clk_put without disable to keep it running */
				clk_put(tpg_clk);
			}

			/*
			 * CSI TPG mode: generate test pattern at PP level.
			 * Register offsets from R21.5 vi2.c (absolute from VI base).
			 */
			tegra_channel_write(chan, pg_ctrl_reg,
			       (PG_DISABLE << PG_MODE_OFFSET) | PG_ENABLE);
			tegra_channel_write(chan, pg_ctrl_reg + 0x08, 0x0);  /* PG_PHASE */
			tegra_channel_write(chan, pg_ctrl_reg + 0x0c, 0x100010); /* PG_RED_FREQ */
			tegra_channel_write(chan, pg_ctrl_reg + 0x10, 0x0);  /* PG_RED_FREQ_RATE */
			tegra_channel_write(chan, pg_ctrl_reg + 0x14, 0x100010); /* PG_GREEN_FREQ */
			tegra_channel_write(chan, pg_ctrl_reg + 0x18, 0x0);  /* PG_GREEN_FREQ_RATE */
			tegra_channel_write(chan, pg_ctrl_reg + 0x1c, 0x100010); /* PG_BLUE_FREQ */
			tegra_channel_write(chan, pg_ctrl_reg + 0x20, 0x0);  /* PG_BLUE_FREQ_RATE */
			/* Override CIL_COMMAND for TPG */
			tegra_channel_write(chan, T124_CSI_PHY_CIL_COMMAND, T124_CIL_ALL_ENABLE);

			/* TPG uses RGB888 format */
			format = 64;	/* TEGRA_IMAGE_FORMAT_T_A8B8G8R8 = 0x40 */
			data_type = 0x24; /* TEGRA_IMAGE_DT_RGB888 */
			word_count = width * 3;

			dev_dbg(&chan->video.dev,
				 "T124 CSI TPG ENABLED: %dx%d RGB888 wc=%d\n",
				 width, height, word_count);
		}

		/* VI CSI image config — port specific base */
		{
			u32 dest;
			if (chan->use_isp && !isp_reprocess)
				dest = (chan->port[0] == 0) ?
					IMAGE_DEF_DEST_ISP_A :
					IMAGE_DEF_DEST_ISP_B;
			else
				dest = IMAGE_DEF_DEST_MEM;
			tegra_channel_write(chan,
				vi_csi_base + TEGRA_VI_CSI_IMAGE_DEF,
				(t124_csi_tpg ? 0 :
					(1 << BYPASS_PXL_TRANSFORM_OFFSET)) |
				(format << IMAGE_DEF_FORMAT_OFFSET) | dest);
		}
		/* Enable VI→ISP interface if ISP streaming (not reprocess) */
		if (chan->use_isp && !isp_reprocess)
			tegra_channel_write(chan,
				vi_csi_base + TEGRA_VI_CSI_ISPINTF_CONFIG,
				ISPINTF_CONFIG_ENABLE);
		tegra_channel_write(chan, vi_csi_base + TEGRA_VI_CSI_IMAGE_DT,
		       data_type);
		tegra_channel_write(chan, vi_csi_base + TEGRA_VI_CSI_IMAGE_SIZE_WC,
		       word_count);
		tegra_channel_write(chan, vi_csi_base + TEGRA_VI_CSI_IMAGE_SIZE,
		       (height << IMAGE_SIZE_HEIGHT_OFFSET) | width);

		/* Enable pixel parser: continuous or single-shot.
		 * In single-shot mode, PP waits for VI_CSI_SINGLE_SHOT
		 * trigger per frame (no PP disable/re-enable needed). */
		tegra_channel_write(chan, pp_cmd_reg,
		       (0xF << CSI_PP_START_MARKER_FRAME_MAX_OFFSET) |
		       (t124_single_shot ? CSI_PP_SINGLE_SHOT_ENABLE : 0) |
		       CSI_PP_ENABLE);

		dev_dbg(&chan->video.dev,
			 "T124 %s init (post-stream): %dx%d fmt=0x%x dt=0x%x wc=%d CIL_CMD=0x%08x\n",
			 port_name, width, height, format, data_type, word_count,
			 tegra_channel_read(chan, T124_CSI_PHY_CIL_COMMAND));
	}
#endif

	/* perform calibration as sensor started streaming (skip for TPG) */
#if defined(CONFIG_ARCH_TEGRA_12x_SOC) || defined(CONFIG_ARCH_TEGRA_13x_SOC)
	if (!chan->vi->pg_mode && !t124_csi_tpg) {
#else
	if (!chan->vi->pg_mode) {
#endif
		tegra_mipi_bias_pad_enable();
		mutex_lock(&chan->vi->mipical_lock);
		tegra_channel_mipi_cal(chan, 0);
		mutex_unlock(&chan->vi->mipical_lock);
	}

	return ret;
}

int tegra_channel_error_status(struct tegra_channel *chan)
{
	u32 val;
	int err = 0;
	int index = 0;

	for (index = 0; index < chan->valid_ports; index++) {
		val = csi_read(chan, index, TEGRA_VI_CSI_ERROR_STATUS);
		csi_write(chan, index, TEGRA_VI_CSI_ERROR_STATUS, val);
		err |= val;
		err |= tegra_csi_error(chan->vi->csi, chan->port[index]);
	}

	if (err)
		dev_err(chan->vi->dev, "%s:error %x frame %d\n",
				__func__, err, chan->sequence);
	return err;
}

static void tegra_channel_capture_error(struct tegra_channel *chan)
{
	u32 val;
	int index = 0;

	for (index = 0; index < chan->valid_ports; index++) {
		val = csi_read(chan, index, TEGRA_VI_CSI_ERROR_STATUS);
		dev_dbg(&chan->video.dev,
			"TEGRA_VI_CSI_ERROR_STATUS 0x%08x\n", val);
		tegra_csi_status(chan->vi->csi, chan->port[index]);
	}
}

static void tegra_channel_ec_init(struct tegra_channel *chan)
{
	chan->timeout = 200;
	tegra_channel_write(chan, TEGRA_VI_CFG_VI_INCR_SYNCPT_CNTRL, 0x100);
}

static void tegra_channel_clear_singleshot(struct tegra_channel *chan,
					   int index)
{
	csi_write(chan, index, TEGRA_VI_CSI_SW_RESET, 0xF);
	csi_write(chan, index, TEGRA_VI_CSI_SW_RESET, 0x0);
}

static void tegra_channel_vi_csi_recover(struct tegra_channel *chan)
{
	u32 error_val = tegra_channel_read(chan,
					TEGRA_VI_CFG_VI_INCR_SYNCPT_ERROR);
	u32 frame_start;
	int index, valid_ports = chan->valid_ports;

#if defined(CONFIG_ARCH_TEGRA_12x_SOC) || defined(CONFIG_ARCH_TEGRA_13x_SOC)
	for (index = 0; index < valid_ports; index++) {
		frame_start = (chan->port[index] == 0) ?
			T124_PPA_FRAME_START : T124_PPB_FRAME_START;
		if (error_val & frame_start)
			chan->syncpoint_fifo[index] = SYNCPT_FIFO_DEPTH;
	}
	tegra_channel_write(chan,
		TEGRA_VI_CFG_VI_INCR_SYNCPT_ERROR, error_val);
	for (index = 0; index < valid_ports; index++) {
		nvhost_syncpt_set_min_eq_max_ext(chan->vi->ndev,
						chan->syncpt[index]);
		nvhost_syncpt_set_min_eq_max_ext(chan->vi->ndev,
						chan->syncpt_mw[index]);
	}
	dev_info(chan->vi->dev, "T124 ec_recover: cleared syncpt errors\n");
	return;
#endif

	tegra_csi_pad_control(chan->vi->csi, chan->port, DISABLE);
	tegra_channel_write(chan, TEGRA_VI_CFG_CG_CTRL, DISABLE);
	for (index = 0; index < valid_ports; index++) {
		tegra_csi_error_recover(chan->vi->csi, chan->port[index]);
		csi_write(chan, index, TEGRA_VI_CSI_IMAGE_DEF, 0);
		tegra_channel_clear_singleshot(chan, index);
	}

	for (index = 0; index < valid_ports; index++) {
		frame_start = T210_VI_CSI_PP_FRAME_START(chan->port[index]);
		if (error_val & frame_start)
			chan->syncpoint_fifo[index] = SYNCPT_FIFO_DEPTH;
	}
	tegra_channel_write(chan,
		TEGRA_VI_CFG_VI_INCR_SYNCPT_ERROR, error_val);

	tegra_channel_write(chan, TEGRA_VI_CFG_CG_CTRL, ENABLE);

	tegra_channel_capture_setup(chan);
	for (index = 0; index < valid_ports; index++) {
		tegra_csi_stop_streaming(chan->vi->csi, chan->port[index]);
		tegra_csi_start_streaming(chan->vi->csi, chan->port[index]);
		nvhost_syncpt_set_min_eq_max_ext(chan->vi->ndev,
						chan->syncpt[index]);
	}
}

void tegra_channel_ec_recover(struct tegra_channel *chan)
{
	tegra_channel_capture_error(chan);
	tegra_channel_vi_csi_recover(chan);
}

/*
 * Capture implementations moved to:
 *   t124_capture.c       — T124 continuous mode, 2-kthread pipeline
 *   singleshot_capture.c — legacy single-shot, ring buffer
 *
 * Shared helpers (non-static): enable_stream, error_status, ec_recover,
 * capture_setup, csi_write/read, tegra_channel_write/read, get_sizeimage,
 * get_bytesperline. Prototypes in vi_capture.h.
 */

struct tegra_channel_buffer *dequeue_buffer_simple(struct tegra_channel *chan)
{
	struct tegra_channel_buffer *buf = NULL;

	spin_lock(&chan->start_lock);
	if (list_empty(&chan->capture))
		goto done;

	buf = list_entry(chan->capture.next,
			 struct tegra_channel_buffer, queue);
	list_del_init(&buf->queue);
done:
	spin_unlock(&chan->start_lock);
	return buf;
}


static int tegra_channel_kthread_capture_start(void *data)
{
	struct tegra_channel *chan = data;
	struct tegra_channel_buffer *buf;
	struct sched_param param = { .sched_priority = 1 };
	int err = 0;

	sched_setscheduler(current, SCHED_FIFO, &param);
	set_freezable();

	while (1) {

		try_to_freeze();

		/* Use pre-queued buffer if available (from prequeue_next_surface),
		 * otherwise wait for userspace to QBUF */
		if (chan->next_buf) {
			buf = chan->next_buf;
			chan->next_buf = NULL;
		} else {
			wait_event_interruptible(chan->start_wait,
						 !list_empty(&chan->capture) ||
						 kthread_should_stop());

			if (kthread_should_stop()) {
				complete(&chan->capture_comp);
				break;
			}

			if (err)
				continue;

			buf = dequeue_buffer_simple(chan);
			if (!buf)
				continue;
		}

		err = chan->capture_ops->capture_start(chan, buf);
	}

	return 0;
}

static void tegra_channel_stop_kthreads(struct tegra_channel *chan)
{
	mutex_lock(&chan->stop_kthread_lock);
	if (chan->kthread_capture_start) {
		kthread_stop(chan->kthread_capture_start);
		wait_for_completion(&chan->capture_comp);
		chan->kthread_capture_start = NULL;
	}
	if (chan->kthread_capture_done) {
		kthread_stop(chan->kthread_capture_done);
		wait_for_completion(&chan->capture_done_comp);
		chan->kthread_capture_done = NULL;
	}
	mutex_unlock(&chan->stop_kthread_lock);
}

/*
 * -----------------------------------------------------------------------------
 * videobuf2 queue operations
 * -----------------------------------------------------------------------------
 */
static int
tegra_channel_queue_setup(struct vb2_queue *vq, const struct v4l2_format *fmt,
		     unsigned int *nbuffers, unsigned int *nplanes,
		     unsigned int sizes[], void *alloc_ctxs[])
{
	struct tegra_channel *chan = vb2_get_drv_priv(vq);
	unsigned int sizeimage = tegra_channel_get_sizeimage(chan);

	/* Make sure the image size is large enough. */
	if (fmt && fmt->fmt.pix.sizeimage < sizeimage)
		return -EINVAL;

	*nplanes = 1;

	sizes[0] = fmt ? fmt->fmt.pix.sizeimage : sizeimage;
	alloc_ctxs[0] = chan->alloc_ctx;

	/* Make sure minimum number of buffers are passed */
#if defined(CONFIG_ARCH_TEGRA_12x_SOC) || defined(CONFIG_ARCH_TEGRA_13x_SOC)
	if (!t124_csi_tpg && *nbuffers < (QUEUED_BUFFERS - 1))
#else
	if (*nbuffers < (QUEUED_BUFFERS - 1))
#endif
		*nbuffers = QUEUED_BUFFERS - 1;

	return 0;
}

static int tegra_channel_buffer_prepare(struct vb2_buffer *vb)
{
	struct tegra_channel *chan = vb2_get_drv_priv(vb->vb2_queue);
	struct tegra_channel_buffer *buf = to_tegra_channel_buffer(vb);

	buf->chan = chan;
	vb2_set_plane_payload(vb, 0, tegra_channel_get_sizeimage(chan));
	buf->addr = vb2_dma_contig_plane_dma_addr(vb, 0);

	return 0;
}

static void tegra_channel_buffer_queue(struct vb2_buffer *vb)
{
	struct tegra_channel *chan = vb2_get_drv_priv(vb->vb2_queue);
	struct tegra_channel_buffer *buf = to_tegra_channel_buffer(vb);

	/* for bypass mode - do nothing */
	if (chan->bypass)
		return;

	/* Put buffer into the capture queue */
	spin_lock(&chan->start_lock);
	list_add_tail(&buf->queue, &chan->capture);
	spin_unlock(&chan->start_lock);

	/* Wait up kthread for capture */
	wake_up_interruptible(&chan->start_wait);
}

/* Return all queued buffers back to videobuf2 */
static void tegra_channel_queued_buf_done(struct tegra_channel *chan,
					  enum vb2_buffer_state state)
{
	struct tegra_channel_buffer *buf, *nbuf;
	spinlock_t *lock = &chan->start_lock;
	struct list_head *q = &chan->capture;

	spin_lock(lock);
	list_for_each_entry_safe(buf, nbuf, q, queue) {
		vb2_buffer_done(&buf->buf, state);
		list_del(&buf->queue);
	}
	spin_unlock(lock);
}

#if defined(CONFIG_ARCH_TEGRA_21x_SOC)
static int tegra_channel_mipi_cal(struct tegra_channel *chan, char is_bypass)
{
	unsigned int lanes, cur_lanes, val;
	unsigned int csi_phya, csi_phyb, csi_phya_mask, csi_phyb_mask;
	struct tegra_mc_vi *vi = chan->vi;
	int j;

	lanes = 0;
	csi_phya = 0x1 << CSI_A_PHY_CIL_ENABLE_SHIFT;
	csi_phya_mask = 0x3 << CSI_A_PHY_CIL_ENABLE_SHIFT;
	csi_phyb = 0x1 << CSI_B_PHY_CIL_ENABLE_SHIFT;
	csi_phyb_mask = 0x3 << CSI_B_PHY_CIL_ENABLE_SHIFT;
	if (chan->numlanes == 2 && chan->total_ports == 1) {
		switch (chan->port[0]) {
		case PORT_A:
			lanes = CSIA;
			val = (host1x_readl(vi->ndev, CSI_PHY_CIL_COMMAND_0) &
				(~csi_phya_mask)) | csi_phya;
			if (is_bypass)
				host1x_writel(vi->ndev, CSI_PHY_CIL_COMMAND_0,
						val);
			break;
		case PORT_B:
			lanes = CSIB;
			val = (host1x_readl(vi->ndev, CSI_PHY_CIL_COMMAND_0) &
				(~csi_phyb_mask)) | csi_phyb;
			if (is_bypass)
				host1x_writel(vi->ndev, CSI_PHY_CIL_COMMAND_0,
						val);
			break;
		case PORT_C:
			lanes = CSIC;
			val = (host1x_readl(vi->ndev, CSI1_PHY_CIL_COMMAND_0) &
				(~csi_phya_mask)) | csi_phya;
			if (is_bypass)
				host1x_writel(vi->ndev, CSI1_PHY_CIL_COMMAND_0,
						val);
			break;
		case PORT_D:
			lanes = CSID;
			val = (host1x_readl(vi->ndev, CSI1_PHY_CIL_COMMAND_0) &
			      (~csi_phyb_mask)) | csi_phyb;
			if (is_bypass)
				host1x_writel(vi->ndev, CSI1_PHY_CIL_COMMAND_0,
						val);
			break;
		case PORT_E:
			lanes = CSIE;
			val = (host1x_readl(vi->ndev, CSI2_PHY_CIL_COMMAND_0) &
				(~csi_phya_mask)) | csi_phya;
			if (is_bypass)
				host1x_writel(vi->ndev, CSI2_PHY_CIL_COMMAND_0,
						val);
			break;
		case PORT_F:
			lanes = CSIF;
			val = (host1x_readl(vi->ndev, CSI2_PHY_CIL_COMMAND_0) &
				(~csi_phyb_mask)) | csi_phyb;
			if (is_bypass)
				host1x_writel(vi->ndev, CSI2_PHY_CIL_COMMAND_0,
						val);
			break;
		default:
			dev_err(vi->dev, "csi_port number: %d", chan->port[0]);
			break;
		}
	} else if (chan->numlanes == 4 && chan->total_ports == 1) {
		switch (chan->port[0]) {
		case PORT_A:
		case PORT_B:
			lanes = CSIA|CSIB;
			if (is_bypass)
				host1x_writel(vi->ndev, CSI_PHY_CIL_COMMAND_0,
					csi_phya | csi_phyb);
			break;
		case PORT_C:
		case PORT_D:
			lanes = CSIC|CSID;
			if (is_bypass)
				host1x_writel(vi->ndev, CSI1_PHY_CIL_COMMAND_0,
					csi_phya | csi_phyb);
			break;
		case PORT_E:
		case PORT_F:
			lanes = CSIE|CSIF;
			if (is_bypass)
				host1x_writel(vi->ndev, CSI2_PHY_CIL_COMMAND_0,
					csi_phya | csi_phyb);
			break;
		default:
			dev_err(vi->dev, "csi_port number: %d", chan->port[0]);
			break;
		}
	} else if (chan->numlanes == 8) {
		cur_lanes = 0;
		for (j = 0; j < chan->valid_ports; ++j) {
			switch (chan->port[j]) {
			case PORT_A:
			case PORT_B:
				cur_lanes = CSIA|CSIB;
				if (is_bypass)
					host1x_writel(vi->ndev,
							CSI_PHY_CIL_COMMAND_0,
							csi_phya | csi_phyb);
				break;
			case PORT_C:
			case PORT_D:
				cur_lanes = CSIC|CSID;
				if (is_bypass)
					host1x_writel(vi->ndev,
							CSI1_PHY_CIL_COMMAND_0,
							csi_phya | csi_phyb);
				break;
			case PORT_E:
			case PORT_F:
				cur_lanes = CSIE|CSIF;
				if (is_bypass)
					host1x_writel(vi->ndev,
							CSI2_PHY_CIL_COMMAND_0,
							csi_phya | csi_phyb);
				break;
			default:
				dev_err(vi->dev, "csi_port number: %d",
						chan->port[0]);
				break;
			}
			lanes |= cur_lanes;
		}
	}
	if (!lanes) {
		dev_err(vi->dev, "Selected no CSI lane, cannot do calibration");
		return -EINVAL;
	}
	return tegra_mipi_calibration(lanes);

}
#elif defined(CONFIG_ARCH_TEGRA_12x_SOC)
static int tegra_channel_mipi_cal(struct tegra_channel *chan, char is_bypass)
{
	int lanes = 0, i;

	/* Build lane mask from channel ports */
	for (i = 0; i < chan->valid_ports; i++) {
		switch (chan->port[i]) {
		case PORT_A:
			lanes |= CSIA | CSIB;
			break;
		case PORT_B:
			/* PORT_B in MC = CSI_C (CILE) for 1-lane */
			if (chan->numlanes == 1)
				lanes |= CSIE;
			else
				lanes |= CSIC | CSID;
			break;
		}
	}

	if (lanes) {
		int ret;
		dev_dbg(&chan->video.dev, "T124 mipi_cal: lanes=0x%x\n", lanes);
		ret = tegra_mipi_calibration(lanes);
		if (ret)
			dev_warn(&chan->video.dev, "T124 mipi_cal failed: %d\n", ret);
	}
	return 0;
}
#else
static int tegra_channel_mipi_cal(struct tegra_channel *chan, char is_bypass)
{
	return 0;
}
#endif

/*
 * -----------------------------------------------------------------------------
 * subdevice set/unset operations
 * -----------------------------------------------------------------------------
 */
static int tegra_channel_set_stream(struct tegra_channel *chan, bool on)
{
	int ret = 0;

	if (atomic_read(&chan->is_streaming) == on)
		return 0;

	ret = v4l2_device_call_until_err(chan->video.v4l2_dev,
			chan->grp_id, video, s_stream, on);
	if (ret)
		return ret;

	atomic_set(&chan->is_streaming, on);

	return 0;
}

static int tegra_channel_set_power(struct tegra_channel *chan, bool on)
{
	int ret, lens_ret;

	dev_dbg(chan->vi->dev, "set_power(%d) lens_chan=%p\n",
		on, chan->lens_chan);

	if (on) {
		/* Power on: sensor first (rails up), then lens */
		ret = v4l2_device_call_until_err(chan->video.v4l2_dev,
				chan->grp_id, core, s_power, 1);
		if (ret)
			return ret;
		if (chan->lens_chan && chan->lens_chan->subdev_on_csi) {
			lens_ret = v4l2_subdev_call(
					chan->lens_chan->subdev_on_csi,
					core, s_power, 1);
			if (lens_ret)
				dev_warn(chan->vi->dev,
					"lens power on failed: %d\n", lens_ret);
		}
	} else {
		/* Power off: lens first (park while rails still up), then sensor */
		if (chan->lens_chan && chan->lens_chan->subdev_on_csi) {
			lens_ret = v4l2_subdev_call(
					chan->lens_chan->subdev_on_csi,
					core, s_power, 0);
			if (lens_ret)
				dev_warn(chan->vi->dev,
					"lens power off failed: %d\n", lens_ret);
		}
		ret = v4l2_device_call_until_err(chan->video.v4l2_dev,
				chan->grp_id, core, s_power, 0);
	}

	return ret;
}

static int update_clk(struct tegra_mc_vi *vi)
{
	unsigned int i;
	unsigned long max_clk = 0;

	for (i = 0; i < vi->num_channels; i++) {
		max_clk = max_clk > vi->chans[i].requested_hz ?
			max_clk : vi->chans[i].requested_hz;
	}
	return clk_set_rate(vi->clk, max_clk);
}

static void tegra_channel_update_clknbw(struct tegra_channel *chan, u8 on)
{
	/* width * height * fps * KBytes write to memory
	 * WAR: Using fix fps until we have a way to set it
	 */
	chan->requested_kbyteps = (on > 0 ? 1 : -1) * ((chan->format.width
				* chan->format.height
				* FRAMERATE * BPP_MEM) / 1000);
	chan->requested_hz = on > 0 ? 600000000 : 0; /* max VI clock */
	mutex_lock(&chan->vi->bw_update_lock);
	chan->vi->aggregated_kbyteps += chan->requested_kbyteps;
	vi_v4l2_update_isobw(chan->vi->aggregated_kbyteps, 0);
	vi_v4l2_set_la(tegra_vi_get(), 0, 0);
	update_clk(chan->vi);
	mutex_unlock(&chan->vi->bw_update_lock);
}

static int tegra_channel_start_streaming(struct vb2_queue *vq, u32 count)
{
	struct tegra_channel *chan = vb2_get_drv_priv(vq);
	struct media_pipeline *pipe = chan->video.entity.pipe;
	int ret = 0, i;

	tegra_channel_ec_init(chan);

	if (!chan->vi->pg_mode) {
		/* Start the pipeline. */
		ret = media_entity_pipeline_start(&chan->video.entity, pipe);
		if (ret < 0)
			goto error_pipeline_start;
	}

	if (chan->bypass) {
		ret = tegra_channel_set_stream(chan, true);
		if (ret < 0)
			goto error_set_stream;
		nvhost_module_enable_clk(chan->vi->dev);
		tegra_mipi_bias_pad_enable();
		mutex_lock(&chan->vi->mipical_lock);
		tegra_channel_mipi_cal(chan, 1);
		mutex_unlock(&chan->vi->mipical_lock);
		nvhost_module_disable_clk(chan->vi->dev);
		return ret;
	}

	chan->capture_state = CAPTURE_IDLE;
	/* T124 SLCG workaround: toggle PLL_D → CSI_OUT to init clock gating */
#if defined(CONFIG_ARCH_TEGRA_12x_SOC) || defined(CONFIG_ARCH_TEGRA_13x_SOC)
	{
		struct clk *pll_d = clk_get(chan->vi->dev, "pll_d");
		if (!IS_ERR(pll_d)) {
			tegra_clk_cfg_ex(pll_d, TEGRA_CLK_PLLD_CSI_OUT_ENB, 1);
			clk_prepare_enable(pll_d);
			udelay(1);
			clk_disable_unprepare(pll_d);
			tegra_clk_cfg_ex(pll_d, TEGRA_CLK_MIPI_CSI_OUT_ENB, 1);
			clk_put(pll_d);
			dev_dbg(&chan->video.dev,
				 "T124 SLCG: PLL_D CSI toggle done\n");
		}
	}

	/* Enable 2nd-level clock gating */
	tegra_channel_write(chan, TEGRA_VI_CFG_CG_CTRL, 0x1);

	/* Syncpt clean for T124 direct path */
	for (i = 0; i < chan->valid_ports; i++) {
		nvhost_syncpt_set_min_eq_max_ext(chan->vi->ndev, chan->syncpt[i]);
		nvhost_syncpt_set_min_eq_max_ext(chan->vi->ndev, chan->syncpt_mw[i]);
	}
#else
	for (i = 0; i < chan->valid_ports; i++) {
		tegra_csi_start_streaming(chan->vi->csi, chan->port[i]);
		/* ensure sync point state is clean */
		nvhost_syncpt_set_min_eq_max_ext(chan->vi->ndev,
							chan->syncpt[i]);
	}

	/* Note: Program VI registers after TPG, sensors and CSI streaming */
	ret = tegra_channel_capture_setup(chan);
	if (ret < 0)
		goto error_capture_setup;
#endif

	chan->sequence = 0;
	chan->bfirst_fstart = false;

	/* ISP pipeline setup BEFORE capture ops selection —
	 * use_isp must be set before ops are chosen.
	 * Only when t124_use_isp=1 (userspace control, default off) */
#if defined(CONFIG_ARCH_TEGRA_12x_SOC)
	if (t124_use_isp && !chan->vi->pg_mode && chan->valid_ports > 0) {
		u8 isp_class = (chan->port[0] == 0) ?
			ISP_A_CLASS_ID : ISP_B_CLASS_ID;
		struct tegra_isp_t124 *isp = isp_t124_get_isp(isp_class);

		dev_info(&chan->video.dev,
			 "ISP setup: port[0]=%d -> %s (class=0x%02x)\n",
			 chan->port[0],
			 isp_class == ISP_A_CLASS_ID ? "ISP-A" : "ISP-B",
			 isp_class);

		if (isp) {
			u32 raw_bpp = 2; /* RAW10 = 2 bytes/pixel */
			chan->isp_raw_size = chan->format.width *
					    chan->format.height * raw_bpp;
			/* Allocate raw buffer through ISP device —
			 * Test: can VI write to ISP SMMU domain? */
			chan->isp_raw_cpu = dma_alloc_coherent(
					&isp->pdev->dev,
					PAGE_ALIGN(chan->isp_raw_size),
					&chan->isp_raw_dma, GFP_KERNEL);
			if (chan->isp_raw_cpu) {
				ret = isp_t124_stream_init(isp,
						chan->format.width,
						chan->format.height,
						isp_reprocess);
				if (ret) {
					dev_warn(&chan->video.dev,
						 "ISP init failed: %d\n", ret);
					dma_free_coherent(
						&isp->pdev->dev,
						PAGE_ALIGN(chan->isp_raw_size),
						chan->isp_raw_cpu,
						chan->isp_raw_dma);
					chan->isp_raw_cpu = NULL;
				} else {
					chan->isp = isp;
					chan->use_isp = true;
					/* Allocate ISP output buffer —
					 * uses sensor resolution (set by stream_init).
					 * ISP writes beyond calculated NV12 size
					 * (SMMU faults at ~13.5MB for 3280x2464).
					 * Use 2x safety margin. */
					{
						u32 y_sz = isp->y_stride * isp->height;
						u32 uv_sz = isp->uv_stride * (isp->height / 2);
						chan->isp_out_size = (y_sz + 2 * uv_sz) * 2;
						chan->isp_out_cpu = dma_alloc_coherent(
							&isp->pdev->dev,
							PAGE_ALIGN(chan->isp_out_size),
							&chan->isp_out_dma,
							GFP_KERNEL);
						if (!chan->isp_out_cpu) {
							dev_warn(&chan->video.dev,
								 "ISP out buf alloc failed\n");
							chan->use_isp = false;
							isp_t124_stream_stop(isp);
							dma_free_coherent(&isp->pdev->dev,
								PAGE_ALIGN(chan->isp_raw_size),
								chan->isp_raw_cpu,
								chan->isp_raw_dma);
							chan->isp_raw_cpu = NULL;
						}
					}
					if (chan->use_isp)
						dev_info(&chan->video.dev,
							 "ISP pipeline active: %ux%u out_dma=0x%pad raw_dma=0x%pad\n",
							 chan->format.width,
							 chan->format.height,
							 &chan->isp_out_dma,
							 &chan->isp_raw_dma);
				}
			}
		}
	}
#endif

	/* Select capture ops — AFTER ISP setup so use_isp is known */
#if defined(CONFIG_ARCH_TEGRA_12x_SOC)
	if (!t124_csi_tpg && !chan->vi->pg_mode &&
	    !chan->use_isp && !chan->bypass) {
		if (t124_single_shot)
			chan->capture_ops = &tegra_vi_t124_singleshot_ops;
		else
			chan->capture_ops = &tegra_vi_t124_capture_ops;
	} else
#endif
		chan->capture_ops = &tegra_vi_t124_singleshot_ops;

	ret = chan->capture_ops->start_streaming(chan);
	if (ret < 0)
		goto error_capture_setup;

	/* Update clock and bandwidth based on the format */
	tegra_channel_update_clknbw(chan, 1);

	/* Start kthread to capture data to buffer */
	chan->kthread_capture_start = kthread_run(
					tegra_channel_kthread_capture_start,
					chan, chan->video.name);
	if (IS_ERR(chan->kthread_capture_start)) {
		dev_err(&chan->video.dev,
			"failed to run kthread for capture start\n");
		ret = PTR_ERR(chan->kthread_capture_start);
		goto error_capture_setup;
	}

	return 0;

error_capture_setup:
	if (!chan->vi->pg_mode)
		tegra_channel_set_stream(chan, false);
error_set_stream:
	if (!chan->vi->pg_mode)
		media_entity_pipeline_stop(&chan->video.entity);
error_pipeline_start:
	vq->start_streaming_called = 0;
	tegra_channel_queued_buf_done(chan, VB2_BUF_STATE_QUEUED);

	return ret;
}

static int tegra_channel_stop_streaming(struct vb2_queue *vq)
{
	struct tegra_channel *chan = vb2_get_drv_priv(vq);
	int index;

	if (!chan->bypass) {
		tegra_channel_stop_kthreads(chan);

		if (chan->capture_ops)
			chan->capture_ops->stop_streaming(chan);

		/* dequeue buffers back to app which are in capture queue */
		tegra_channel_queued_buf_done(chan, VB2_BUF_STATE_ERROR);

		/* Disable clock gating to enable continuous clock */
		tegra_channel_write(chan, TEGRA_VI_CFG_CG_CTRL, DISABLE);
		for (index = 0; index < chan->valid_ports; index++) {
			tegra_csi_stop_streaming(chan->vi->csi,
						chan->port[index]);
			/* Always clear single shot if armed at close */
			if (csi_read(chan, index, TEGRA_VI_CSI_SINGLE_SHOT))
				tegra_channel_clear_singleshot(chan, index);
		}
		/* Enable clock gating so VI can be clock gated if necessary */
		tegra_channel_write(chan, TEGRA_VI_CFG_CG_CTRL, ENABLE);
		tegra_csi_pad_control(chan->vi->csi, chan->port, DISABLE);
	}

	if (!chan->vi->pg_mode) {
		tegra_channel_set_stream(chan, false);
		media_entity_pipeline_stop(&chan->video.entity);
	}

	if (!chan->bypass)
		tegra_channel_update_clknbw(chan, 0);

	/* ISP pipeline cleanup */
	if (chan->isp_out_cpu && chan->isp) {
		dma_free_coherent(&chan->isp->pdev->dev,
				  PAGE_ALIGN(chan->isp_out_size),
				  chan->isp_out_cpu, chan->isp_out_dma);
		chan->isp_out_cpu = NULL;
	}
	if (chan->isp_raw_cpu && chan->isp) {
		dma_free_coherent(&chan->isp->pdev->dev,
				  PAGE_ALIGN(chan->isp_raw_size),
				  chan->isp_raw_cpu, chan->isp_raw_dma);
		chan->isp_raw_cpu = NULL;
	}
	if (chan->use_isp) {
		isp_t124_stream_stop(chan->isp);
		chan->use_isp = false;
		chan->isp = NULL;
	}

	tegra_mipi_bias_pad_disable();

	return 0;
}

static const struct vb2_ops tegra_channel_queue_qops = {
	.queue_setup = tegra_channel_queue_setup,
	.buf_prepare = tegra_channel_buffer_prepare,
	.buf_queue = tegra_channel_buffer_queue,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.start_streaming = tegra_channel_start_streaming,
	.stop_streaming = tegra_channel_stop_streaming,
};

/* -----------------------------------------------------------------------------
 * V4L2 ioctls
 */

static int
tegra_channel_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);

	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	cap->device_caps |= V4L2_CAP_EXT_PIX_FORMAT;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	strlcpy(cap->driver, "tegra-video", sizeof(cap->driver));
	strlcpy(cap->card, chan->video.name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s:%u",
		 dev_name(chan->vi->dev), chan->port[0]);

	return 0;
}

static int
tegra_channel_enum_framesizes(struct file *file, void *fh,
			      struct v4l2_frmsizeenum *sizes)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);

	return v4l2_device_call_until_err(chan->video.v4l2_dev,
			chan->grp_id, video, enum_framesizes, sizes);
}

static int
tegra_channel_enum_frameintervals(struct file *file, void *fh,
			      struct v4l2_frmivalenum *intervals)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);

	return v4l2_device_call_until_err(chan->video.v4l2_dev,
			chan->grp_id, video, enum_frameintervals,
			intervals);
}

static int
tegra_channel_s_parm(struct file *file, void *fh, struct v4l2_streamparm *parm)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);

	return v4l2_device_call_until_err(chan->video.v4l2_dev,
			chan->grp_id, video, s_parm, parm);
}

static int
tegra_channel_g_parm(struct file *file, void *fh, struct v4l2_streamparm *parm)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);

	return v4l2_device_call_until_err(chan->video.v4l2_dev,
			chan->grp_id, video, g_parm, parm);
}


static int
tegra_channel_enum_format(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	unsigned int index = 0, i;
	unsigned long *fmts_bitmap = NULL;

	if (chan->vi->pg_mode)
		fmts_bitmap = chan->vi->tpg_fmts_bitmap;
	else
		fmts_bitmap = chan->fmts_bitmap;

	if (f->index >= bitmap_weight(fmts_bitmap, MAX_FORMAT_NUM))
		return -EINVAL;

	for (i = 0; i < f->index + 1; i++, index++)
		index = find_next_bit(fmts_bitmap, MAX_FORMAT_NUM, index);

	index -= 1;
	f->pixelformat = tegra_core_get_fourcc_by_idx(index);
	tegra_core_get_description_by_idx(index, f->description);

	return 0;
}

static int
tegra_channel_g_edid(struct file *file, void *fh, struct v4l2_edid *edid)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_subdev *sd = chan->subdev_on_csi;

	return v4l2_subdev_call(sd, pad, get_edid, edid);
}

static int
tegra_channel_s_edid(struct file *file, void *fh, struct v4l2_edid *edid)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_subdev *sd = chan->subdev_on_csi;

	return v4l2_subdev_call(sd, pad, set_edid, edid);
}

static int
tegra_channel_s_dv_timings(struct file *file, void *fh,
		struct v4l2_dv_timings *timings)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_bt_timings *bt = &timings->bt;
	int ret;

	ret = v4l2_device_call_until_err(chan->video.v4l2_dev,
			chan->grp_id, video, s_dv_timings, timings);

	if (!ret) {
		chan->format.width = bt->width;
		chan->format.height = bt->height;
		chan->format.bytesperline = bt->width *
			chan->fmtinfo->bpp;
		chan->format.sizeimage = chan->format.bytesperline *
			chan->format.height;
	}

	if (chan->total_ports > 1)
		update_gang_mode(chan);

	return ret;
}

static int
tegra_channel_g_dv_timings(struct file *file, void *fh,
		struct v4l2_dv_timings *timings)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);

	return v4l2_device_call_until_err(chan->video.v4l2_dev,
			chan->grp_id, video, g_dv_timings, timings);
}

static int
tegra_channel_query_dv_timings(struct file *file, void *fh,
		struct v4l2_dv_timings *timings)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);

	return v4l2_device_call_until_err(chan->video.v4l2_dev,
			chan->grp_id, video, query_dv_timings, timings);
}

static int
tegra_channel_enum_dv_timings(struct file *file, void *fh,
		struct v4l2_enum_dv_timings *timings)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_subdev *sd = chan->subdev_on_csi;

	return v4l2_subdev_call(sd, pad, enum_dv_timings, timings);
}

static int
tegra_channel_dv_timings_cap(struct file *file, void *fh,
		struct v4l2_dv_timings_cap *cap)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_subdev *sd = chan->subdev_on_csi;

	return v4l2_subdev_call(sd, pad, dv_timings_cap, cap);
}

static void tegra_channel_fmt_align(struct v4l2_pix_format *pix,
				struct tegra_channel *chan, unsigned int bpp)
{
	unsigned int min_width;
	unsigned int max_width;
	unsigned int min_bpl;
	unsigned int max_bpl;
	unsigned int width;
	unsigned int align;
	unsigned int bpl;

	/* The transfer alignment requirements are expressed in bytes. Compute
	 * the minimum and maximum values, clamp the requested width and convert
	 * it back to pixels.
	 */
	align = lcm(chan->width_align, bpp);
	min_width = roundup(TEGRA_MIN_WIDTH, align);
	max_width = rounddown(TEGRA_MAX_WIDTH, align);
	width = roundup(pix->width * bpp, align);

	pix->width = clamp(width, min_width, max_width) / bpp;
	pix->height = clamp(pix->height, TEGRA_MIN_HEIGHT, TEGRA_MAX_HEIGHT);

	/* Clamp the requested bytes per line value. If the maximum bytes per
	 * line value is zero, the module doesn't support user configurable line
	 * sizes. Override the requested value with the minimum in that case.
	 */
	min_bpl = pix->width * bpp;
	max_bpl = rounddown(TEGRA_MAX_WIDTH, chan->stride_align);
	bpl = roundup(pix->bytesperline, chan->stride_align);

	pix->bytesperline = clamp(bpl, min_bpl, max_bpl);
	pix->sizeimage = pix->bytesperline * pix->height;
}

static int tegra_channel_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct tegra_channel *chan = container_of(ctrl->handler,
				struct tegra_channel, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_VI_BYPASS_MODE:
		if (switch_ctrl_qmenu[ctrl->val] == SWITCH_ON)
			chan->bypass = true;
		else
			chan->bypass = false;
		break;
	default:
		dev_err(&chan->video.dev, "%s:Not valid ctrl\n", __func__);
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops channel_ctrl_ops = {
	.s_ctrl	= tegra_channel_s_ctrl,
};

/**
 * By default channel will be in VI mode
 * User space can set it to 0 for working in bypass mode
 */
static const struct v4l2_ctrl_config bypass_mode_ctrl = {
	.ops = &channel_ctrl_ops,
	.id = V4L2_CID_VI_BYPASS_MODE,
	.name = "Bypass Mode",
	.type = V4L2_CTRL_TYPE_INTEGER_MENU,
	.def = 0,
	.min = 0,
	.max = ARRAY_SIZE(switch_ctrl_qmenu) - 1,
	.menu_skip_mask = 0,
	.qmenu_int = switch_ctrl_qmenu,
};

static int tegra_channel_setup_controls(struct tegra_channel *chan)
{
	int num_sd = 0;
	struct v4l2_subdev *sd = NULL;

	/* Initialize the subdev and controls here at first open */
	sd = chan->subdev[num_sd];
	while ((sd = chan->subdev[num_sd++]) &&
		(num_sd <= chan->num_subdevs)) {
		/* Add control handler for the subdevice */
		v4l2_ctrl_add_handler(&chan->ctrl_handler,
					sd->ctrl_handler, NULL);
		if (chan->ctrl_handler.error)
			dev_err(chan->vi->dev,
				"Failed to add sub-device controls\n");
	}

	/* Add the bypass mode ctrl */
	v4l2_ctrl_new_custom(&chan->ctrl_handler, &bypass_mode_ctrl, NULL);
	if (chan->ctrl_handler.error) {
		dev_err(chan->vi->dev,
			"Failed to add bypass control\n");
		return chan->ctrl_handler.error;
	}

	/* setup the controls */
	return v4l2_ctrl_handler_setup(&chan->ctrl_handler);
}

int tegra_channel_init_subdevices(struct tegra_channel *chan)
{
	struct media_entity *entity;
	struct media_pad *pad;
	int index = 0;
	int num_sd = 0;
	int grp_id = chan->port[0] + 1;
	struct v4l2_subdev *sd;

	/* set_stream of CSI */
	entity = &chan->video.entity;
	pad = media_entity_remote_source(&chan->pad);
	if (!pad)
		return -ENODEV;

	/* the remote source entity */
	entity = pad->entity;
	sd = media_entity_to_v4l2_subdev(entity);
	sd->grp_id = grp_id;
	chan->grp_id = grp_id;
	chan->subdev[num_sd++] = sd;
	/* Each CSI channel has only one pad, thus there
	 * is only one subdev directly attached to this
	 * CSI channel. Set this subdev to subdev_on_csi */
	chan->subdev_on_csi = sd;

	/* Append subdev name to this video dev name*/
	snprintf(chan->video.name, sizeof(chan->video.name), "%s, %s",
	chan->video.name, sd->name);

	index = pad->index - 1;
	while (index >= 0) {
		pad = &entity->pads[index];
		if (!(pad->flags & MEDIA_PAD_FL_SINK))
			break;

		pad = media_entity_remote_source(pad);
		if (pad == NULL ||
		    media_entity_type(pad->entity) != MEDIA_ENT_T_V4L2_SUBDEV)
			break;

		if (num_sd >= MAX_SUBDEVICES)
			break;

		entity = pad->entity;
		sd = media_entity_to_v4l2_subdev(entity);
		sd->grp_id = grp_id;
		chan->subdev[num_sd++] = sd;

		index = pad->index - 1;
	}
	chan->num_subdevs = num_sd;

	/* initialize the available formats */
	if (chan->num_subdevs)
		tegra_channel_fmts_bitmap_init(chan);

	return tegra_channel_setup_controls(chan);
}

static int
__tegra_channel_get_format(struct tegra_channel *chan,
			struct v4l2_pix_format *pix)
{
	struct tegra_video_format const *vfmt;
	struct v4l2_subdev_format fmt;
	int ret = 0;
	struct v4l2_subdev *sd = chan->subdev_on_csi;

	memset(&fmt, 0x0, sizeof(fmt));
	fmt.pad = 0;
	ret = v4l2_subdev_call(sd, pad, get_fmt, NULL, &fmt);
	if (ret == -ENOIOCTLCMD)
		return -ENOTTY;

	v4l2_fill_pix_format(pix, &fmt.format);
	vfmt = tegra_core_get_format_by_code(fmt.format.code);
	if (vfmt != NULL) {
		pix->pixelformat = vfmt->fourcc;
		pix->bytesperline = pix->width * vfmt->bpp;
		pix->sizeimage = pix->height * pix->bytesperline;
	}

	return ret;
}

static int
tegra_channel_get_format(struct file *file, void *fh,
			struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_pix_format *pix = &format->fmt.pix;

	return  __tegra_channel_get_format(chan, pix);
}

static int
__tegra_channel_try_format(struct tegra_channel *chan,
			struct v4l2_pix_format *pix)
{
	const struct tegra_video_format *vfmt;
	struct v4l2_subdev_format fmt;
	struct v4l2_subdev *sd = chan->subdev_on_csi;
	int ret = 0;

	/* Use the channel format if pixformat is not supported */
	vfmt = tegra_core_get_format_by_fourcc(pix->pixelformat);
	if (!vfmt) {
		pix->pixelformat = chan->format.pixelformat;
		vfmt = tegra_core_get_format_by_fourcc(pix->pixelformat);
	}

	tegra_channel_fmt_align(pix, chan, vfmt->bpp);

	fmt.which = V4L2_SUBDEV_FORMAT_TRY;
	fmt.pad = 0;
	v4l2_fill_mbus_format(&fmt.format, pix, vfmt->code);

	ret = v4l2_subdev_call(sd, pad, set_fmt, NULL, &fmt);
	if (ret == -ENOIOCTLCMD)
		return -ENOTTY;

	v4l2_fill_pix_format(pix, &fmt.format);
	if (ret)
		pix->bytesperline = pix->width * chan->fmtinfo->bpp;
	else
		pix->bytesperline = pix->width * vfmt->bpp;

	pix->sizeimage = pix->height * pix->bytesperline;

	return ret;
}

static int
tegra_channel_try_format(struct file *file, void *fh,
			struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);

	return  __tegra_channel_try_format(chan, &format->fmt.pix);
}

static int
__tegra_channel_set_format(struct tegra_channel *chan,
			struct v4l2_pix_format *pix)
{
	const struct tegra_video_format *vfmt;
	struct v4l2_subdev_format fmt;
	struct v4l2_subdev *sd = chan->subdev_on_csi;
	int ret = 0;

	vfmt = tegra_core_get_format_by_fourcc(pix->pixelformat);

	fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt.pad = 0;
	v4l2_fill_mbus_format(&fmt.format, pix, vfmt->code);

	ret = v4l2_subdev_call(sd, pad, set_fmt, NULL, &fmt);
	if (ret == -ENOIOCTLCMD)
		return -ENOTTY;

	v4l2_fill_pix_format(pix, &fmt.format);
	pix->bytesperline = pix->width * vfmt->bpp;
	pix->sizeimage = pix->height * pix->bytesperline;

	if (!ret) {
		chan->format = *pix;
		chan->fmtinfo = vfmt;
		if (chan->total_ports > 1)
			update_gang_mode(chan);
	}

	return ret;
}

static int
tegra_channel_set_format(struct file *file, void *fh,
			struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	int ret = 0;

	/* get the supported format by try_fmt */
	ret = __tegra_channel_try_format(chan, &format->fmt.pix);
	if (ret)
		return ret;

	if (vb2_is_busy(&chan->queue))
		return -EBUSY;

	return __tegra_channel_set_format(chan, &format->fmt.pix);
}

static int tegra_channel_s_crop(struct file *file, void *fh,
				const struct v4l2_crop *crop)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_subdev *subdev = NULL;
	int num_sd = 0;
	int ret = 0;

	while ((subdev = chan->subdev[num_sd++]) &&
		(num_sd <= chan->num_subdevs)) {
		ret = v4l2_subdev_call(subdev, video, s_crop, crop);
		if (ret < 0 && ret != -ENOIOCTLCMD)
			return ret;
	}

	return 0;
}

static int tegra_channel_g_crop(struct file *file, void *fh,
				struct v4l2_crop *crop)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_subdev *subdev = NULL;
	int num_sd = 0;
	int ret = 0;

	while ((subdev = chan->subdev[num_sd++]) &&
		(num_sd <= chan->num_subdevs)) {
		ret = v4l2_subdev_call(subdev, video, g_crop, crop);
		if (ret < 0 && ret != -ENOIOCTLCMD)
			return ret;
	}

	return 0;
}

static int tegra_channel_subscribe_event(struct v4l2_fh *fh,
				  const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_event_subscribe(fh, sub, 4, NULL);
	}
	return v4l2_ctrl_subscribe_event(fh, sub);
}

static int
tegra_channel_enum_input(struct file *file, void *fh, struct v4l2_input *inp)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_subdev *sd_on_csi = chan->subdev_on_csi;
	int ret;

	if (inp->index)
		return -EINVAL;

	ret = v4l2_device_call_until_err(chan->video.v4l2_dev,
			chan->grp_id, video, g_input_status, &inp->status);

	if (ret != -ENODEV) {
		if (v4l2_subdev_has_op(sd_on_csi, video, s_dv_timings))
			inp->capabilities = V4L2_IN_CAP_DV_TIMINGS;

		inp->type = V4L2_INPUT_TYPE_CAMERA;
		if (inp->capabilities == V4L2_IN_CAP_DV_TIMINGS)
			snprintf(inp->name,
				sizeof(inp->name), "HDMI %u",
				chan->port[0]);
		else
			snprintf(inp->name,
				sizeof(inp->name), "Camera %u",
				chan->port[0]);

		return ret;
	}

	return -ENOTTY;
}

static int tegra_channel_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int tegra_channel_s_input(struct file *file, void *priv, unsigned int i)
{
	if (i > 0)
		return -EINVAL;
	return 0;
}

static const struct v4l2_ioctl_ops tegra_channel_ioctl_ops = {
	.vidioc_querycap		= tegra_channel_querycap,
	.vidioc_enum_framesizes		= tegra_channel_enum_framesizes,
	.vidioc_enum_frameintervals	= tegra_channel_enum_frameintervals,
	.vidioc_s_parm			= tegra_channel_s_parm,
	.vidioc_g_parm			= tegra_channel_g_parm,
	.vidioc_enum_fmt_vid_cap	= tegra_channel_enum_format,
	.vidioc_g_fmt_vid_cap		= tegra_channel_get_format,
	.vidioc_s_fmt_vid_cap		= tegra_channel_set_format,
	.vidioc_try_fmt_vid_cap		= tegra_channel_try_format,
	.vidioc_s_crop			= tegra_channel_s_crop,
	.vidioc_g_crop			= tegra_channel_g_crop,
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
	.vidioc_g_edid			= tegra_channel_g_edid,
	.vidioc_s_edid			= tegra_channel_s_edid,
	.vidioc_s_dv_timings		= tegra_channel_s_dv_timings,
	.vidioc_g_dv_timings		= tegra_channel_g_dv_timings,
	.vidioc_query_dv_timings	= tegra_channel_query_dv_timings,
	.vidioc_enum_dv_timings		= tegra_channel_enum_dv_timings,
	.vidioc_dv_timings_cap		= tegra_channel_dv_timings_cap,
	.vidioc_subscribe_event		= tegra_channel_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
	.vidioc_enum_input		= tegra_channel_enum_input,
	.vidioc_g_input			= tegra_channel_g_input,
	.vidioc_s_input			= tegra_channel_s_input,
};

static int tegra_channel_open(struct file *fp)
{
	int ret;
	struct video_device *vdev = video_devdata(fp);
	struct tegra_channel *chan = video_get_drvdata(vdev);
	struct vi *tegra_vi;
	struct tegra_mc_vi *vi;
	struct tegra_csi_device *csi;

	mutex_lock(&chan->video_lock);
	ret = v4l2_fh_open(fp);
	if (ret || !v4l2_fh_is_singular_file(fp))
		goto unlock;

	if (chan->subdev_on_csi == NULL) {
		ret = -ENODEV;
		goto unlock;
	}

	vi = chan->vi;
	tegra_vi = vi->vi;
	csi = vi->csi;

	/* TPG mode and a real sensor is open, return busy */
	if (vi->pg_mode && tegra_vi->sensor_opened)
		return -EBUSY;

	/* None TPG mode and a TPG channel is opened, return busy */
	if (!vi->pg_mode && tegra_vi->tpg_opened)
		return -EBUSY;

	/* The first open then turn on power */
	if (atomic_add_return(1, &vi->power_on_refcnt) == 1) {
		tegra_vi_power_on(vi);
		tegra_csi_power_on(csi);
		if (vi->pg_mode)
			tegra_vi->tpg_opened = true;
		else
			tegra_vi->sensor_opened = true;
	}

	if (!vi->pg_mode &&
		(atomic_add_return(1, &chan->power_on_refcnt) == 1)) {
		/* power on sensors connected in channel */
		tegra_csi_channel_power_on(csi, chan->port);
		ret = tegra_channel_set_power(chan, 1);
		if (ret < 0)
			goto unlock;
	}

	chan->fh = (struct v4l2_fh *)fp->private_data;

unlock:
	mutex_unlock(&chan->video_lock);
	return ret;
}

static int tegra_channel_close(struct file *fp)
{
	int ret = 0;
	struct video_device *vdev = video_devdata(fp);
	struct tegra_channel *chan = video_get_drvdata(vdev);
	struct tegra_mc_vi *vi = chan->vi;
	struct vi *tegra_vi = vi->vi;
	struct tegra_csi_device *csi = vi->csi;
	bool is_singular;

	mutex_lock(&chan->video_lock);
	is_singular = v4l2_fh_is_singular_file(fp);
	ret = _vb2_fop_release(fp, NULL);

	if (!is_singular) {
		mutex_unlock(&chan->video_lock);
		return ret;
	}

	if (!vi->pg_mode &&
		atomic_dec_and_test(&chan->power_on_refcnt)) {
		/* power off sensors connected in channel */
		tegra_csi_channel_power_off(csi, chan->port);
		ret = tegra_channel_set_power(chan, 0);
		if (ret < 0)
			dev_err(vi->dev, "Failed to power off subdevices\n");
	}

	/* The last release then turn off power */
	if (atomic_dec_and_test(&vi->power_on_refcnt)) {
		tegra_csi_power_off(csi);
		tegra_vi_power_off(vi);
		if (vi->pg_mode)
			tegra_vi->tpg_opened = false;
		else
			tegra_vi->sensor_opened = false;
	}

	mutex_unlock(&chan->video_lock);
	return ret;
}

/* -----------------------------------------------------------------------------
 * V4L2 file operations
 */
static const struct v4l2_file_operations tegra_channel_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= video_ioctl2,
	.open		= tegra_channel_open,
	.release	= tegra_channel_close,
	.read		= vb2_fop_read,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
};

static void vi_channel_syncpt_init(struct tegra_channel *chan)
{
	int i;

	for (i = 0; i < chan->total_ports; i++) {
		chan->syncpt[i] =
			nvhost_get_syncpt_client_managed(chan->vi->ndev, "vi");
		chan->syncpt_mw[i] =
			nvhost_get_syncpt_client_managed(chan->vi->ndev,
							 "vi-mw");
	}
}

static void vi_channel_syncpt_free(struct tegra_channel *chan)
{
	int i;

	for (i = 0; i < chan->total_ports; i++) {
		nvhost_syncpt_put_ref_ext(chan->vi->ndev, chan->syncpt[i]);
		nvhost_syncpt_put_ref_ext(chan->vi->ndev, chan->syncpt_mw[i]);
	}
}

static void tegra_channel_csi_init(struct tegra_mc_vi *vi, unsigned int index)
{
	int numlanes = 0;
	int idx = 0;
	struct tegra_channel *chan  = &vi->chans[index];

	chan->gang_mode = CAMERA_NO_GANG_MODE;
	chan->total_ports = 0;
	memset(&chan->port[0], INVALID_CSI_PORT, TEGRA_CSI_BLOCKS);
	memset(&chan->syncpoint_fifo[0], 0, TEGRA_CSI_BLOCKS);
	if (vi->pg_mode) {
		chan->port[0] = index;
		chan->numlanes = 2;
	} else
		tegra_vi_get_port_info(chan, vi->dev->of_node, index);

	for (idx = 0; csi_port_is_valid(chan->port[idx]); idx++) {
		chan->total_ports++;
		numlanes = chan->numlanes - (idx * 4);
		numlanes = numlanes > 4 ? 4 : numlanes;
		/* maximum of 4 lanes are present per CSI block */
		chan->csibase[idx] = vi->iomem +
					TEGRA_VI_CSI_BASE(chan->port[idx]);
		set_csi_portinfo(vi->csi, chan->port[idx], numlanes);
	}
	/* based on gang mode valid ports will be updated - set default to 1 */
	chan->valid_ports = chan->total_ports ? 1 : 0;

	if (chan->valid_ports == 0) {
		/* No CSI port — check if this is an ISP or lens channel.
		 * Use of_graph_get_remote_port_parent() to find the device
		 * node of the remote endpoint and check its compatible.
		 */
		struct device_node *ports, *port, *ep, *remote_dev;
		bool found_isp = false;

		ports = of_get_child_by_name(vi->dev->of_node, "ports");
		if (ports) {
			for_each_child_of_node(ports, port) {
				u32 reg;
				if (of_node_cmp(port->name, "port"))
					continue;
				if (of_property_read_u32(port, "reg", &reg))
					continue;
				if (reg != index)
					continue;
				ep = of_get_next_child(port, NULL);
				if (!ep)
					break;
				remote_dev = of_graph_get_remote_port_parent(ep);
				of_node_put(ep);
				if (remote_dev &&
				    of_device_is_compatible(remote_dev,
							   "nvidia,tegra124-isp"))
					found_isp = true;
				of_node_put(remote_dev);
				of_node_put(port);
				break;
			}
			of_node_put(ports);
		}

		if (found_isp) {
			chan->is_isp_channel = true;
			dev_dbg(vi->dev, "channel %u: ISP (no CSI port)\n",
				index);
		} else {
			chan->is_lens_channel = true;
			dev_dbg(vi->dev, "channel %u: lens-only (no CSI port)\n",
				index);
		}
	}
}

static int tegra_channel_init(struct tegra_mc_vi *vi, unsigned int index)
{
	int ret;
	struct tegra_channel *chan = &vi->chans[index];

	chan->vi = vi;
	tegra_channel_csi_init(vi, index);

	chan->width_align = TEGRA_WIDTH_ALIGNMENT;
	chan->stride_align = TEGRA_STRIDE_ALIGNMENT;
	chan->num_subdevs = 0;
	mutex_init(&chan->video_lock);
	INIT_LIST_HEAD(&chan->capture);
	init_waitqueue_head(&chan->start_wait);
	spin_lock_init(&chan->start_lock);
	mutex_init(&chan->stop_kthread_lock);
	init_completion(&chan->capture_comp);
	INIT_LIST_HEAD(&chan->done);
	init_waitqueue_head(&chan->done_wait);
	spin_lock_init(&chan->done_lock);
	init_completion(&chan->capture_done_comp);
	atomic_set(&chan->dma_active, 0);
	init_waitqueue_head(&chan->dma_wait);
	atomic_set(&chan->is_streaming, DISABLE);

	if (chan->is_lens_channel) {
		/* Lens channel: only need media entity and ctrl handler.
		 * No video format, no vb2 queue, no video device.
		 */
		chan->pad.flags = MEDIA_PAD_FL_SINK;
		ret = media_entity_init(&chan->video.entity, 1, &chan->pad, 0);
		if (ret < 0)
			return ret;

		ret = v4l2_ctrl_handler_init(&chan->ctrl_handler, MAX_CID_CONTROLS);
		if (chan->ctrl_handler.error)
			return chan->ctrl_handler.error;

		chan->video.ctrl_handler = &chan->ctrl_handler;
		/* Name for debug, but no video_register_device */
		snprintf(chan->video.name, sizeof(chan->video.name), "%s-lens-%u",
			dev_name(vi->dev), chan->port[0]);

		return 0;
	}

	if (chan->is_isp_channel) {
		/* ISP channel: media entity only, no video device or VB2 queue.
		 * ISP subdev will be bound via async notifier.
		 */
		chan->pad.flags = MEDIA_PAD_FL_SINK;
		ret = media_entity_init(&chan->video.entity, 1, &chan->pad, 0);
		if (ret < 0)
			return ret;

		snprintf(chan->video.name, sizeof(chan->video.name), "%s-isp-%u",
			dev_name(vi->dev), index);

		return 0;
	}

	/* Init video format */
	chan->fmtinfo = tegra_core_get_format_by_code(TEGRA_VF_DEF);
	chan->format.pixelformat = chan->fmtinfo->fourcc;
	chan->format.colorspace = V4L2_COLORSPACE_SRGB;
	chan->format.field = V4L2_FIELD_NONE;
	chan->format.width = TEGRA_DEF_WIDTH;
	chan->format.height = TEGRA_DEF_HEIGHT;
	chan->format.bytesperline = chan->format.width * chan->fmtinfo->bpp;
	chan->format.sizeimage = chan->format.bytesperline *
				    chan->format.height;
	chan->buffer_offset[0] = 0;

	/* Initialize the media entity... */
	chan->pad.flags = MEDIA_PAD_FL_SINK;

	ret = media_entity_init(&chan->video.entity, 1, &chan->pad, 0);
	if (ret < 0)
		return ret;

	/* init control handler */
	ret = v4l2_ctrl_handler_init(&chan->ctrl_handler, MAX_CID_CONTROLS);
	if (chan->ctrl_handler.error) {
		dev_err(&chan->video.dev, "failed to init control handler\n");
		goto video_register_error;
	}

	/* init video node... */
	chan->video.fops = &tegra_channel_fops;
	chan->video.v4l2_dev = &vi->v4l2_dev;
	chan->video.queue = &chan->queue;
	snprintf(chan->video.name, sizeof(chan->video.name), "%s-%s-%u",
		dev_name(vi->dev), vi->pg_mode ? "tpg" : "output",
		chan->port[0]);
	chan->video.vfl_type = VFL_TYPE_GRABBER;
	chan->video.vfl_dir = VFL_DIR_RX;
	chan->video.release = video_device_release_empty;
	chan->video.ioctl_ops = &tegra_channel_ioctl_ops;
	chan->video.ctrl_handler = &chan->ctrl_handler;
	chan->video.lock = &chan->video_lock;

	set_bit(V4L2_FL_USE_FH_PRIO, &chan->video.flags);

	video_set_drvdata(&chan->video, chan);

	vi_channel_syncpt_init(chan);

	/* get the buffers queue... */
	chan->alloc_ctx = vb2_dma_contig_init_ctx(chan->vi->dev);
	if (IS_ERR(chan->alloc_ctx)) {
		dev_err(chan->vi->dev, "failed to init vb2 buffer\n");
		ret = -ENOMEM;
		goto vb2_init_error;
	}

	chan->queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	chan->queue.io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ | VB2_USERPTR;
	chan->queue.lock = &chan->video_lock;
	chan->queue.drv_priv = chan;
	chan->queue.buf_struct_size = sizeof(struct tegra_channel_buffer);
	chan->queue.ops = &tegra_channel_queue_qops;
	chan->queue.mem_ops = &vb2_dma_contig_memops;
	chan->queue.timestamp_type = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
				   | V4L2_BUF_FLAG_TSTAMP_SRC_EOF;
	ret = vb2_queue_init(&chan->queue);
	if (ret < 0) {
		dev_err(chan->vi->dev, "failed to initialize VB2 queue\n");
		goto vb2_queue_error;
	}

	ret = video_register_device(&chan->video, VFL_TYPE_GRABBER, -1);
	if (ret < 0) {
		dev_err(&chan->video.dev, "failed to register video device\n");
		goto video_register_error;
	}

	return 0;

video_register_error:
	vb2_queue_release(&chan->queue);
vb2_queue_error:
	vb2_dma_contig_cleanup_ctx(chan->alloc_ctx);
vb2_init_error:
	media_entity_cleanup(&chan->video.entity);
	return ret;
}

static int tegra_channel_cleanup(struct tegra_channel *chan)
{
	if (chan->is_lens_channel || chan->is_isp_channel) {
		/* These channels have no video device or VB2 queue */
		media_entity_cleanup(&chan->video.entity);
		if (chan->is_lens_channel)
			v4l2_ctrl_handler_free(&chan->ctrl_handler);
		return 0;
	}

	video_unregister_device(&chan->video);

	v4l2_ctrl_handler_free(&chan->ctrl_handler);
	vb2_queue_release(&chan->queue);
	vb2_dma_contig_cleanup_ctx(chan->alloc_ctx);

	vi_channel_syncpt_free(chan);
	media_entity_cleanup(&chan->video.entity);

	return 0;
}

int tegra_vi_channels_init(struct tegra_mc_vi *vi)
{
	unsigned int i;
	int ret;

	for (i = 0; i < vi->num_channels; i++) {
		ret = tegra_channel_init(vi, i);
		if (ret < 0) {
			dev_err(vi->dev, "channel %d init failed\n", i);
			return ret;
		}
	}
	return 0;
}
EXPORT_SYMBOL(tegra_vi_channels_init);

int tegra_vi_channels_cleanup(struct tegra_mc_vi *vi)
{
	unsigned int i;
	int ret;

	for (i = 0; i < vi->num_channels; i++) {
		ret = tegra_channel_cleanup(&vi->chans[i]);
		if (ret < 0) {
			dev_err(vi->dev, "channel %d cleanup failed\n", i);
			return ret;
		}
	}
	return 0;
}
EXPORT_SYMBOL(tegra_vi_channels_cleanup);

int tegra_clean_unlinked_channels(struct tegra_mc_vi *vi)
{
	unsigned int i;
	int ret;

	for (i = 0; i < vi->num_channels; i++) {
		struct tegra_channel *chan = &vi->chans[i];

		if (chan->num_subdevs)
			continue;

		ret = tegra_channel_cleanup(chan);
		if (ret < 0) {
			dev_err(vi->dev, "channel %d cleanup failed\n", i);
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL(tegra_clean_unlinked_channels);
