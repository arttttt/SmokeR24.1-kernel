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
#define ISP_TEST_INPUT_SIZE	(256 * 1024) /* 256KB: 128KB working + 128KB stats */

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
	struct isp_dma_buf in_buf = {}, out_y = {}, out_u = {}, out_v = {};
	u32 *cmdbuf, *frame_cmdbuf;
	dma_addr_t cmdbuf_phys, frame_phys;
	int err, n, nonzero, i;
	size_t y_size, uv_size;
	ktime_t start;
	s64 us;

	if (!isp->channel || !isp->syncpt_id)
		return -ENODEV;

	y_size = ISP_TEST_Y_STRIDE * ISP_TEST_H;
	uv_size = ISP_TEST_UV_STRIDE * (ISP_TEST_H / 2);

	seq_printf(s, "ISP DMA test: %dx%d, Y=%zuKB, UV=%zuKB each\n",
		   ISP_TEST_W, ISP_TEST_H, y_size / 1024, uv_size / 1024);

	err = nvhost_module_busy(isp->pdev);
	if (err)
		return err;

	/* Allocate buffers */
	err = isp_dma_buf_alloc(dev, &in_buf, ISP_TEST_INPUT_SIZE);
	if (err) {
		seq_printf(s, "input alloc failed: %d\n", err);
		goto idle;
	}
	err = isp_dma_buf_alloc(dev, &out_y, y_size);
	if (err) {
		seq_printf(s, "output Y alloc failed: %d\n", err);
		goto free_in;
	}
	err = isp_dma_buf_alloc(dev, &out_u, uv_size);
	if (err) {
		seq_printf(s, "output U alloc failed: %d\n", err);
		goto free_y;
	}
	err = isp_dma_buf_alloc(dev, &out_v, uv_size);
	if (err) {
		seq_printf(s, "output V alloc failed: %d\n", err);
		goto free_u;
	}

	/* Command buffer for calibration (need ~1550 words, use 2 pages) */
	cmdbuf = dma_alloc_coherent(dev, PAGE_SIZE * 2, &cmdbuf_phys,
				    GFP_KERNEL);
	if (!cmdbuf) {
		err = -ENOMEM;
		seq_printf(s, "cmdbuf alloc failed\n");
		goto free_v;
	}

	/* Per-frame command buffer (separate page) */
	frame_cmdbuf = dma_alloc_coherent(dev, PAGE_SIZE, &frame_phys,
					  GFP_KERNEL);
	if (!frame_cmdbuf) {
		err = -ENOMEM;
		seq_printf(s, "frame cmdbuf alloc failed\n");
		goto free_cmdbuf;
	}

	/* Fill input with test pattern */
	memset(in_buf.cpu, 0xA5, ISP_TEST_INPUT_SIZE);

	seq_printf(s, "  input:  dma=0x%pad, %d KB\n",
		   &in_buf.dma, ISP_TEST_INPUT_SIZE / 1024);
	seq_printf(s, "  out_y:  dma=0x%pad, %zu KB\n",
		   &out_y.dma, y_size / 1024);
	seq_printf(s, "  out_u:  dma=0x%pad, %zu KB\n",
		   &out_u.dma, uv_size / 1024);
	seq_printf(s, "  out_v:  dma=0x%pad, %zu KB\n",
		   &out_v.dma, uv_size / 1024);

	/*
	 * Submit 1: Calibration (stock ISP-A data, SET_CLASS stripped)
	 * Copy calibration, patch last word (buffer IOVA) with our input.
	 * Append syncpt increment for submit completion.
	 */
	n = ARRAY_SIZE(isp_a_cal_data);
	memcpy(cmdbuf, isp_a_cal_data, sizeof(isp_a_cal_data));
	/* Last word of calibration is the stock buffer IOVA — replace */
	cmdbuf[n - 1] = (u32)in_buf.dma;
	/* Append syncpt REG_WR_SAFE increment */
	cmdbuf[n++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_reg_wr_safe_v(),
		isp->syncpt_id);
	cmdbuf[n++] = NVHOST_OPCODE_NOOP;

	seq_printf(s, "  cal submit: %d words (cal=%zu + syncpt)\n",
		   n, ARRAY_SIZE(isp_a_cal_data));

	start = ktime_get();
	err = isp_t124_submit(isp, cmdbuf, cmdbuf_phys, n);
	us = ktime_us_delta(ktime_get(), start);
	if (err) {
		seq_printf(s, "  cal submit FAILED: %d (%lld us)\n", err, us);
		goto free_frame;
	}
	seq_printf(s, "  cal submit OK (%lld us)\n", us);

	/*
	 * Submit 2: Per-frame output config + trigger
	 * Matches stock 45-word block but without SET_CLASS opcodes.
	 * Also adds the "0x60D8-like" registers from isp_test.c.
	 */
	n = 0;

	/* Output enable block (from isp_test.c 0x60D8 path) */
	frame_cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_DIMS, 1);
	frame_cmdbuf[n++] = ISP_TEST_W | (ISP_TEST_H << 16);
	frame_cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_FMT2, 1);
	frame_cmdbuf[n++] = ISP_FORMAT_STOCK;
	frame_cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_STRIDE, 1);
	frame_cmdbuf[n++] = ISP_TEST_Y_STRIDE;
	frame_cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_ENABLE, 1);
	frame_cmdbuf[n++] = 0x00000007;
	frame_cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_ENABLE, 1);
	frame_cmdbuf[n++] = 0x00000001;

	/* Output dimensions (from stock 45-word block) */
	frame_cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_WIDTH, 1);
	frame_cmdbuf[n++] = ((ISP_TEST_W - 1) & 0x3FFF) << 16;
	frame_cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_HEIGHT, 1);
	frame_cmdbuf[n++] = ((ISP_TEST_H - 1) & 0x3FFF) << 16;

	/* Output format */
	frame_cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_FORMAT, 1);
	frame_cmdbuf[n++] = ISP_FORMAT_STOCK;
	frame_cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_COLOR, 1);
	frame_cmdbuf[n++] = 0x00000000;

	/* Output surface Y: [IOVA, 0, stride] */
	frame_cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_Y, 3);
	frame_cmdbuf[n++] = (u32)out_y.dma;
	frame_cmdbuf[n++] = ISP_SURF_WORD1;
	frame_cmdbuf[n++] = ISP_TEST_Y_STRIDE;

	/* Output surface U */
	frame_cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_U, 3);
	frame_cmdbuf[n++] = (u32)out_u.dma;
	frame_cmdbuf[n++] = ISP_SURF_WORD1;
	frame_cmdbuf[n++] = ISP_TEST_UV_STRIDE;

	/* Output surface V */
	frame_cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_V, 3);
	frame_cmdbuf[n++] = (u32)out_v.dma;
	frame_cmdbuf[n++] = ISP_SURF_WORD1;
	frame_cmdbuf[n++] = ISP_TEST_UV_STRIDE;

	/* Processing: 5 zeros + (H << 16) | W */
	frame_cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_PROCESSING, 6);
	frame_cmdbuf[n++] = 0;
	frame_cmdbuf[n++] = 0;
	frame_cmdbuf[n++] = 0;
	frame_cmdbuf[n++] = 0;
	frame_cmdbuf[n++] = 0;
	frame_cmdbuf[n++] = (ISP_TEST_H << 16) | ISP_TEST_W;

	/* Input buffer */
	frame_cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_INPUT_BUF, 4);
	frame_cmdbuf[n++] = (u32)in_buf.dma;
	frame_cmdbuf[n++] = 0;
	frame_cmdbuf[n++] = 0;
	frame_cmdbuf[n++] = 0;

	/* Trigger */
	frame_cmdbuf[n++] = nvhost_opcode_nonincr(ISP_METHOD_CONTROL, 1);
	frame_cmdbuf[n++] = ISP_TRIGGER_RUNTIME;

	/* Syncpt OP_DONE */
	frame_cmdbuf[n++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_op_done_v(),
		isp->syncpt_id);
	frame_cmdbuf[n++] = NVHOST_OPCODE_NOOP;

	seq_printf(s, "  frame submit: %d words\n", n);

	start = ktime_get();
	err = isp_t124_submit(isp, frame_cmdbuf, frame_phys, n);
	us = ktime_us_delta(ktime_get(), start);
	if (err) {
		seq_printf(s, "  frame submit FAILED: %d (%lld us)\n", err, us);
		goto free_frame;
	}
	seq_printf(s, "  frame submit OK (%lld us)\n", us);

	/* Check output Y plane for non-zero data
	 * (dma_alloc_coherent buffers are always CPU-coherent, no sync needed)
	 */
	nonzero = 0;
	for (i = 0; i < 4096; i++) {
		if (((u8 *)out_y.cpu)[i] != 0)
			nonzero++;
	}

	seq_printf(s, "\n  output Y: %d/4096 bytes non-zero%s\n",
		   nonzero,
		   nonzero ? " (ISP WROTE DATA!)" : " (untouched)");

	/* Hex dump first 64 bytes */
	seq_printf(s, "  Y hex[0..63]: ");
	for (i = 0; i < 64; i++)
		seq_printf(s, "%02x ", ((u8 *)out_y.cpu)[i]);
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

free_frame:
	dma_free_coherent(dev, PAGE_SIZE, frame_cmdbuf, frame_phys);
free_cmdbuf:
	dma_free_coherent(dev, PAGE_SIZE * 2, cmdbuf, cmdbuf_phys);
free_v:
	isp_dma_buf_free(dev, &out_v);
free_u:
	isp_dma_buf_free(dev, &out_u);
free_y:
	isp_dma_buf_free(dev, &out_y);
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
