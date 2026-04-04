/*
 * Tegra T124 ISP Media Controller Driver — Header
 *
 * Copyright (c) 2025-2026, Smoke Team. All rights reserved.
 */

#ifndef __ISP_T124_H__
#define __ISP_T124_H__

#include <linux/types.h>
#include <media/v4l2-subdev.h>
#include <media/media-entity.h>

struct nvhost_channel;
struct dentry;

struct isp_dma_buf {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

struct tegra_isp_t124 {
	struct platform_device *pdev;

	/* V4L2 Media Controller */
	struct v4l2_subdev subdev;
	struct media_pad pads[2]; /* [0]=SOURCE, [1]=SINK */

	/* Host1x job submission */
	struct nvhost_channel *channel;
	u8 class_id; /* ISP_A_CLASS_ID (0x32) or ISP_B_CLASS_ID (0x34) */

	/* Syncpoints — 4 per ISP (params 0, 1, 2, 3) */
	u32 syncpt_memory;  /* param 0 */
	u32 syncpt_stats;   /* param 1 */
	u32 syncpt_stream;  /* param 2 — stock uses this for submit */
	u32 syncpt_loadv;   /* param 3 */

	/* Calibration data (from isp_t124_cal.h) */
	const u32 *cal_data;
	int cal_words;

	/* Runtime state (allocated during stream_init) */
	struct isp_dma_buf work_buf;    /* 256KB ISP working buffer */
	u32 *cmdbuf;                    /* DMA-coherent command buffer */
	dma_addr_t cmdbuf_phys;
	bool streaming;                 /* stream_init called */

	/* Frame dimensions (set during stream_init) */
	u32 width;
	u32 height;
	u32 y_stride;
	u32 uv_stride;
	u32 in_stride;   /* input RAW stride (bytes per line) */
	u32 in_format;   /* input pixel format descriptor */

	/* debugfs */
	struct dentry *debugfs_dir;
};

/* Called from legacy isp.c probe to initialize MC integration */
int tegra_isp_t124_mc_init(struct platform_device *pdev);
void tegra_isp_t124_mc_cleanup(struct platform_device *pdev);

/* Runtime API — called from channel.c capture path */
struct tegra_isp_t124 *isp_t124_get_isp(u8 class_id);
int isp_t124_stream_init(struct tegra_isp_t124 *isp, u32 width, u32 height);
void isp_t124_stream_stop(struct tegra_isp_t124 *isp);
int isp_t124_process_frame(struct tegra_isp_t124 *isp,
			   dma_addr_t out_dma, dma_addr_t stats_dma,
			   u32 vi_syncpt, u32 vi_thresh);

/* ---- ISP method offsets (from stock cmdbuf capture) ---- */

/* Control */
#define ISP_METHOD_CONTROL		0x00C
#define ISP_METHOD_ENABLE		0x015
#define ISP_METHOD_ISP_ENABLE		0x053

/* Input */
/* Stats buffer (0x100 = stats, NOT input!) */
#define ISP_METHOD_STATS_BUF		0x100

/* Processing */
#define ISP_METHOD_PROCESSING		0x500
#define ISP_METHOD_PROCESSING2		0x506

/* Runtime config (from stock submit 5) */
#define ISP_METHOD_RT_CONFIG		0x400
#define ISP_METHOD_RT_BUF_A		0x800
#define ISP_METHOD_RT_BUF_B		0x820
#define ISP_METHOD_RT_HIST		0x930
#define ISP_METHOD_RT_EXTRA		0xC00

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

/* Input (v3 methods — NOT 0x100!) */
#define ISP_METHOD_IN_TRIGGER		0xE30
#define ISP_METHOD_IN_DIMS		0xE31
#define ISP_METHOD_IN_STRIP		0xE32
#define ISP_METHOD_IN_FORMAT		0xE33
#define ISP_METHOD_IN_SURF0		0xE34
#define ISP_METHOD_IN_SURF1		0xE37
#define ISP_METHOD_IN_SURF2		0xE3A

/* ---- Stock values (from cmdbuf capture) ---- */

#define ISP_FORMAT_STOCK		0x04FE00E6
#define ISP_TRIGGER_RUNTIME		0x05
#define ISP_TRIGGER_POST_APPLY		0x0F
#define ISP_ENABLE_REPROCESS		0x07
#define ISP_ENABLE_STREAMING		0x04040007
#define ISP_ENABLE_MODE			ISP_ENABLE_STREAMING

/* Syncpoint increment condition values */
#define ISP_SYNCPT_COND_OP_DONE		4
#define ISP_SYNCPT_COND_STATS_DONE	5
#define ISP_SYNCPT_COND_RD_DONE		6

/* Surface descriptor: [IOVA, 0, stride] */
#define ISP_SURF_WORD1			0x00000000

/* ISP class IDs */
#define ISP_A_CLASS_ID			0x32
#define ISP_B_CLASS_ID			0x34

/* Command buffer size — enough for cal(~1545) + runtime config(~200) + frame(~60) + overhead */
#define ISP_CMDBUF_WORDS		4096
#define ISP_CMDBUF_SIZE			(ISP_CMDBUF_WORDS * 4)

/* Working buffer size */
#define ISP_WORK_BUF_SIZE		(256 * 1024)

#endif /* __ISP_T124_H__ */
