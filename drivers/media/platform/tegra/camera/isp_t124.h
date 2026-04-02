/*
 * Tegra T124 ISP Media Controller Driver — Header
 *
 * Copyright (c) 2025-2026, Smoke Team. All rights reserved.
 */

#ifndef __ISP_T124_H__
#define __ISP_T124_H__

#include <media/v4l2-subdev.h>
#include <media/media-entity.h>

struct nvhost_channel;
struct dentry;

struct tegra_isp_t124 {
	struct platform_device *pdev;

	/* V4L2 Media Controller */
	struct v4l2_subdev subdev;
	struct media_pad pads[2]; /* [0]=SOURCE, [1]=SINK */

	/* Host1x job submission */
	struct nvhost_channel *channel;
	u32 syncpt_id;

	/* Calibration data */
	const u32 *cal_data;
	int cal_words;

	/* debugfs */
	struct dentry *debugfs_dir;
};

/* Called from legacy isp.c probe to initialize MC integration */
int tegra_isp_t124_mc_init(struct platform_device *pdev);
void tegra_isp_t124_mc_cleanup(struct platform_device *pdev);

/* ---- ISP method offsets (from stock cmdbuf capture) ---- */

/* Control */
#define ISP_METHOD_CONTROL		0x00C
#define ISP_METHOD_ENABLE		0x015
#define ISP_METHOD_ISP_ENABLE		0x053

/* Input */
#define ISP_METHOD_INPUT_BUF		0x100

/* Processing */
#define ISP_METHOD_PROCESSING		0x500

/* Tone curves (4 channels) */
#define ISP_METHOD_TC_CH0_CTRL		0x651
#define ISP_METHOD_TC_CH0_LUT		0x652
#define ISP_METHOD_TC_CH1_CTRL		0x653
#define ISP_METHOD_TC_CH1_LUT		0x654
#define ISP_METHOD_TC_CH2_CTRL		0x655
#define ISP_METHOD_TC_CH2_LUT		0x656
#define ISP_METHOD_TC_CH3_CTRL		0x657
#define ISP_METHOD_TC_CH3_LUT		0x658

/* Stats config */
#define ISP_METHOD_STATS_CTRL		0x902
#define ISP_METHOD_STATS_AEWB		0x903
#define ISP_METHOD_STATS_AF_CTRL	0x906
#define ISP_METHOD_STATS_AF		0x907

/* Lens shading */
#define ISP_METHOD_LS_CTRL		0xD00
#define ISP_METHOD_LS_ENABLE		0xD0A
#define ISP_METHOD_LS_TABLE		0xD0B
#define ISP_METHOD_LS_EXTRA		0xD20

/* Output */
#define ISP_METHOD_OUT_WIDTH		0xE00
#define ISP_METHOD_OUT_HEIGHT		0xE01
#define ISP_METHOD_OUT_FORMAT		0xE02
#define ISP_METHOD_OUT_COLOR		0xE03
#define ISP_METHOD_OUT_SURF_Y		0xE04
#define ISP_METHOD_OUT_SURF_U		0xE07
#define ISP_METHOD_OUT_SURF_V		0xE0A
#define ISP_METHOD_OUT_ENABLE		0xE30
#define ISP_METHOD_OUT_DIMS		0xE31
#define ISP_METHOD_OUT_STRIDE		0xE32
#define ISP_METHOD_OUT_FMT2		0xE33
#define ISP_METHOD_IN_SURF		0xE34

/* ---- Stock values (from cmdbuf capture) ---- */

#define ISP_FORMAT_STOCK		0x04FE00E6
#define ISP_TRIGGER_RUNTIME		0x05
#define ISP_TRIGGER_POST_APPLY		0x0F
#define ISP_ENABLE_MODE			0x04040007

/* Surface descriptor: [IOVA, 0, stride] */
#define ISP_SURF_WORD1			0x00000000

/* ISP class IDs */
#define ISP_A_CLASS_ID			0x32
#define ISP_B_CLASS_ID			0x34

#endif /* __ISP_T124_H__ */
