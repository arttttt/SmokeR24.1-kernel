/*
 * Tegra T124 ISP Media Controller Driver
 *
 * Copyright (c) 2025-2026, Smoke Team. All rights reserved.
 *
 * V4L2 subdev driver for T124 ISP integrated into the MC framework.
 * Separate from legacy nvhost ISP driver (drivers/video/tegra/host/isp/isp.c).
 *
 * This driver:
 * - Registers ISP-A as V4L2 subdev entity in the MC graph
 * - Manages host1x channel + syncpoint for command buffer submission
 * - Provides debugfs interface for hardware testing
 * - Will integrate into VI capture pipeline for RAW→YUV processing
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/nvhost.h>

#include <media/v4l2-subdev.h>
#include <media/v4l2-async.h>
#include <media/media-entity.h>

#include "isp_t124.h"
#include "isp_t124_cal.h"

/* Include host1x internals for opcode macros and job API */
#include "dev.h"
#include "nvhost_job.h"
#include "nvhost_acm.h"
#include "host1x/host1x01_hardware.h"
#include "class_ids.h"

/* ISP-A test dimensions (match stock calibration) */
#define ISP_TEST_W		3280
#define ISP_TEST_H		2460
#define ISP_TEST_Y_STRIDE	3328	/* 3280 aligned to 64 */
#define ISP_TEST_UV_STRIDE	1664	/* half Y stride */
#define ISP_TEST_INPUT_SIZE	(ISP_TEST_W * ISP_TEST_H * 2) /* full RAW frame */

/* ----------------------------------------------------------------
 * Host1x channel + job submission
 * ---------------------------------------------------------------- */

static int isp_t124_channel_init(struct tegra_isp_t124 *isp)
{
	struct nvhost_device_data *pdata = platform_get_drvdata(isp->pdev);
	int err;

	err = nvhost_channel_map(pdata, &isp->channel, isp);
	if (err) {
		dev_err(&isp->pdev->dev, "host1x channel map failed: %d\n", err);
		return err;
	}

	isp->syncpt_id = nvhost_get_syncpt_host_managed(isp->pdev, 0, "isp_mc");
	if (!isp->syncpt_id) {
		dev_err(&isp->pdev->dev, "syncpt allocation failed\n");
		nvhost_putchannel(isp->channel, 1);
		isp->channel = NULL;
		return -ENOMEM;
	}

	dev_info(&isp->pdev->dev, "host1x channel mapped, syncpt=%u\n",
		 isp->syncpt_id);
	return 0;
}

static void isp_t124_channel_cleanup(struct tegra_isp_t124 *isp)
{
	if (isp->syncpt_id) {
		nvhost_syncpt_put_ref_ext(isp->pdev, isp->syncpt_id);
		isp->syncpt_id = 0;
	}
	if (isp->channel) {
		nvhost_putchannel(isp->channel, 1);
		isp->channel = NULL;
	}
}

/**
 * isp_t124_submit() - Submit command buffer to ISP via host1x
 * @isp: ISP device
 * @cmdbuf: DMA-coherent command buffer (CPU virtual)
 * @cmdbuf_phys: DMA address of command buffer
 * @num_words: number of 32-bit words in command buffer
 *
 * Submits a gather to ISP class (0x32) with OP_DONE syncpt.
 * Returns 0 on success, negative on error or timeout.
 */
static int isp_t124_submit(struct tegra_isp_t124 *isp,
			    u32 *cmdbuf, dma_addr_t cmdbuf_phys,
			    int num_words)
{
	struct nvhost_job *job;
	int err;

	if (!isp->channel || !isp->syncpt_id)
		return -ENODEV;

	job = nvhost_job_alloc(isp->channel, 1, 0, 0, 1);
	if (!job)
		return -ENOMEM;

	job->sp->id = isp->syncpt_id;
	job->sp->incrs = 1;
	job->num_syncpts = 1;

	err = nvhost_job_add_client_gather_address(job, num_words,
			NV_VIDEO_STREAMING_ISP_CLASS_ID, cmdbuf_phys);
	if (err) {
		nvhost_job_put(job);
		return err;
	}

	err = nvhost_channel_submit(job);
	if (err) {
		nvhost_job_put(job);
		return err;
	}

	err = nvhost_syncpt_wait_timeout_ext(isp->pdev,
			job->sp->id, job->sp->fence,
			msecs_to_jiffies(500), NULL, NULL);

	nvhost_job_put(job);
	return err;
}

/**
 * isp_t124_ping() - Verify ISP hardware responds
 */
static int isp_t124_ping(struct tegra_isp_t124 *isp)
{
	struct device *dev = &isp->pdev->dev;
	u32 *cmdbuf;
	dma_addr_t cmdbuf_phys;
	int err, n = 0;

	err = nvhost_module_busy(isp->pdev);
	if (err)
		return err;

	cmdbuf = dma_alloc_coherent(dev, PAGE_SIZE, &cmdbuf_phys, GFP_KERNEL);
	if (!cmdbuf) {
		err = -ENOMEM;
		goto idle;
	}

	cmdbuf[n++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_immediate_v(),
		isp->syncpt_id);
	cmdbuf[n++] = NVHOST_OPCODE_NOOP;

	err = isp_t124_submit(isp, cmdbuf, cmdbuf_phys, n);

	dma_free_coherent(dev, PAGE_SIZE, cmdbuf, cmdbuf_phys);
idle:
	nvhost_module_idle(isp->pdev);
	return err;
}

/**
 * isp_t124_probe_methods() - Test all known ISP method offsets
 */
static int isp_t124_probe_methods(struct tegra_isp_t124 *isp,
				  struct seq_file *s)
{
	static const struct {
		u16 offset;
		const char *name;
	} methods[] = {
		{ 0x00C, "control" },
		{ 0x015, "enable" },
		{ 0x053, "isp_enable" },
		{ 0x100, "input_buf" },
		{ 0x500, "processing" },
		{ 0x651, "tc_ch0_ctrl" },
		{ 0x652, "tc_ch0_lut" },
		{ 0x902, "stats_ctrl" },
		{ 0x903, "stats_aewb" },
		{ 0x906, "stats_af_ctrl" },
		{ 0x907, "stats_af" },
		{ 0xD00, "ls_ctrl" },
		{ 0xD0A, "ls_enable" },
		{ 0xD0B, "ls_table" },
		{ 0xE00, "out_width" },
		{ 0xE01, "out_height" },
		{ 0xE02, "out_format" },
		{ 0xE03, "out_color" },
		{ 0xE04, "out_surf_y" },
		{ 0xE07, "out_surf_u" },
		{ 0xE0A, "out_surf_v" },
		{ 0xE30, "out_enable" },
		{ 0xE31, "out_dims" },
		{ 0xE32, "out_stride" },
		{ 0xE33, "out_fmt2" },
	};
	struct device *dev = &isp->pdev->dev;
	struct nvhost_job *job;
	u32 *cmdbuf;
	dma_addr_t cmdbuf_phys;
	int err, i, n;
	ktime_t start;
	s64 us;

	if (!isp->channel || !isp->syncpt_id)
		return -ENODEV;

	err = nvhost_module_busy(isp->pdev);
	if (err)
		return err;

	cmdbuf = dma_alloc_coherent(dev, PAGE_SIZE, &cmdbuf_phys, GFP_KERNEL);
	if (!cmdbuf) {
		nvhost_module_idle(isp->pdev);
		return -ENOMEM;
	}

	seq_printf(s, "Probing %zu ISP methods (write 0 + REG_WR_SAFE):\n",
		   ARRAY_SIZE(methods));

	for (i = 0; i < ARRAY_SIZE(methods); i++) {
		n = 0;
		cmdbuf[n++] = nvhost_opcode_incr(methods[i].offset, 1);
		cmdbuf[n++] = 0;
		cmdbuf[n++] = nvhost_opcode_imm_incr_syncpt(
			host1x_uclass_incr_syncpt_cond_reg_wr_safe_v(),
			isp->syncpt_id);
		cmdbuf[n++] = NVHOST_OPCODE_NOOP;

		job = nvhost_job_alloc(isp->channel, 1, 0, 0, 1);
		if (!job) continue;
		job->sp->id = isp->syncpt_id;
		job->sp->incrs = 1;
		job->num_syncpts = 1;

		err = nvhost_job_add_client_gather_address(job, n,
			NV_VIDEO_STREAMING_ISP_CLASS_ID, cmdbuf_phys);
		if (err) { nvhost_job_put(job); continue; }

		start = ktime_get();
		err = nvhost_channel_submit(job);
		if (err) { nvhost_job_put(job); continue; }

		err = nvhost_syncpt_wait_timeout_ext(isp->pdev,
			job->sp->id, job->sp->fence,
			msecs_to_jiffies(100), NULL, NULL);
		us = ktime_us_delta(ktime_get(), start);
		nvhost_job_put(job);

		seq_printf(s, "  0x%03x %-14s %s (%lld us)\n",
			   methods[i].offset, methods[i].name,
			   err ? "TIMEOUT" : "OK", us);
		if (err) {
			seq_printf(s, "  *** stopped at first timeout\n");
			break;
		}
	}

	dma_free_coherent(dev, PAGE_SIZE, cmdbuf, cmdbuf_phys);
	nvhost_module_idle(isp->pdev);
	return 0;
}

/* ----------------------------------------------------------------
 * DMA frame processing test
 * ---------------------------------------------------------------- */

struct isp_dma_buf {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

/*
 * Allocate DMA buffer via dma_alloc_coherent.
 * On SMMU-attached devices (like ISP), this allocates individual pages
 * and maps them into a contiguous IOVA range — same as nvmap IOVMM.
 * No physically contiguous memory needed.
 */
static int isp_dma_buf_alloc(struct device *dev, struct isp_dma_buf *buf,
			     size_t size)
{
	buf->size = PAGE_ALIGN(size);
	buf->cpu = dma_alloc_coherent(dev, buf->size, &buf->dma, GFP_KERNEL);
	if (!buf->cpu)
		return -ENOMEM;
	return 0;
}

static void isp_dma_buf_free(struct device *dev, struct isp_dma_buf *buf)
{
	if (!buf->cpu)
		return;
	dma_free_coherent(dev, buf->size, buf->cpu, buf->dma);
	buf->cpu = NULL;
}

/**
 * isp_t124_dma_test() - Test ISP DMA frame processing
 *
 * Submits stock calibration + per-frame output command buffer to ISP-A,
 * checks if output Y plane contains non-zero data.
 */
static int isp_t124_dma_test(struct tegra_isp_t124 *isp, struct seq_file *s)
{
	struct device *dev = &isp->pdev->dev;
	struct isp_dma_buf in_buf = {}, out_buf = {}, work_buf = {};
	u32 *cmdbuf;
	dma_addr_t cmdbuf_phys;
	dma_addr_t out_y_dma, out_u_dma, out_v_dma;
	int err, n, nonzero, i, cal_last_idx, trigger_idx;
	size_t y_size, uv_size, out_total;
	ktime_t start;
	s64 us;

	if (!isp->channel || !isp->syncpt_id)
		return -ENODEV;

	y_size = ISP_TEST_Y_STRIDE * ISP_TEST_H;
	uv_size = ISP_TEST_UV_STRIDE * (ISP_TEST_H / 2);
	out_total = y_size + uv_size * 2;

	seq_printf(s, "ISP DMA test: %dx%d, out=%zuKB (Y+U+V contiguous)\n",
		   ISP_TEST_W, ISP_TEST_H, out_total / 1024);

	err = nvhost_module_busy(isp->pdev);
	if (err)
		return err;

	/*
	 * Allocate buffers matching stock memory model:
	 * - input: full RAW frame (W*H*2), also used as ISP working memory
	 * - output: single contiguous Y+U+V (like stock nvmap handle with offsets)
	 * - work_buf: ISP runtime working buffer for method 0x054
	 *   (stock calibration has IOVA 0x00745f5c which is invalid here)
	 */
	err = isp_dma_buf_alloc(dev, &in_buf, ISP_TEST_INPUT_SIZE);
	if (err) {
		seq_printf(s, "input alloc failed: %d\n", err);
		goto idle;
	}
	err = isp_dma_buf_alloc(dev, &out_buf, out_total);
	if (err) {
		seq_printf(s, "output alloc failed: %d\n", err);
		goto free_in;
	}
	/* ISP working buffer — stock uses ~512KB, allocate 256KB */
	err = isp_dma_buf_alloc(dev, &work_buf, 256 * 1024);
	if (err) {
		seq_printf(s, "work buf alloc failed: %d\n", err);
		goto free_out;
	}

	/* Command buffer: need ~1600 words, use 2 pages */
	cmdbuf = dma_alloc_coherent(dev, PAGE_SIZE * 2, &cmdbuf_phys,
				    GFP_KERNEL);
	if (!cmdbuf) {
		err = -ENOMEM;
		seq_printf(s, "cmdbuf alloc failed\n");
		goto free_work;
	}

	/* Contiguous output: Y at base, U after Y, V after U */
	out_y_dma = out_buf.dma;
	out_u_dma = out_buf.dma + y_size;
	out_v_dma = out_buf.dma + y_size + uv_size;

	/* Fill input with test pattern */
	memset(in_buf.cpu, 0xA5, ISP_TEST_INPUT_SIZE);

	seq_printf(s, "  input:  dma=0x%pad, %d KB\n",
		   &in_buf.dma, ISP_TEST_INPUT_SIZE / 1024);
	seq_printf(s, "  output: dma=0x%pad, %zu KB (contiguous Y+U+V)\n",
		   &out_buf.dma, out_total / 1024);
	seq_printf(s, "    Y=+0x0, U=+0x%zx, V=+0x%zx\n",
		   y_size, y_size + uv_size);
	seq_printf(s, "  work:   dma=0x%pad, 256 KB\n", &work_buf.dma);

	/*
	 * Build SINGLE combined command buffer:
	 * calibration + output config + surfaces + trigger + syncpt
	 *
	 * SET_CLASS stripped — class set by nvhost_job_add_client_gather_address()
	 * in the pushbuffer (outside gather), bypassing gather filter.
	 */

	/* Part 1: SET_CLASS + Calibration
	 * Gather filter disabled for ISP, so SET_CLASS inside gather is OK.
	 * ISP methods require SET_CLASS for proper dispatch.
	 */
	n = 0;
	cmdbuf[n++] = nvhost_opcode_setclass(NV_VIDEO_STREAMING_ISP_CLASS_ID,
					     0, 0);
	memcpy(&cmdbuf[n], isp_a_cal_data, sizeof(isp_a_cal_data));
	n += ARRAY_SIZE(isp_a_cal_data);

	/*
	 * Patch calibration tail: INCR(0x053, 2) → [enable, buffer_addr]
	 * Last word is stock ISP working buffer IOVA (0x00745f5c) — replace
	 * with our allocated working buffer.
	 */
	cal_last_idx = ARRAY_SIZE(isp_a_cal_data); /* +1 offset for SET_CLASS */
	seq_printf(s, "  cal[%d] (work buf): 0x%08x → 0x%08x\n",
		   cal_last_idx, cmdbuf[cal_last_idx], (u32)work_buf.dma);
	cmdbuf[cal_last_idx] = (u32)work_buf.dma;

	/* Part 2: Output enable block */
	cmdbuf[n++] = nvhost_opcode_setclass(NV_VIDEO_STREAMING_ISP_CLASS_ID,
					     0, 0);
	cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_DIMS, 1);
	cmdbuf[n++] = ISP_TEST_W | (ISP_TEST_H << 16);
	cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_FMT2, 1);
	cmdbuf[n++] = ISP_FORMAT_STOCK;
	cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_STRIDE, 1);
	cmdbuf[n++] = ISP_TEST_Y_STRIDE;
	cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_ENABLE, 1);
	cmdbuf[n++] = 0x00000007;
	cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_ENABLE, 1);
	cmdbuf[n++] = 0x00000001;

	/* Part 3: Output dimensions (stock per-frame block) */
	cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_WIDTH, 1);
	cmdbuf[n++] = ((ISP_TEST_W - 1) & 0x3FFF) << 16;
	cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_HEIGHT, 1);
	cmdbuf[n++] = ((ISP_TEST_H - 1) & 0x3FFF) << 16;

	/* Output format */
	cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_FORMAT, 1);
	cmdbuf[n++] = ISP_FORMAT_STOCK;
	cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_COLOR, 1);
	cmdbuf[n++] = 0x00000000;

	/* Output surface Y: [IOVA, 0, stride] — contiguous buffer */
	cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_Y, 3);
	cmdbuf[n++] = (u32)out_y_dma;
	cmdbuf[n++] = ISP_SURF_WORD1;
	cmdbuf[n++] = ISP_TEST_Y_STRIDE;

	/* Output surface U */
	cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_U, 3);
	cmdbuf[n++] = (u32)out_u_dma;
	cmdbuf[n++] = ISP_SURF_WORD1;
	cmdbuf[n++] = ISP_TEST_UV_STRIDE;

	/* Output surface V */
	cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_V, 3);
	cmdbuf[n++] = (u32)out_v_dma;
	cmdbuf[n++] = ISP_SURF_WORD1;
	cmdbuf[n++] = ISP_TEST_UV_STRIDE;

	/* Processing: 5 zeros + (H << 16) | W */
	cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_PROCESSING, 6);
	cmdbuf[n++] = 0;
	cmdbuf[n++] = 0;
	cmdbuf[n++] = 0;
	cmdbuf[n++] = 0;
	cmdbuf[n++] = 0;
	cmdbuf[n++] = (ISP_TEST_H << 16) | ISP_TEST_W;

	/* Input buffer */
	cmdbuf[n++] = nvhost_opcode_setclass(NV_VIDEO_STREAMING_ISP_CLASS_ID,
					     0, 0);
	cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_INPUT_BUF, 4);
	cmdbuf[n++] = (u32)in_buf.dma;
	cmdbuf[n++] = 0;
	cmdbuf[n++] = 0;
	cmdbuf[n++] = 0;

	/* Trigger — will be patched for each phase */
	cmdbuf[n++] = nvhost_opcode_setclass(NV_VIDEO_STREAMING_ISP_CLASS_ID,
					     0, 0);
	cmdbuf[n++] = nvhost_opcode_nonincr(ISP_METHOD_CONTROL, 1);
	trigger_idx = n;
	cmdbuf[n++] = ISP_TRIGGER_POST_APPLY; /* placeholder, patched below */

	/* Syncpt OP_DONE */
	cmdbuf[n++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_op_done_v(),
		isp->syncpt_id);
	cmdbuf[n++] = NVHOST_OPCODE_NOOP;

	seq_printf(s, "  cmdbuf: %d words (cal=%zu + frame), trigger@[%d]\n",
		   n, ARRAY_SIZE(isp_a_cal_data), trigger_idx);

	/*
	 * ISP requires two submits to produce output:
	 * Submit 1: initializes ISP pipeline (output empty)
	 * Submit 2+: ISP processes and writes output
	 *
	 * On stock kernel: trigger 0x05 works for both phases.
	 * On 24.1: also try 0x0F for phase 1, 0x05 for phase 2.
	 * Submit the same full cmdbuf each time.
	 */

	/* Phase 1: Init (trigger 0x0F) */
	cmdbuf[trigger_idx] = ISP_TRIGGER_POST_APPLY;

	start = ktime_get();
	err = isp_t124_submit(isp, cmdbuf, cmdbuf_phys, n);
	us = ktime_us_delta(ktime_get(), start);
	if (err) {
		seq_printf(s, "  phase 1 (0x0F init) FAILED: %d (%lld us)\n",
			   err, us);
		goto free_cmdbuf;
	}
	seq_printf(s, "  phase 1 (0x0F init) OK (%lld us)\n", us);

	/* Phase 2: Frame (trigger 0x05) */
	cmdbuf[trigger_idx] = ISP_TRIGGER_RUNTIME;

	start = ktime_get();
	err = isp_t124_submit(isp, cmdbuf, cmdbuf_phys, n);
	us = ktime_us_delta(ktime_get(), start);
	if (err) {
		seq_printf(s, "  phase 2 (0x05 run) FAILED: %d (%lld us)\n",
			   err, us);
		goto free_cmdbuf;
	}
	seq_printf(s, "  phase 2 (0x05 run) OK (%lld us)\n", us);

	/* Phase 3: Another frame (trigger 0x05, may be needed for pipeline) */
	start = ktime_get();
	err = isp_t124_submit(isp, cmdbuf, cmdbuf_phys, n);
	us = ktime_us_delta(ktime_get(), start);
	if (err) {
		seq_printf(s, "  phase 3 (0x05 run2) FAILED: %d (%lld us)\n",
			   err, us);
		goto free_cmdbuf;
	}
	seq_printf(s, "  phase 3 (0x05 run2) OK (%lld us)\n", us);

	/* Wait for ISP DMA to complete */
	msleep(200);

	/* Check output Y plane for non-zero data */
	nonzero = 0;
	for (i = 0; i < 4096; i++) {
		if (((u8 *)out_buf.cpu)[i] != 0)
			nonzero++;
	}

	seq_printf(s, "\n  output Y: %d/4096 bytes non-zero%s\n",
		   nonzero,
		   nonzero ? " (ISP WROTE DATA!)" : " (untouched)");

	/* Hex dump first 64 bytes */
	seq_printf(s, "  Y hex[0..63]: ");
	for (i = 0; i < 64; i++)
		seq_printf(s, "%02x ", ((u8 *)out_buf.cpu)[i]);
	seq_printf(s, "\n");

	/* Check stats region in input buffer at +0x20000 */
	if (ISP_TEST_INPUT_SIZE >= 0x20000 + 64) {
		u8 *stats = (u8 *)in_buf.cpu + 0x20000;
		nonzero = 0;
		for (i = 0; i < 4096; i++) {
			if (stats[i] != 0xA5)
				nonzero++;
		}
		seq_printf(s, "  stats region: %d/4096 bytes changed\n",
			   nonzero);
	}

	/* Check if ISP touched the working buffer */
	{
		u8 *wb = (u8 *)work_buf.cpu;
		nonzero = 0;
		for (i = 0; i < 4096; i++) {
			if (wb[i] != 0)
				nonzero++;
		}
		seq_printf(s, "  work buf: %d/4096 bytes non-zero\n", nonzero);
	}

free_cmdbuf:
	dma_free_coherent(dev, PAGE_SIZE * 2, cmdbuf, cmdbuf_phys);
free_work:
	isp_dma_buf_free(dev, &work_buf);
free_out:
	isp_dma_buf_free(dev, &out_buf);
free_in:
	isp_dma_buf_free(dev, &in_buf);
idle:
	nvhost_module_idle(isp->pdev);
	return err;
}

/* ----------------------------------------------------------------
 * debugfs
 * ---------------------------------------------------------------- */

static int isp_t124_debugfs_ping_show(struct seq_file *s, void *data)
{
	struct tegra_isp_t124 *isp = s->private;
	ktime_t start = ktime_get();
	int ret = isp_t124_ping(isp);
	s64 us = ktime_us_delta(ktime_get(), start);

	seq_printf(s, "ISP-A %s, syncpt %u (%lld us)\n",
		   ret ? "FAILED" : "alive", isp->syncpt_id, us);
	return 0;
}

static int isp_t124_debugfs_ping_open(struct inode *inode, struct file *file)
{
	return single_open(file, isp_t124_debugfs_ping_show, inode->i_private);
}

static const struct file_operations isp_t124_debugfs_ping_fops = {
	.open    = isp_t124_debugfs_ping_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int isp_t124_debugfs_probe_show(struct seq_file *s, void *data)
{
	return isp_t124_probe_methods(s->private, s);
}

static int isp_t124_debugfs_probe_open(struct inode *inode, struct file *file)
{
	return single_open(file, isp_t124_debugfs_probe_show, inode->i_private);
}

static const struct file_operations isp_t124_debugfs_probe_fops = {
	.open    = isp_t124_debugfs_probe_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int isp_t124_debugfs_dma_show(struct seq_file *s, void *data)
{
	return isp_t124_dma_test(s->private, s);
}

static int isp_t124_debugfs_dma_open(struct inode *inode, struct file *file)
{
	return single_open(file, isp_t124_debugfs_dma_show, inode->i_private);
}

static const struct file_operations isp_t124_debugfs_dma_fops = {
	.open    = isp_t124_debugfs_dma_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ----------------------------------------------------------------
 * MMIO bypass test — write ISP registers directly, no host1x
 * This is a DIRTY HACK to check if ISP hardware works at all.
 * ---------------------------------------------------------------- */
#include <linux/io.h>

#define ISP_A_BASE 0x54600000
#define ISP_A_SIZE 0x40000

static int isp_t124_mmio_test(struct tegra_isp_t124 *isp, struct seq_file *s)
{
	struct device *dev = &isp->pdev->dev;
	struct isp_dma_buf in_buf = {}, out_buf = {}, work_buf = {};
	void __iomem *base;
	int err, i, nonzero;
	size_t y_size, uv_size, out_total;

	/* Real sensor dimensions — 64x64 doesn't work on stock either */
	const int W = 3280, H = 2460;
	const int Y_STRIDE = (W + 63) & ~63;  /* 3328 */
	const int UV_STRIDE = Y_STRIDE / 2;   /* 1664 */

	y_size = Y_STRIDE * H;
	uv_size = UV_STRIDE * (H / 2);
	out_total = y_size + uv_size * 2;

	seq_printf(s, "MMIO bypass test: %dx%d\n", W, H);

	err = nvhost_module_busy(isp->pdev);
	if (err) return err;

	/* ioremap ISP-A */
	base = ioremap(ISP_A_BASE, ISP_A_SIZE);
	if (!base) {
		seq_printf(s, "ioremap failed!\n");
		goto idle;
	}

	/* Read some regs before */
	seq_printf(s, "  pre: 0x030=%08x 0x054=%08x 0x074=%08x\n",
		   readl(base + 0x030), readl(base + 0x054),
		   readl(base + 0x074));

	/* Alloc DMA buffers — these go through ISP SMMU */
	err = isp_dma_buf_alloc(dev, &in_buf, W * H * 2);
	if (err) { seq_printf(s, "in alloc fail\n"); goto unmap; }
	err = isp_dma_buf_alloc(dev, &out_buf, out_total);
	if (err) { seq_printf(s, "out alloc fail\n"); goto free_in; }
	err = isp_dma_buf_alloc(dev, &work_buf, 256 * 1024);
	if (err) { seq_printf(s, "work alloc fail\n"); goto free_out; }

	/* Fill input with pattern, output with sentinel */
	memset(in_buf.cpu, 0x55, W * H * 2);
	memset(out_buf.cpu, 0xDE, out_total);
	memset(work_buf.cpu, 0xDE, 256 * 1024);

	seq_printf(s, "  in=0x%pad out=0x%pad work=0x%pad\n",
		   &in_buf.dma, &out_buf.dma, &work_buf.dma);

	/*
	 * Write ISP registers directly via MMIO.
	 * Method offset * 4 = MMIO byte offset.
	 *
	 * Stock MMIO values for reference:
	 * 0x008(0x20)=F000F800, 0x015(0x54)=04040007, etc.
	 */

	/* Init/config registers */
	writel(0xF000F800, base + 0x008 * 4);  /* input cfg */
	writel(0x000000EB, base + 0x014 * 4);  /* sensor param (IMX179) */
	writel(0x04040007, base + 0x015 * 4);  /* enable mode */
	writel(0x0A00500A, base + 0x018 * 4);  /* proc0 */
	writel(0x00008089, base + 0x019 * 4);  /* proc1 */
	writel(0x013645CB, base + 0x01A * 4);  /* cal0 */
	writel(0x000001E7, base + 0x01B * 4);  /* cal1 */
	writel(0x00000001, base + 0x01C * 4);  /* unk */
	writel(0x00000001, base + 0x01D * 4);  /* CG_CTRL */
	writel(0x00000003, base + 0x01F * 4);  /* mode */
	writel(0x00003232, base + 0x05E * 4);  /* unk2 */

	/* ISP enable + work buffer */
	writel(0x00000001, base + 0x053 * 4);  /* ISP enable */
	writel((u32)work_buf.dma, base + 0x054 * 4);  /* work buf addr */

	/* Output dimensions */
	writel(((W - 1) & 0x3FFF) << 16, base + 0xE00 * 4);
	writel(((H - 1) & 0x3FFF) << 16, base + 0xE01 * 4);
	writel(0x04FE00E6, base + 0xE02 * 4);  /* format */
	writel(0x00000000, base + 0xE03 * 4);  /* color */

	/* Output surfaces Y/U/V: [addr, 0, stride] */
	writel((u32)out_buf.dma, base + 0xE04 * 4);
	writel(0, base + 0xE05 * 4);
	writel(Y_STRIDE, base + 0xE06 * 4);

	writel((u32)(out_buf.dma + y_size), base + 0xE07 * 4);
	writel(0, base + 0xE08 * 4);
	writel(UV_STRIDE, base + 0xE09 * 4);

	writel((u32)(out_buf.dma + y_size + uv_size), base + 0xE0A * 4);
	writel(0, base + 0xE0B * 4);
	writel(UV_STRIDE, base + 0xE0C * 4);

	/* Secondary output config (0x60D8-like) */
	writel(W | (H << 16), base + 0xE31 * 4);
	writel(Y_STRIDE, base + 0xE32 * 4);
	writel(0x04FE00E6, base + 0xE33 * 4);
	writel(0x00000001, base + 0xE30 * 4);  /* output enable */

	/* Processing: method 0x500 (6 words) */
	writel(0, base + 0x500 * 4);
	writel(0, base + 0x501 * 4);
	writel(0, base + 0x502 * 4);
	writel(0, base + 0x503 * 4);
	writel(0, base + 0x504 * 4);
	writel((H << 16) | W, base + 0x505 * 4);

	/* Input buffer: method 0x100 (4 words) */
	writel((u32)in_buf.dma, base + 0x100 * 4);
	writel(0, base + 0x101 * 4);
	writel(0, base + 0x102 * 4);
	writel(0, base + 0x103 * 4);

	/* Read back to confirm */
	seq_printf(s, "  post-config: 0x054=%08x E00=%08x E04=%08x 0x100=%08x\n",
		   readl(base + 0x054 * 4),
		   readl(base + 0xE00 * 4),
		   readl(base + 0xE04 * 4),
		   readl(base + 0x100 * 4));

	/* Trigger 0x0F — static config apply */
	seq_printf(s, "  trigger 0x0F...\n");
	writel(0x0000000F, base + 0x00C * 4);
	msleep(50);
	seq_printf(s, "  after 0x0F: ctrl=%08x status=%08x\n",
		   readl(base + 0x030), readl(base + 0x034));

	/* Trigger 0x05 — runtime frame processing */
	seq_printf(s, "  trigger 0x05...\n");
	writel(0x00000005, base + 0x00C * 4);
	msleep(100);
	seq_printf(s, "  after 0x05: ctrl=%08x status=%08x\n",
		   readl(base + 0x030), readl(base + 0x034));

	/* Second trigger 0x05 */
	seq_printf(s, "  trigger 0x05 again...\n");
	writel(0x00000005, base + 0x00C * 4);
	msleep(100);
	seq_printf(s, "  after 0x05: ctrl=%08x status=%08x\n",
		   readl(base + 0x030), readl(base + 0x034));

	/* Check output */
	nonzero = 0;
	for (i = 0; i < (int)y_size && i < 4096; i++) {
		if (((u8 *)out_buf.cpu)[i] != 0xDE)
			nonzero++;
	}
	seq_printf(s, "\n  output Y: %d/%d bytes changed from 0xDE%s\n",
		   nonzero, (int)(y_size < 4096 ? y_size : 4096),
		   nonzero ? " ** ISP DMA WORKS! **" : " (untouched)");

	/* Hex dump first 32 bytes */
	seq_printf(s, "  hex: ");
	for (i = 0; i < 32 && i < (int)y_size; i++)
		seq_printf(s, "%02x ", ((u8 *)out_buf.cpu)[i]);
	seq_printf(s, "\n");

	/* Check work buffer */
	nonzero = 0;
	for (i = 0; i < 4096; i++) {
		if (((u8 *)work_buf.cpu)[i] != 0xDE)
			nonzero++;
	}
	seq_printf(s, "  work: %d/4096 bytes changed\n", nonzero);

	isp_dma_buf_free(dev, &work_buf);
free_out:
	isp_dma_buf_free(dev, &out_buf);
free_in:
	isp_dma_buf_free(dev, &in_buf);
unmap:
	iounmap(base);
idle:
	nvhost_module_idle(isp->pdev);
	return 0;
}

static int isp_t124_debugfs_mmio_show(struct seq_file *s, void *data)
{
	struct tegra_isp_t124 *isp = s->private;
	return isp_t124_mmio_test(isp, s);
}

static int isp_t124_debugfs_mmio_open(struct inode *inode, struct file *file)
{
	return single_open(file, isp_t124_debugfs_mmio_show, inode->i_private);
}

static const struct file_operations isp_t124_debugfs_mmio_fops = {
	.open    = isp_t124_debugfs_mmio_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static void isp_t124_debugfs_init(struct tegra_isp_t124 *isp)
{
	isp->debugfs_dir = debugfs_create_dir("isp_t124", NULL);
	if (!isp->debugfs_dir)
		return;
	debugfs_create_file("ping", 0444, isp->debugfs_dir,
			    isp, &isp_t124_debugfs_ping_fops);
	debugfs_create_file("probe", 0444, isp->debugfs_dir,
			    isp, &isp_t124_debugfs_probe_fops);
	debugfs_create_file("dma_test", 0444, isp->debugfs_dir,
			    isp, &isp_t124_debugfs_dma_fops);
	debugfs_create_file("mmio_test", 0444, isp->debugfs_dir,
			    isp, &isp_t124_debugfs_mmio_fops);
}

static void isp_t124_debugfs_cleanup(struct tegra_isp_t124 *isp)
{
	debugfs_remove_recursive(isp->debugfs_dir);
	isp->debugfs_dir = NULL;
}

/* ----------------------------------------------------------------
 * V4L2 subdev
 * ---------------------------------------------------------------- */

static int isp_t124_subdev_s_power(struct v4l2_subdev *sd, int on)
{
	return 0; /* nvhost PM handles power */
}

static const struct v4l2_subdev_core_ops isp_t124_subdev_core_ops = {
	.s_power = isp_t124_subdev_s_power,
};

static const struct v4l2_subdev_ops isp_t124_subdev_ops = {
	.core = &isp_t124_subdev_core_ops,
};

#if defined(CONFIG_MEDIA_CONTROLLER)
static const struct media_entity_operations isp_t124_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};
#endif

static int isp_t124_register_subdev(struct tegra_isp_t124 *isp)
{
	struct v4l2_subdev *sd = &isp->subdev;
	struct device *dev = &isp->pdev->dev;
	int ret;

	v4l2_subdev_init(sd, &isp_t124_subdev_ops);
	sd->dev = dev;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	snprintf(sd->name, sizeof(sd->name), "%s", dev_name(dev));

	isp->pads[0].flags = MEDIA_PAD_FL_SOURCE;
	isp->pads[1].flags = MEDIA_PAD_FL_SINK;

#if defined(CONFIG_MEDIA_CONTROLLER)
	sd->entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	sd->entity.ops = &isp_t124_media_ops;
	ret = media_entity_init(&sd->entity, 2, isp->pads, 0);
	if (ret < 0)
		return ret;
#endif

	ret = v4l2_async_register_subdev(sd);
	if (ret < 0) {
		media_entity_cleanup(&sd->entity);
		return ret;
	}

	dev_info(dev, "V4L2 subdev registered\n");
	return 0;
}

static void isp_t124_unregister_subdev(struct tegra_isp_t124 *isp)
{
	v4l2_async_unregister_subdev(&isp->subdev);
	media_entity_cleanup(&isp->subdev.entity);
}

/* ----------------------------------------------------------------
 * Init/cleanup — called from legacy isp.c probe
 * ---------------------------------------------------------------- */

static struct tegra_isp_t124 *g_isp_t124;

int tegra_isp_t124_mc_init(struct platform_device *pdev)
{
	struct tegra_isp_t124 *isp;
	int err;

	if (!of_device_is_compatible(pdev->dev.of_node, "nvidia,tegra124-isp"))
		return -ENODEV;

	isp = devm_kzalloc(&pdev->dev, sizeof(*isp), GFP_KERNEL);
	if (!isp)
		return -ENOMEM;

	isp->pdev = pdev;

	err = isp_t124_register_subdev(isp);
	if (err) {
		dev_warn(&pdev->dev, "V4L2 subdev failed: %d\n", err);
		return err;
	}

	err = isp_t124_channel_init(isp);
	if (err) {
		dev_warn(&pdev->dev, "host1x channel failed: %d\n", err);
	} else {
		isp_t124_debugfs_init(isp);
	}

	g_isp_t124 = isp;
	dev_info(&pdev->dev, "T124 ISP MC driver initialized\n");
	return 0;
}
EXPORT_SYMBOL(tegra_isp_t124_mc_init);

void tegra_isp_t124_mc_cleanup(struct platform_device *pdev)
{
	if (!g_isp_t124)
		return;
	isp_t124_debugfs_cleanup(g_isp_t124);
	isp_t124_channel_cleanup(g_isp_t124);
	isp_t124_unregister_subdev(g_isp_t124);
	g_isp_t124 = NULL;
}
EXPORT_SYMBOL(tegra_isp_t124_mc_cleanup);

MODULE_DESCRIPTION("Tegra T124 ISP Media Controller Driver");
MODULE_LICENSE("GPL v2");
