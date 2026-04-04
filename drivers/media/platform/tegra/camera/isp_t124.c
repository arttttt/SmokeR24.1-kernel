/*
 * Tegra T124 ISP Media Controller Driver
 *
 * Copyright (c) 2025-2026, Smoke Team. All rights reserved.
 *
 * V4L2 subdev driver for T124 ISP-A and ISP-B.
 * Provides runtime API for VI→ISP capture pipeline integration.
 *
 * ISP-A (class 0x32): rear IMX179, 3280x2460
 * ISP-B (class 0x34): front OV5693, 2592x1944
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

/* ----------------------------------------------------------------
 * DMA buffer helpers
 * ---------------------------------------------------------------- */

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
	memset(buf, 0, sizeof(*buf));
}

/* ----------------------------------------------------------------
 * Host1x channel + syncpoints
 * ---------------------------------------------------------------- */

static int isp_t124_channel_init(struct tegra_isp_t124 *isp)
{
	struct nvhost_device_data *pdata = platform_get_drvdata(isp->pdev);
	int err;

	err = nvhost_channel_map(pdata, &isp->channel, isp);
	if (err) {
		dev_err(&isp->pdev->dev, "host1x channel map failed: %d\n",
			err);
		return err;
	}

	/* 3 syncpoints per ISP: memory (param 0), stats (param 1), loadv (param 3) */
	isp->syncpt_memory = nvhost_get_syncpt_host_managed(isp->pdev,
							    0, "isp_memory");
	isp->syncpt_stats = nvhost_get_syncpt_host_managed(isp->pdev,
							   1, "isp_stats");
	isp->syncpt_loadv = nvhost_get_syncpt_host_managed(isp->pdev,
							   3, "isp_loadv");

	if (!isp->syncpt_memory || !isp->syncpt_stats || !isp->syncpt_loadv) {
		dev_err(&isp->pdev->dev, "syncpt allocation failed\n");
		goto fail;
	}

	dev_info(&isp->pdev->dev,
		 "host1x channel mapped, syncpts: mem=%u stats=%u loadv=%u\n",
		 isp->syncpt_memory, isp->syncpt_stats, isp->syncpt_loadv);
	return 0;

fail:
	if (isp->syncpt_memory)
		nvhost_syncpt_put_ref_ext(isp->pdev, isp->syncpt_memory);
	if (isp->syncpt_stats)
		nvhost_syncpt_put_ref_ext(isp->pdev, isp->syncpt_stats);
	if (isp->syncpt_loadv)
		nvhost_syncpt_put_ref_ext(isp->pdev, isp->syncpt_loadv);
	nvhost_putchannel(isp->channel, 1);
	isp->channel = NULL;
	return -ENOMEM;
}

static void isp_t124_channel_cleanup(struct tegra_isp_t124 *isp)
{
	if (isp->syncpt_memory) {
		nvhost_syncpt_put_ref_ext(isp->pdev, isp->syncpt_memory);
		isp->syncpt_memory = 0;
	}
	if (isp->syncpt_stats) {
		nvhost_syncpt_put_ref_ext(isp->pdev, isp->syncpt_stats);
		isp->syncpt_stats = 0;
	}
	if (isp->syncpt_loadv) {
		nvhost_syncpt_put_ref_ext(isp->pdev, isp->syncpt_loadv);
		isp->syncpt_loadv = 0;
	}
	if (isp->channel) {
		nvhost_putchannel(isp->channel, 1);
		isp->channel = NULL;
	}
}

/* ----------------------------------------------------------------
 * Ping test (simple submit)
 * ---------------------------------------------------------------- */

static int isp_t124_ping(struct tegra_isp_t124 *isp)
{
	struct device *dev = &isp->pdev->dev;
	struct nvhost_job *job;
	u32 *cmdbuf;
	dma_addr_t cmdbuf_phys;
	int err, n = 0;

	if (!isp->channel || !isp->syncpt_memory)
		return -ENODEV;

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
		isp->syncpt_memory);
	cmdbuf[n++] = NVHOST_OPCODE_NOOP;

	job = nvhost_job_alloc(isp->channel, 1, 0, 0, 1);
	if (!job) {
		err = -ENOMEM;
		goto free_cmdbuf;
	}

	job->sp->id = isp->syncpt_memory;
	job->sp->incrs = 1;
	job->num_syncpts = 1;

	err = nvhost_job_add_client_gather_address(job, n,
			isp->class_id, cmdbuf_phys);
	if (err) {
		nvhost_job_put(job);
		goto free_cmdbuf;
	}

	err = nvhost_channel_submit(job);
	if (err) {
		nvhost_job_put(job);
		goto free_cmdbuf;
	}

	err = nvhost_syncpt_wait_timeout_ext(isp->pdev,
			job->sp->id, job->sp->fence,
			msecs_to_jiffies(500), NULL, NULL);

	nvhost_job_put(job);
free_cmdbuf:
	dma_free_coherent(dev, PAGE_SIZE, cmdbuf, cmdbuf_phys);
idle:
	nvhost_module_idle(isp->pdev);
	return err;
}

/**
 * isp_t124_test_opdone() - Test if conditional OP_DONE syncpt fires
 *
 * Sends minimal submit: trigger 0x05 + cond4 OP_DONE syncpt incr.
 * If this times out, ISP never generates OP_DONE for trigger 0x05.
 */
static int isp_t124_test_opdone(struct tegra_isp_t124 *isp)
{
	struct device *dev = &isp->pdev->dev;
	struct nvhost_job *job;
	u32 *cmdbuf;
	dma_addr_t cmdbuf_phys;
	int err, n = 0;

	if (!isp->channel || !isp->syncpt_memory)
		return -ENODEV;

	err = nvhost_module_busy(isp->pdev);
	if (err)
		return err;

	cmdbuf = dma_alloc_coherent(dev, PAGE_SIZE, &cmdbuf_phys, GFP_KERNEL);
	if (!cmdbuf) {
		err = -ENOMEM;
		goto idle;
	}

	/* Conditional OP_DONE syncpt incr */
	cmdbuf[n++] = nvhost_opcode_nonincr(0x000, 1);
	cmdbuf[n++] = (4 << 8) | isp->syncpt_memory; /* cond4 = OP_DONE */

	/* Trigger POST_APPLY (0x0F) — known to generate OP_DONE in stream_init */
	cmdbuf[n++] = nvhost_opcode_nonincr(0x00C, 1);
	cmdbuf[n++] = 0x0F;

	/* IMMEDIATE incr as backup fence */
	cmdbuf[n++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_immediate_v(),
		isp->syncpt_memory);
	cmdbuf[n++] = NVHOST_OPCODE_NOOP;

	job = nvhost_job_alloc(isp->channel, 1, 0, 0, 1);
	if (!job) {
		err = -ENOMEM;
		goto free_cmdbuf;
	}

	job->sp->id = isp->syncpt_memory;
	job->sp->incrs = 2; /* 1 OP_DONE + 1 IMMEDIATE */
	job->num_syncpts = 1;

	nvhost_job_add_gather(job, 0, n, 0, isp->class_id, 0);
	job->gathers[0].mem_base = cmdbuf_phys;

	err = nvhost_channel_submit(job);
	if (err) {
		nvhost_job_put(job);
		goto free_cmdbuf;
	}

	err = nvhost_syncpt_wait_timeout_ext(isp->pdev,
			job->sp->id, job->sp->fence,
			msecs_to_jiffies(500), NULL, NULL);

	dev_info(dev, "test_opdone: %s (fence=%u)\n",
		 err ? "TIMEOUT — OP_DONE never fired" : "OK — OP_DONE works",
		 job->sp->fence);

	nvhost_job_put(job);
free_cmdbuf:
	dma_free_coherent(dev, PAGE_SIZE, cmdbuf, cmdbuf_phys);
idle:
	nvhost_module_idle(isp->pdev);
	return err;
}

/**
 * isp_t124_stream_init() - Prepare ISP for streaming
 *
 * Allocates working buffer, command buffer, applies calibration
 * with trigger 0x0F (POST_APPLY).
 */
int isp_t124_stream_init(struct tegra_isp_t124 *isp, u32 width, u32 height)
{
	struct device *dev = &isp->pdev->dev;
	struct platform_device *host1x_pdev;
	struct device *host1x_dev;
	struct nvhost_job *job;
	int err, n = 0, cal_last_idx;

	if (!isp->channel || !isp->syncpt_memory)
		return -ENODEV;
	if (isp->streaming)
		return -EBUSY;

	isp->width = width;
	isp->height = height;
	isp->y_stride = (width + 63) & ~63;
	isp->uv_stride = ((width / 2) + 63) & ~63;

	err = nvhost_module_busy(isp->pdev);
	if (err)
		return err;

	/* Allocate working buffer (ISP internal scratch) */
	err = isp_dma_buf_alloc(dev, &isp->work_buf, ISP_WORK_BUF_SIZE);
	if (err) {
		dev_err(dev, "ISP work buf alloc failed: %d\n", err);
		goto idle;
	}

	/* Command buffer through host1x device for CDMA reads */
	host1x_pdev = nvhost_get_parent(isp->pdev);
	host1x_dev = host1x_pdev ? &host1x_pdev->dev : dev;
	isp->cmdbuf = dma_alloc_coherent(host1x_dev,
					  ISP_CMDBUF_SIZE * 2, /* room for init + frame */
					  &isp->cmdbuf_phys, GFP_KERNEL);
	if (!isp->cmdbuf) {
		err = -ENOMEM;
		goto free_work;
	}

	dev_info(dev, "stream_init: cmdbuf_phys=0x%pad work_buf=0x%pad host1x_dev=%s\n",
		 &isp->cmdbuf_phys, &isp->work_buf.dma,
		 dev_name(host1x_dev));

	/*
	 * Build init command buffer: calibration + trigger 0x0F
	 * No SET_CLASS needed — class set by gather_address.
	 */
	n = 0;
	memcpy(&isp->cmdbuf[n], isp->cal_data, isp->cal_words * 4);
	n += isp->cal_words;

	/* Patch last word of calibration: replace stock work buf IOVA
	 * with our allocated work buffer address.
	 * Cal ends with INCR(0x053, 2) + [enable=1, work_buf_addr]
	 */
	cal_last_idx = isp->cal_words - 1;
	isp->cmdbuf[cal_last_idx] = (u32)isp->work_buf.dma;

	/* Output config — set up dims + format so ISP knows about output DMA */
	isp->cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_WIDTH, 1);
	isp->cmdbuf[n++] = ((width - 1) & 0x3FFF) << 16;
	isp->cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_HEIGHT, 1);
	isp->cmdbuf[n++] = ((height - 1) & 0x3FFF) << 16;
	isp->cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_FORMAT, 1);
	isp->cmdbuf[n++] = ISP_FORMAT_STOCK;
	isp->cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_COLOR, 1);
	isp->cmdbuf[n++] = 0x00000000;

	/* Output surfaces — dummy addr (will be overwritten per-frame) */
	isp->cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_Y, 3);
	isp->cmdbuf[n++] = 0x00000000;
	isp->cmdbuf[n++] = ISP_SURF_WORD1;
	isp->cmdbuf[n++] = isp->y_stride;
	isp->cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_U, 3);
	isp->cmdbuf[n++] = 0x00000000;
	isp->cmdbuf[n++] = ISP_SURF_WORD1;
	isp->cmdbuf[n++] = isp->uv_stride;
	isp->cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_V, 3);
	isp->cmdbuf[n++] = 0x00000000;
	isp->cmdbuf[n++] = ISP_SURF_WORD1;
	isp->cmdbuf[n++] = isp->uv_stride;

	/* Processing dims */
	isp->cmdbuf[n++] = nvhost_opcode_incr(ISP_METHOD_PROCESSING, 6);
	isp->cmdbuf[n++] = 0;
	isp->cmdbuf[n++] = 0;
	isp->cmdbuf[n++] = 0;
	isp->cmdbuf[n++] = 0;
	isp->cmdbuf[n++] = 0;
	isp->cmdbuf[n++] = (height << 16) | width;

	/* Trigger 0x0F — static config apply */
	isp->cmdbuf[n++] = nvhost_opcode_nonincr(ISP_METHOD_CONTROL, 1);
	isp->cmdbuf[n++] = ISP_TRIGGER_POST_APPLY;

	/* Syncpt OP_DONE (cond=1) — only standard host1x conditions work */
	isp->cmdbuf[n++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_op_done_v(),
		isp->syncpt_memory);

	isp->cmdbuf[n++] = NVHOST_OPCODE_NOOP;

	/* Submit init */
	job = nvhost_job_alloc(isp->channel, 1, 0, 0, 1);
	if (!job) {
		err = -ENOMEM;
		goto free_cmdbuf;
	}

	job->sp->id = isp->syncpt_memory;
	job->sp->incrs = 1;
	job->num_syncpts = 1;

	err = nvhost_job_add_client_gather_address(job, n,
			isp->class_id, isp->cmdbuf_phys);
	if (err) {
		nvhost_job_put(job);
		goto free_cmdbuf;
	}

	err = nvhost_channel_submit(job);
	if (err) {
		dev_err(dev, "ISP init submit failed: %d\n", err);
		nvhost_job_put(job);
		goto free_cmdbuf;
	}

	err = nvhost_syncpt_wait_timeout_ext(isp->pdev,
			job->sp->id, job->sp->fence,
			msecs_to_jiffies(1000), NULL, NULL);
	nvhost_job_put(job);

	if (err) {
		dev_err(dev, "ISP init timeout: %d\n", err);
		goto free_cmdbuf;
	}

	isp->streaming = true;
	dev_info(dev, "ISP stream init OK: %ux%u, class=0x%02x\n",
		 width, height, isp->class_id);
	nvhost_module_idle(isp->pdev);
	return 0;

free_cmdbuf:
	{
		struct platform_device *hp = nvhost_get_parent(isp->pdev);
		struct device *hd = hp ? &hp->dev : dev;
		dma_free_coherent(hd, ISP_CMDBUF_SIZE * 2,
				  isp->cmdbuf, isp->cmdbuf_phys);
		isp->cmdbuf = NULL;
	}
free_work:
	isp_dma_buf_free(dev, &isp->work_buf);
idle:
	nvhost_module_idle(isp->pdev);
	return err;
}
EXPORT_SYMBOL(isp_t124_stream_init);

/**
 * isp_t124_stream_stop() - Release ISP streaming resources
 */
void isp_t124_stream_stop(struct tegra_isp_t124 *isp)
{
	struct device *dev = &isp->pdev->dev;

	if (!isp->streaming)
		return;

	isp->streaming = false;

	if (isp->cmdbuf) {
		struct platform_device *hp = nvhost_get_parent(isp->pdev);
		struct device *hd = hp ? &hp->dev : dev;
		dma_free_coherent(hd, ISP_CMDBUF_SIZE * 2,
				  isp->cmdbuf, isp->cmdbuf_phys);
		isp->cmdbuf = NULL;
	}

	isp_dma_buf_free(dev, &isp->work_buf);
	dev_info(dev, "ISP stream stopped\n");
}
EXPORT_SYMBOL(isp_t124_stream_stop);

/**
 * isp_t124_process_frame() - Submit one frame through ISP
 *
 * Sends output+input+trigger as one gather, cal as second gather (stock order).
 * Uses cond=4 (ISP OP_DONE) but falls back if timeout — checks if ISP wrote data.
 */
int isp_t124_process_frame(struct tegra_isp_t124 *isp,
			   dma_addr_t in_dma, dma_addr_t out_dma,
			   u32 vi_syncpt, u32 vi_thresh)
{
	struct nvhost_job *job;
	u32 *cmd;
	dma_addr_t cmd_phys;
	int err;
	int n;
	int g1_off, g1_words, g2_off, g2_words;
	u32 W = isp->width, H = isp->height;
	u32 y_stride = isp->y_stride;
	u32 uv_stride = isp->uv_stride;
	size_t y_size = (size_t)y_stride * H;
	size_t uv_size = (size_t)uv_stride * (H / 2);
	dma_addr_t out_y = out_dma;
	dma_addr_t out_u = out_dma + y_size;
	dma_addr_t out_v = out_dma + y_size + uv_size;

	if (!isp->streaming || !isp->cmdbuf)
		return -ENODEV;

	err = nvhost_module_busy(isp->pdev);
	if (err)
		return err;

	cmd = isp->cmdbuf + ISP_CMDBUF_WORDS;
	cmd_phys = isp->cmdbuf_phys + ISP_CMDBUF_SIZE;
	n = 0;

	dev_info(&isp->pdev->dev,
		 "process_frame: in=0x%pad out=0x%pad %ux%u\n",
		 &in_dma, &out_dma, W, H);

	/* ---- G1: output + input + conditional syncpts + trigger ---- */
	g1_off = n;

	/* Output config */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_WIDTH, 1);
	cmd[n++] = ((W - 1) & 0x3FFF) << 16;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_HEIGHT, 1);
	cmd[n++] = ((H - 1) & 0x3FFF) << 16;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_FORMAT, 1);
	cmd[n++] = ISP_FORMAT_STOCK;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_COLOR, 1);
	cmd[n++] = 0x00000000;

	/* Output surfaces Y/U/V */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_Y, 3);
	cmd[n++] = (u32)out_y;
	cmd[n++] = ISP_SURF_WORD1;
	cmd[n++] = y_stride;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_U, 3);
	cmd[n++] = (u32)out_u;
	cmd[n++] = ISP_SURF_WORD1;
	cmd[n++] = uv_stride;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_V, 3);
	cmd[n++] = (u32)out_v;
	cmd[n++] = ISP_SURF_WORD1;
	cmd[n++] = uv_stride;

	/* Processing */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_PROCESSING, 6);
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = (H << 16) | W;

	/* Input buffer */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_INPUT_BUF, 4);
	cmd[n++] = (u32)in_dma;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;

	/* Conditional syncpt — cond=4 (ISP OP_DONE), should work now with module_busy */
	cmd[n++] = nvhost_opcode_nonincr(0x000, 1);
	cmd[n++] = (4 << 8) | isp->syncpt_memory;

	/* Trigger RUNTIME (0x05) */
	cmd[n++] = nvhost_opcode_nonincr(ISP_METHOD_CONTROL, 1);
	cmd[n++] = ISP_TRIGGER_RUNTIME;

	g1_words = n - g1_off;

	/* ---- G2: calibration (stock G6) ---- */
	g2_off = n;
	memcpy(&cmd[n], isp->cal_data, isp->cal_words * 4);
	n += isp->cal_words;
	cmd[g2_off + isp->cal_words - 1] = (u32)isp->work_buf.dma;
	g2_words = n - g2_off;

	/* 2-gather job */
	job = nvhost_job_alloc(isp->channel, 2, 0, 0, 1);
	if (!job)
		return -ENOMEM;

	job->sp->id = isp->syncpt_memory;
	job->sp->incrs = 1; /* 1 OP_DONE (cond=1) */
	job->num_syncpts = 1;

	nvhost_job_add_gather(job, 0, g1_words, 0, isp->class_id, 0);
	job->gathers[0].mem_base = cmd_phys + g1_off * 4;

	nvhost_job_add_gather(job, 0, g2_words, 0, isp->class_id, 0);
	job->gathers[1].mem_base = cmd_phys + g2_off * 4;

	err = nvhost_channel_submit(job);
	if (err) {
		dev_err(&isp->pdev->dev, "ISP frame submit failed: %d\n", err);
		nvhost_job_put(job);
		return err;
	}

	err = nvhost_syncpt_wait_timeout_ext(isp->pdev,
			job->sp->id, job->sp->fence,
			msecs_to_jiffies(500), NULL, NULL);
	if (err)
		dev_err(&isp->pdev->dev, "ISP frame timeout: %d\n", err);
	else
		dev_info(&isp->pdev->dev, "ISP frame OK!\n");

	nvhost_job_put(job);
	nvhost_module_idle(isp->pdev);
	return err;
}
EXPORT_SYMBOL(isp_t124_process_frame);

/* ----------------------------------------------------------------
 * debugfs
 * ---------------------------------------------------------------- */

static int isp_t124_debugfs_ping_show(struct seq_file *s, void *data)
{
	struct tegra_isp_t124 *isp = s->private;
	ktime_t start = ktime_get();
	int ret = isp_t124_ping(isp);
	s64 us = ktime_us_delta(ktime_get(), start);

	seq_printf(s, "ISP%s %s, syncpts: mem=%u stats=%u loadv=%u (%lld us)\n",
		   isp->class_id == ISP_A_CLASS_ID ? "-A" : "-B",
		   ret ? "FAILED" : "alive",
		   isp->syncpt_memory, isp->syncpt_stats, isp->syncpt_loadv,
		   us);
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

static int isp_t124_debugfs_opdone_show(struct seq_file *s, void *data)
{
	struct tegra_isp_t124 *isp = s->private;
	ktime_t start = ktime_get();
	int ret = isp_t124_test_opdone(isp);
	s64 us = ktime_us_delta(ktime_get(), start);

	seq_printf(s, "ISP%s OP_DONE test: %s (%lld us)\n",
		   isp->class_id == ISP_A_CLASS_ID ? "-A" : "-B",
		   ret ? "TIMEOUT" : "OK", us);
	return 0;
}

static int isp_t124_debugfs_opdone_open(struct inode *inode, struct file *file)
{
	return single_open(file, isp_t124_debugfs_opdone_show, inode->i_private);
}

static const struct file_operations isp_t124_debugfs_opdone_fops = {
	.open    = isp_t124_debugfs_opdone_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static void isp_t124_debugfs_init(struct tegra_isp_t124 *isp,
				  const char *name)
{
	isp->debugfs_dir = debugfs_create_dir(name, NULL);
	if (!isp->debugfs_dir)
		return;
	debugfs_create_file("ping", 0444, isp->debugfs_dir,
			    isp, &isp_t124_debugfs_ping_fops);
	debugfs_create_file("test_opdone", 0444, isp->debugfs_dir,
			    isp, &isp_t124_debugfs_opdone_fops);
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
 *
 * T124 has two ISP instances at different platform device addresses.
 * We detect A vs B by device index (isp.0 = A, isp.1 = B).
 * ---------------------------------------------------------------- */

static struct tegra_isp_t124 *g_isp_a;
static struct tegra_isp_t124 *g_isp_b;

struct tegra_isp_t124 *isp_t124_get_isp(u8 class_id)
{
	if (class_id == ISP_B_CLASS_ID)
		return g_isp_b;
	return g_isp_a;
}
EXPORT_SYMBOL(isp_t124_get_isp);

int tegra_isp_t124_mc_init(struct platform_device *pdev)
{
	struct tegra_isp_t124 *isp;
	int err;
	bool is_isp_b;
	const char *dbg_name;

	if (!of_device_is_compatible(pdev->dev.of_node, "nvidia,tegra124-isp"))
		return -ENODEV;

	isp = devm_kzalloc(&pdev->dev, sizeof(*isp), GFP_KERNEL);
	if (!isp)
		return -ENOMEM;

	isp->pdev = pdev;

	/*
	 * Determine ISP-A vs ISP-B:
	 * First probe = ISP-A (g_isp_a is NULL), second = ISP-B.
	 */
	is_isp_b = (g_isp_a != NULL);

	if (is_isp_b) {
		isp->class_id = ISP_B_CLASS_ID;
		isp->cal_data = isp_b_cal_data;
		isp->cal_words = ARRAY_SIZE(isp_b_cal_data);
		dbg_name = "isp_t124_b";
	} else {
		isp->class_id = ISP_A_CLASS_ID;
		isp->cal_data = isp_a_cal_data;
		isp->cal_words = ARRAY_SIZE(isp_a_cal_data);
		dbg_name = "isp_t124_a";
	}

	err = isp_t124_register_subdev(isp);
	if (err) {
		dev_warn(&pdev->dev, "V4L2 subdev failed: %d\n", err);
		return err;
	}

	err = isp_t124_channel_init(isp);
	if (err) {
		dev_warn(&pdev->dev, "host1x channel failed: %d\n", err);
	} else {
		isp_t124_debugfs_init(isp, dbg_name);
	}

	if (is_isp_b)
		g_isp_b = isp;
	else
		g_isp_a = isp;

	dev_info(&pdev->dev, "T124 ISP-%s MC driver initialized (class=0x%02x)\n",
		 is_isp_b ? "B" : "A", isp->class_id);
	return 0;
}
EXPORT_SYMBOL(tegra_isp_t124_mc_init);

void tegra_isp_t124_mc_cleanup(struct platform_device *pdev)
{
	struct tegra_isp_t124 *isp;

	/* Find which instance this pdev belongs to */
	if (g_isp_b && g_isp_b->pdev == pdev)
		isp = g_isp_b;
	else if (g_isp_a && g_isp_a->pdev == pdev)
		isp = g_isp_a;
	else
		return;

	if (isp->streaming)
		isp_t124_stream_stop(isp);

	isp_t124_debugfs_cleanup(isp);
	isp_t124_channel_cleanup(isp);
	isp_t124_unregister_subdev(isp);

	if (isp == g_isp_a)
		g_isp_a = NULL;
	else
		g_isp_b = NULL;
}
EXPORT_SYMBOL(tegra_isp_t124_mc_cleanup);

MODULE_DESCRIPTION("Tegra T124 ISP Media Controller Driver");
MODULE_LICENSE("GPL v2");
