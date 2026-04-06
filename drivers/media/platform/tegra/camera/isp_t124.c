/*
 * Tegra T124 ISP Media Controller Driver
 *
 * Copyright (c) 2025-2026, Smoke Team. All rights reserved.
 *
 * V4L2 subdev driver for T124 ISP-A and ISP-B.
 * Provides runtime API for VI→ISP capture pipeline integration.
 *
 * ISP-A (class 0x32): isp.0, rear IMX179, 3280x2460
 * ISP-B (class 0x34): isp.1, front OV5693, 2592x1944
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
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
#include "isp_trace.h"

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
	buf->page = NULL;
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

	/* Register as client — stock does this in nvhost_channelopen.
	 * Required for proper power management and ACM tracking. */
	err = nvhost_module_add_client(isp->pdev, isp);
	if (err) {
		dev_err(&isp->pdev->dev, "module_add_client failed: %d\n", err);
		return err;
	}

	/* ISP has keepalive=true — disable automatic poweroff.
	 * Stock userspace does this at channel open time. */
	if (pdata->keepalive)
		nvhost_module_disable_poweroff(isp->pdev);

	err = nvhost_channel_map(pdata, &isp->channel, isp);
	if (err) {
		dev_err(&isp->pdev->dev, "host1x channel map failed: %d\n",
			err);
		return err;
	}

	/* 4 syncpoints per ISP: memory (0), stats (1), stream (2), loadv (3) */
	isp->syncpt_memory = nvhost_get_syncpt_host_managed(isp->pdev,
							    0, "isp_memory");
	isp->syncpt_stats = nvhost_get_syncpt_host_managed(isp->pdev,
							   1, "isp_stats");
	isp->syncpt_stream = nvhost_get_syncpt_host_managed(isp->pdev,
							    2, "isp_stream");
	isp->syncpt_loadv = nvhost_get_syncpt_host_managed(isp->pdev,
							   3, "isp_loadv");

	if (!isp->syncpt_memory || !isp->syncpt_stats ||
	    !isp->syncpt_stream || !isp->syncpt_loadv) {
		dev_err(&isp->pdev->dev, "syncpt allocation failed\n");
		goto fail;
	}

	dev_info(&isp->pdev->dev,
		 "host1x channel mapped, syncpts: mem=%u stats=%u stream=%u loadv=%u\n",
		 isp->syncpt_memory, isp->syncpt_stats,
		 isp->syncpt_stream, isp->syncpt_loadv);
	return 0;

fail:
	if (isp->syncpt_memory)
		nvhost_syncpt_put_ref_ext(isp->pdev, isp->syncpt_memory);
	if (isp->syncpt_stats)
		nvhost_syncpt_put_ref_ext(isp->pdev, isp->syncpt_stats);
	if (isp->syncpt_stream)
		nvhost_syncpt_put_ref_ext(isp->pdev, isp->syncpt_stream);
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
	if (isp->syncpt_stream) {
		nvhost_syncpt_put_ref_ext(isp->pdev, isp->syncpt_stream);
		isp->syncpt_stream = 0;
	}
	if (isp->syncpt_loadv) {
		nvhost_syncpt_put_ref_ext(isp->pdev, isp->syncpt_loadv);
		isp->syncpt_loadv = 0;
	}
	if (isp->channel) {
		nvhost_putchannel(isp->channel, 1);
		isp->channel = NULL;
	}
	nvhost_module_enable_poweroff(isp->pdev);
	nvhost_module_remove_client(isp->pdev, isp);
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

	if (!isp->channel || !isp->syncpt_stream)
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
		isp->syncpt_stream);
	cmdbuf[n++] = NVHOST_OPCODE_NOOP;

	job = nvhost_job_alloc(isp->channel, 1, 0, 0, 1);
	if (!job) {
		err = -ENOMEM;
		goto free_cmdbuf;
	}

	job->sp->id = isp->syncpt_stream;
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

	if (!isp->channel || !isp->syncpt_stream)
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
	cmdbuf[n++] = (4 << 8) | isp->syncpt_stream; /* cond4 = OP_DONE */

	/* Trigger POST_APPLY (0x0F) — known to generate OP_DONE in stream_init */
	cmdbuf[n++] = nvhost_opcode_nonincr(0x00C, 1);
	cmdbuf[n++] = 0x0F;

	/* IMMEDIATE incr as backup fence */
	cmdbuf[n++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_immediate_v(),
		isp->syncpt_stream);
	cmdbuf[n++] = NVHOST_OPCODE_NOOP;

	job = nvhost_job_alloc(isp->channel, 1, 0, 0, 1);
	if (!job) {
		err = -ENOMEM;
		goto free_cmdbuf;
	}

	job->sp->id = isp->syncpt_stream;
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


/* Helper: submit a command buffer and wait for cond=1 OP_DONE */
static int isp_submit_and_wait(struct tegra_isp_t124 *isp,
			       u32 *cmdbuf, dma_addr_t phys,
			       int words, const char *name)
{
	struct nvhost_job *job;
	int err;

	job = nvhost_job_alloc(isp->channel, 1, 0, 0, 1);
	if (!job)
		return -ENOMEM;

	job->sp->id = isp->syncpt_stream;
	job->sp->incrs = 1;
	job->num_syncpts = 1;

	err = nvhost_job_add_client_gather_address(job, words,
			isp->class_id, phys);
	if (err) {
		nvhost_job_put(job);
		return err;
	}

	err = nvhost_channel_submit(job);
	if (err) {
		dev_err(&isp->pdev->dev, "ISP %s submit failed: %d\n",
			name, err);
		nvhost_job_put(job);
		return err;
	}

	err = nvhost_syncpt_wait_timeout_ext(isp->pdev,
			job->sp->id, job->sp->fence,
			msecs_to_jiffies(1000), NULL, NULL);
	nvhost_job_put(job);

	if (err)
		dev_err(&isp->pdev->dev, "ISP %s timeout: %d\n", name, err);
	else
		dev_info(&isp->pdev->dev, "ISP %s OK\n", name);

	return err;
}

/* Build stock zero-init block (1813 words) — all ISP registers zeroed */
static int isp_build_zero_init(u32 *buf)
{
	int n = 0;
#define ZI(off, cnt) do { \
	buf[n++] = nvhost_opcode_incr(off, cnt); \
	memset(&buf[n], 0, (cnt) * 4); n += (cnt); \
} while (0)
#define ZN(off, cnt) do { \
	buf[n++] = nvhost_opcode_nonincr(off, cnt); \
	memset(&buf[n], 0, (cnt) * 4); n += (cnt); \
} while (0)
	ZI(0x202, 3); ZI(0x200, 2); ZI(0x205, 4);
	ZI(0x700, 16); ZI(0x750, 16);
	/* Processing (500-505), ISP_ENABLE (015) */
	ZI(0x500, 6); ZI(0x015, 1);
	ZI(0xd00, 10); ZI(0xd0a, 1); ZN(0xd0b, 480);
	ZI(0xd0c, 2); ZI(0xd20, 6);
	ZI(0x900, 2); ZI(0x902, 1); ZN(0x903, 64);
	ZI(0x904, 2); ZI(0x906, 1); ZN(0x907, 36);
	ZI(0x908, 1); ZI(0x920, 10); ZI(0x909, 7);
	ZI(0x910, 9); ZI(0x919, 1); ZN(0x91a, 9);
	ZI(0x91b, 1); ZN(0x91c, 9);
	ZI(0x91d, 1); ZN(0x91e, 9);
	buf[n++] = nvhost_opcode_incr(0x91f, 1);
	buf[n++] = 0x00000002; /* stock value */
	ZI(0x506, 9); ZI(0x600, 16); ZI(0x650, 1);
	ZI(0x651, 1); ZN(0x652, 257);
	ZI(0x653, 1); ZN(0x654, 257);
	ZI(0x655, 1); ZN(0x656, 257);
	ZI(0x657, 1); ZN(0x658, 257);
	ZI(0x300, 4); ZI(0x304, 4);
	ZI(0x053, 1); ZI(0x054, 1);
#undef ZI
#undef ZN
	return n; /* 1813 */
}

/* Append zero-init block + trigger. Returns new n. */
static int isp_append_zero_block(struct tegra_isp_t124 *isp, u32 *buf, int n)
{
	u32 safe = (u32)isp->work_buf.dma;
	int zi = isp_build_zero_init(&buf[n]);
	/* Patch 0x053 (ISP global enable) and 0x054 (work_buf IOVA).
	 * ZI(0x053,1)+ZI(0x054,1) = last 4 words: [opcode][0x053_data][opcode][0x054_data]
	 * buf[zi-1] = 0x054 data, buf[zi-3] = 0x053 data
	 * Stock: 0x053 = 1 (ISP enable ON), 0x054 = work_buf IOVA */
	buf[n + zi - 3] = 0x00000001; /* 0x053: ISP global enable = 1 */
	buf[n + zi - 1] = safe;       /* 0x054: work_buf IOVA */
	n += zi;

	/* Set output/input/stats surface DMA addresses to safe value (work_buf)
	 * to prevent ISP writing to stale bootloader addresses.
	 * 0xE00-0xE03 = width,height,format,color — leave zero
	 * 0xE04,0xE07,0xE0A = Y,U,V surface addr — set to safe */
	buf[n++] = nvhost_opcode_incr(0xE00, 11);
	buf[n++] = 0;    /* 0xE00: width */
	buf[n++] = 0;    /* 0xE01: height */
	buf[n++] = 0;    /* 0xE02: format */
	buf[n++] = 0;    /* 0xE03: color */
	buf[n++] = safe; /* 0xE04: Y addr */
	buf[n++] = 0;    /* 0xE05: Y unk */
	buf[n++] = 0;    /* 0xE06: Y stride */
	buf[n++] = safe; /* 0xE07: U addr */
	buf[n++] = 0;    /* 0xE08: U unk */
	buf[n++] = 0;    /* 0xE09: U stride */
	buf[n++] = safe; /* 0xE0A: V addr */
	/* 0xE30: input surfaces — same layout, only set addr fields */
	buf[n++] = nvhost_opcode_incr(0xE30, 11);
	buf[n++] = 0;    buf[n++] = 0;    buf[n++] = 0;    buf[n++] = 0;
	buf[n++] = safe; buf[n++] = 0;    buf[n++] = 0;
	buf[n++] = safe; buf[n++] = 0;    buf[n++] = 0;
	buf[n++] = safe;
	/* 0x100: stats buffer addr */
	buf[n++] = nvhost_opcode_incr(0x100, 4);
	buf[n++] = safe; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;

	buf[n++] = nvhost_opcode_nonincr(ISP_METHOD_CONTROL, 1);
	buf[n++] = ISP_TRIGGER_POST_APPLY;
	return n;
}

/* Append real calibration + trigger. Returns new n. */
static int isp_append_cal_block(struct tegra_isp_t124 *isp, u32 *buf, int n)
{
	memcpy(&buf[n], isp->cal_data, isp->cal_words * 4);
	n += isp->cal_words;
	/* Cal data ends with INCR(0x053, 2) + [val, val].
	 * Patch: 0x053 = 1 (ISP global enable), 0x054 = work_buf IOVA */
	buf[n - 2] = 0x00000001;              /* 0x053: ISP global enable = 1 */
	buf[n - 1] = (u32)isp->work_buf.dma;  /* 0x054: work_buf IOVA */
	buf[n++] = nvhost_opcode_nonincr(ISP_METHOD_CONTROL, 1);
	buf[n++] = ISP_TRIGGER_POST_APPLY;
	return n;
}

/* Append syncpt IMMEDIATE incr + NOOP — used for init submits (S1-S5). */
static int isp_append_syncpt(struct tegra_isp_t124 *isp, u32 *buf, int n)
{
	buf[n++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_immediate_v(),
		isp->syncpt_stream);
	buf[n++] = NVHOST_OPCODE_NOOP;
	return n;
}

/**
 * isp_t124_stream_init() - Prepare ISP for streaming
 *
 * Reproduces exact stock init sequence: 5 submits.
 * S1: cal + 0x018 tail + cal + 0x018 tail2
 * S2: cal + trigger
 * S3: SET_CLASS only
 * S4: cal + trigger (repeat)
 * S5: runtime config (0x400, 0x800, 0x930, 0x506, cal, trigger)
 */
int isp_t124_stream_init(struct tegra_isp_t124 *isp, u32 width, u32 height,
			 bool reprocess)
{
	struct device *dev = &isp->pdev->dev;
	struct platform_device *host1x_pdev;
	struct device *host1x_dev;
	u32 *cmd;
	dma_addr_t cmd_phys;
	int err, n;

	if (!isp->channel || !isp->syncpt_stream)
		return -ENODEV;
	if (isp->streaming)
		return -EBUSY;

	/* ISP output = sensor resolution (stock behavior).
	 * ISP does NOT downscale — it processes at full sensor res.
	 * Downscale to user-requested resolution happens later (GPU/scaler). */
	if (isp->class_id == ISP_A_CLASS_ID) {
		isp->width = 3280;
		isp->height = 2464;
	} else {
		isp->width = 2592;
		isp->height = 1944;
	}
	isp->y_stride = (isp->width + 63) & ~63;
	isp->uv_stride = ((isp->width / 2) + 63) & ~63;
	isp->in_stride = width * 2;  /* RAW10 packed to 16-bit = 2 bytes/pixel */
	isp->in_format = 0x11000020; /* RAW Bayer single-plane linear (from RE) */

	err = nvhost_module_busy(isp->pdev);
	if (err)
		return err;

	/* PIO write: stock does this before first submit */
	host1x_writel(isp->pdev, 0x00fc, 0x00000020);

	/* Allocate working buffer */
	err = isp_dma_buf_alloc(dev, &isp->work_buf, ISP_WORK_BUF_SIZE);
	if (err) {
		dev_err(dev, "ISP work buf alloc failed: %d\n", err);
		goto idle;
	}
	dev_info(dev, "work_buf: iova=0x%pad size=%zu\n",
		 &isp->work_buf.dma, isp->work_buf.size);

	/* Command buffer — 16KB total for all submits */
	host1x_pdev = nvhost_get_parent(isp->pdev);
	host1x_dev = host1x_pdev ? &host1x_pdev->dev : dev;
	isp->cmdbuf = dma_alloc_coherent(host1x_dev,
					  ISP_CMDBUF_SIZE * 2,
					  &isp->cmdbuf_phys, GFP_KERNEL);
	if (!isp->cmdbuf) {
		err = -ENOMEM;
		goto free_work;
	}

	cmd = isp->cmdbuf;
	cmd_phys = isp->cmdbuf_phys;

	dev_info(dev, "stream_init: %ux%u cmdbuf=0x%pad work=0x%pad\n",
		 width, height, &cmd_phys, &isp->work_buf.dma);

	/* S1 (stock 3654w): zero_block×2 + 0x018 tails + syncpt */
	n = 0;
	n = isp_append_zero_block(isp, cmd, n);
	cmd[n++] = nvhost_opcode_incr(0x018, 5);
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000400;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000200;
	cmd[n++] = 0x00000002;
	n = isp_append_zero_block(isp, cmd, n);
	cmd[n++] = nvhost_opcode_incr(0x018, 5);
	cmd[n++] = 0x0a00500a; cmd[n++] = 0x00008089;
	cmd[n++] = 0x013645cb; cmd[n++] = 0x000001e7;
	cmd[n++] = 0x00000001;
	n = isp_append_syncpt(isp, cmd, n);
	dev_info(dev, "S1: %d words\n", n);

	err = isp_submit_and_wait(isp, cmd, cmd_phys, n, "S1-init");
	if (err)
		goto free_cmdbuf;

	/* S2 (stock 1817w): zero_block + trigger + syncpt */
	n = 0;
	n = isp_append_zero_block(isp, cmd, n);
	n = isp_append_syncpt(isp, cmd, n);

	err = isp_submit_and_wait(isp, cmd, cmd_phys, n, "S2-cal");
	if (err)
		goto free_cmdbuf;

	/* S3 (stock 1+2w): SET_CLASS + syncpt */
	n = 0;
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_op_done_v(),
		isp->syncpt_stream);
	cmd[n++] = NVHOST_OPCODE_NOOP;

	err = isp_submit_and_wait(isp, cmd, cmd_phys, n, "S3-class");
	if (err)
		goto free_cmdbuf;

	/* S4 (stock 1817w): zero_block + trigger + syncpt */
	n = 0;
	n = isp_append_zero_block(isp, cmd, n);
	n = isp_append_syncpt(isp, cmd, n);

	err = isp_submit_and_wait(isp, cmd, cmd_phys, n, "S4-cal");
	if (err)
		goto free_cmdbuf;

	/* S5 (stock 1238w): full runtime config from stock trace + cal + trigger
	 * Values differ between ISP-A (isp.0, IMX179 rear) and ISP-B (isp.1, OV5693 front).
	 * Each register group preceded by SET_CLASS per stock trace. */
	n = 0;
	{
	bool is_b = (isp->class_id == ISP_B_CLASS_ID);

	/* 0x400: runtime config (12 words) */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_RT_CONFIG, 12);
	cmd[n++] = 0x00000001;
	cmd[n++] = 0x004b0000;
	cmd[n++] = 0x00930000;
	cmd[n++] = 0x00220000;
	cmd[n++] = (u32)isp->work_buf.dma + 0x10000;
	cmd[n++] = (u32)isp->work_buf.dma + 0x10000;
	cmd[n++] = (u32)isp->work_buf.dma + 0x10000;
	cmd[n++] = (u32)isp->work_buf.dma + 0x10000;
	cmd[n++] = 0x00030000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00020000;
	cmd[n++] = 0x00000000;

	/* 0x800: stats buffer A */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_RT_BUF_A, 3);
	cmd[n++] = (u32)isp->work_buf.dma;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;

	/* 0x820: stats buffer B */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_RT_BUF_B, 3);
	cmd[n++] = (u32)isp->work_buf.dma;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;

	/* 0x930: histogram config (18 words) */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_incr(0x930, 18);
	cmd[n++] = 0x0000001c; cmd[n++] = 0x88888888;
	cmd[n++] = 0x78787800; cmd[n++] = 0x00000078;
	cmd[n++] = 0x88888888; cmd[n++] = 0x78787800;
	cmd[n++] = 0x00000078; cmd[n++] = 0x88888888;
	cmd[n++] = 0x78787800; cmd[n++] = 0x00000078;
	cmd[n++] = 0x88888888; cmd[n++] = 0x78787800;
	cmd[n++] = 0x00000078; cmd[n++] = 0x3fc00000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00070000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00070000;

	/* 0xC00: extra config */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_RT_EXTRA, 3);
	cmd[n++] = 0x00000101;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00100000;

	/* 0x202: input config — sensor-specific dims */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_incr(0x202, 3);
	cmd[n++] = 0x00000001;
	cmd[n++] = is_b ? 0x00780078 : 0x02000200; /* 0x203 */
	cmd[n++] = is_b ? 0x00780078 : 0x02000200; /* 0x204 */

	/* 0x200: input enable */
	cmd[n++] = nvhost_opcode_incr(0x200, 2);
	cmd[n++] = 0x00000001;
	cmd[n++] = 0x00000000;

	/* 0x205: input stride/format config */
	cmd[n++] = nvhost_opcode_incr(0x205, 4);
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x000600c8;
	cmd[n++] = 0x000f000f;
	cmd[n++] = is_b ? 0x00000000 : 0x00003333; /* 0x208 */

	/* 0x700: processing channel A (16 words) — sensor-specific strides */
	cmd[n++] = nvhost_opcode_incr(0x700, 16);
	cmd[n++] = 0x00000001; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = is_b ? 0x00001a40 : 0x00001dc0; /* 0x705 */
	cmd[n++] = 0x00000000; cmd[n++] = (u32)isp->work_buf.dma + 0x30000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00001000;
	cmd[n++] = is_b ? 0x00001a00 : 0x00001c50; /* 0x70b */
	cmd[n++] = (u32)isp->work_buf.dma + 0x20000; cmd[n++] = (u32)isp->work_buf.dma + 0x20000;
	cmd[n++] = (u32)isp->work_buf.dma + 0x20000; cmd[n++] = (u32)isp->work_buf.dma + 0x20000;

	/* 0x750: processing channel B (16 words) */
	cmd[n++] = nvhost_opcode_incr(0x750, 16);
	cmd[n++] = 0x00000003; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = (u32)isp->work_buf.dma + 0x20000; cmd[n++] = (u32)isp->work_buf.dma + 0x20000;
	cmd[n++] = (u32)isp->work_buf.dma + 0x20000; cmd[n++] = (u32)isp->work_buf.dma + 0x20000;

	/* 0xd20: lens shading extra — sensor-specific */
	cmd[n++] = nvhost_opcode_incr(0xd20, 6);
	cmd[n++] = is_b ? 0x00001101 : 0x00003101; /* 0xd20 */
	cmd[n++] = 0x00000000;
	cmd[n++] = is_b ? 0x00210000 : 0x01ec0000; /* 0xd22 */
	cmd[n++] = is_b ? 0x00210000 : 0x01ec0000; /* 0xd23 */
	cmd[n++] = is_b ? 0x00210000 : 0x01ec0000; /* 0xd24 */
	cmd[n++] = is_b ? 0x00210000 : 0x01ec0000; /* 0xd25 */

	/* 0x900: stats enable */
	cmd[n++] = nvhost_opcode_incr(0x900, 2);
	cmd[n++] = 0x00000001;
	cmd[n++] = 0x00000001;

	/* 0x904/0x908: stats config */
	cmd[n++] = nvhost_opcode_incr(0x904, 2);
	cmd[n++] = 0x00005555;
	cmd[n++] = 0x00000001;
	cmd[n++] = nvhost_opcode_incr(0x908, 1);
	cmd[n++] = 0x00005555;

	/* 0x920: stats window (10 words) */
	cmd[n++] = nvhost_opcode_incr(0x920, 10);
	cmd[n++] = 0x00000002; cmd[n++] = (u32)isp->work_buf.dma + 0x31660;
	cmd[n++] = 0x00000000; cmd[n++] = (u32)isp->work_buf.dma + 0x3f4a0;
	cmd[n++] = 0x0000fa80; cmd[n++] = (u32)isp->work_buf.dma + 0x30000;
	cmd[n++] = 0x00001c50; cmd[n++] = (u32)isp->work_buf.dma + 0x20000;
	cmd[n++] = (u32)isp->work_buf.dma + 0x20000; cmd[n++] = (u32)isp->work_buf.dma + 0x20000;

	/* 0x909: stats config (7 words) — sensor-specific */
	cmd[n++] = nvhost_opcode_incr(0x909, 7);
	cmd[n++] = 0x00000001; cmd[n++] = 0xfc000f00;
	cmd[n++] = 0xf680f320; cmd[n++] = 0x0d80fde0;
	cmd[n++] = is_b ? 0x00000030 : 0x00000000; /* 0x90d */
	cmd[n++] = 0x1400002a;
	cmd[n++] = 0x3c00002b;

	/* 0x910: stats config (9 words) — sensor-specific */
	cmd[n++] = nvhost_opcode_incr(0x910, 9);
	cmd[n++] = 0x00000003; cmd[n++] = 0x00000028;
	cmd[n++] = 0x01480029;
	cmd[n++] = is_b ? 0x0003030b : 0x00177e0b; /* 0x913 */
	cmd[n++] = 0x00990030; cmd[n++] = 0x00000800;
	cmd[n++] = 0x007b0666;
	cmd[n++] = is_b ? 0x00000036 : 0x00000039; /* 0x917 */
	cmd[n++] = is_b ? 0x00001f1f : 0x00000000; /* 0x918 */

	/* 0x91b: from stock */
	cmd[n++] = nvhost_opcode_incr(0x91b, 1);
	cmd[n++] = 0x00000000;

	/* 0x91c: NONINCR 9 words — sensor-specific */
	cmd[n++] = nvhost_opcode_nonincr(0x91c, 9);
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000001;
	cmd[n++] = is_b ? 0x00000025 : 0x00000026; /* word 5 */
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000026;
	cmd[n++] = 0x00000361;

	/* 0x91d: from stock */
	cmd[n++] = nvhost_opcode_incr(0x91d, 1);
	cmd[n++] = 0x00000000;

	/* 0x91e: NONINCR 9 words */
	cmd[n++] = nvhost_opcode_nonincr(0x91e, 9);
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000780;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000780;
	cmd[n++] = 0x00000200;

	/* 0x91f: from stock */
	cmd[n++] = nvhost_opcode_incr(0x91f, 1);
	cmd[n++] = 0x00000032;

	/* 0x506: demosaic processing (9 words) — same both ISPs */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_PROCESSING2, 9);
	cmd[n++] = 0x3f3fcff3;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x04c1304c;
	cmd[n++] = 0x08220882;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x03d0f43d;
	cmd[n++] = 0x08621886;
	cmd[n++] = 0x01204812;
	cmd[n++] = 0x06e1b86e;

	/* 0x600: GPP config (16 words) */
	cmd[n++] = nvhost_opcode_incr(0x600, 16);
	cmd[n++] = 0x00000005; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x3fff0000; cmd[n++] = 0x3fff0000;
	cmd[n++] = 0x3fff0000; cmd[n++] = (u32)isp->work_buf.dma + 0x31000;

	/* 0x650: tone curve enable */
	cmd[n++] = nvhost_opcode_incr(0x650, 1);
	cmd[n++] = 0x00000003;

	/* 0x651: from stock */
	cmd[n++] = nvhost_opcode_incr(0x651, 1);
	cmd[n++] = 0x00000000;

	/* ISP_ENABLE = 0x04040007 (stats/streaming mode)
	 * Ghidra RE: stock writes this on first output submit via cached check.
	 * Without it, ISP_ENABLE=0 from zero_init → ISP disabled. */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_incr(0x015, 1);
	cmd[n++] = 0x04040007;

	} /* end is_b scope */

	/* Real calibration (lens shading + tone curves) + trigger */
	n = isp_append_cal_block(isp, cmd, n);
	n = isp_append_syncpt(isp, cmd, n);

	isp_trace_log("S5 %d words, syncpts: mem=%u stats=%u stream=%u loadv=%u",
		      n, isp->syncpt_memory, isp->syncpt_stats,
		      isp->syncpt_stream, isp->syncpt_loadv);
	isp_trace_hex("S5", cmd, n > 256 ? 256 : n);

	err = isp_submit_and_wait(isp, cmd, cmd_phys, n, "S5-rtcfg");
	if (err)
		goto free_cmdbuf;

	/* S6+S7: Only for streaming mode (warmup needs VI pixel path).
	 * Reprocess mode sets ISP_ENABLE per-frame and doesn't need warmup. */
	if (!reprocess) {
	/* S6: Histogram config submit (stock 25 words — between init and first frame)
	 * Sets 0x930 histogram + ISP enable. Stock does this from userspace. */
	n = 0;
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_incr(0x930, 18);
	cmd[n++] = 0x0000001c; cmd[n++] = 0x88888888;
	cmd[n++] = 0x78787800; cmd[n++] = 0x00000078;
	cmd[n++] = 0x88888888; cmd[n++] = 0x78787800;
	cmd[n++] = 0x00000078; cmd[n++] = 0x88888888;
	cmd[n++] = 0x78787800; cmd[n++] = 0x00000078;
	cmd[n++] = 0x88888888; cmd[n++] = 0x78787800;
	cmd[n++] = 0x00000078; cmd[n++] = 0x3fc00000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00070000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00070000;
	/* SET_CLASS × 2 + ISP enable */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_incr(0x053, 2);
	cmd[n++] = 0x00000001; /* ISP enable = 1 */
	cmd[n++] = 0x00000000; /* 0x054 = 0 (stock per-frame value) */

	n = isp_append_syncpt(isp, cmd, n);
	err = isp_submit_and_wait(isp, cmd, cmd_phys, n, "S6-hist");
	if (err)
		goto free_cmdbuf;

	/* S7: Warmup 8×8 frame (stock does this before real frames)
	 * ISP_ENABLE=0x04040007 + processing flags=3 + 8×8 output
	 * This initializes ISP streaming pipeline. */
	n = 0;
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);

	/* Output 8×8 */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_WIDTH, 1);
	cmd[n++] = 0x00070000; /* (8-1) << 16 */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_HEIGHT, 1);
	cmd[n++] = 0x00070000;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_FORMAT, 1);
	cmd[n++] = 0x010000c9; /* stock warmup format */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_COLOR, 1);
	cmd[n++] = 0x00000000;

	/* Output surface Y only (8×8, stride=256, into work_buf) */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_Y, 3);
	cmd[n++] = (u32)isp->work_buf.dma + 0x40000; /* safe offset in work_buf */
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000100; /* stride = 256 */

	/* Processing: flags=3, stock warmup values, dim=8×8 */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_PROCESSING, 6);
	cmd[n++] = 0x00000003;
	cmd[n++] = 0x00000ca4;
	cmd[n++] = 0x14400000;
	cmd[n++] = 0x0f300000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00080008; /* (8 << 16) | 8 */

	/* ISP_ENABLE = streaming mode */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_ENABLE, 1);
	cmd[n++] = 0x04040007;

	/* Stats buffer (work_buf) */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_STATS_BUF, 4);
	cmd[n++] = (u32)isp->work_buf.dma;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;

	/* Conditional syncpt incrs (cond=4, 5, 6) */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_nonincr(0x000, 1);
	cmd[n++] = (ISP_SYNCPT_COND_OP_DONE << 8) | isp->syncpt_memory;
	cmd[n++] = nvhost_opcode_nonincr(0x000, 1);
	cmd[n++] = (ISP_SYNCPT_COND_STATS_DONE << 8) | isp->syncpt_stats;
	cmd[n++] = nvhost_opcode_nonincr(0x000, 1);
	cmd[n++] = (ISP_SYNCPT_COND_RD_DONE << 8) | isp->syncpt_loadv;

	/* Trigger */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_nonincr(ISP_METHOD_CONTROL, 1);
	cmd[n++] = ISP_TRIGGER_RUNTIME;

	{
		int g1w = n;
		int g2_off = n;
		struct nvhost_job *wjob;

		/* G[1]: immediate syncpt */
		cmd[n++] = nvhost_opcode_imm_incr_syncpt(
			host1x_uclass_incr_syncpt_cond_immediate_v(),
			isp->syncpt_stream);
		cmd[n++] = NVHOST_OPCODE_NOOP;

		wjob = nvhost_job_alloc(isp->channel, 2, 0, 0, 4);
		if (!wjob) {
			err = -ENOMEM;
			goto free_cmdbuf;
		}
		wjob->sp[0].id = isp->syncpt_memory;
		wjob->sp[0].incrs = 1;
		wjob->sp[1].id = isp->syncpt_stats;
		wjob->sp[1].incrs = 1;
		wjob->sp[2].id = isp->syncpt_loadv;
		wjob->sp[2].incrs = 1;
		wjob->sp[3].id = isp->syncpt_stream;
		wjob->sp[3].incrs = 1;
		wjob->num_syncpts = 4;

		nvhost_job_add_gather(wjob, 0, g1w, 0, isp->class_id, 0);
		wjob->gathers[0].mem_base = cmd_phys;

		nvhost_job_add_gather(wjob, 0, 2, 0, isp->class_id, 0);
		wjob->gathers[1].mem_base = cmd_phys + g2_off * 4;

		err = nvhost_channel_submit(wjob);
		if (err) {
			dev_err(dev, "ISP warmup submit failed: %d\n", err);
			nvhost_job_put(wjob);
			goto free_cmdbuf;
		}

		/* Wait for warmup frame completion (OP_DONE) */
		err = nvhost_syncpt_wait_timeout_ext(isp->pdev,
				isp->syncpt_memory, wjob->sp[0].fence,
				msecs_to_jiffies(500), NULL, NULL);
		if (err)
			dev_warn(dev, "ISP warmup timeout: %d\n", err);
		else
			dev_info(dev, "ISP S7-warmup OK\n");

		nvhost_job_put(wjob);
	}
	} /* end !reprocess */

	isp->streaming = true;
	isp->reprocess = reprocess;

	/* Stock has ~190ms gap between ISP init and first VI submit.
	 * ISP needs time to fully initialize pipeline after S5-S7.
	 * Without this delay, first frames may arrive before ISP is ready. */
	if (!reprocess)
		msleep(200);

	dev_info(dev, "ISP stream init OK: %ux%u, class=0x%02x (%s)\n",
		 width, height, isp->class_id,
		 reprocess ? "reprocess" : "streaming");
	if (!reprocess)
		nvhost_module_idle(isp->pdev);
	/* Reprocess: keep powered — ISP state lost on power cycle */
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

	if (isp->reprocess)
		nvhost_module_idle(isp->pdev);

	isp->reprocess = false;
	dev_info(dev, "ISP stream stopped\n");
}
EXPORT_SYMBOL(isp_t124_stream_stop);

/**
 * isp_t124_process_frame() - Submit one frame through ISP (streaming mode)
 *
 * Stock per-frame sequence (from isp_trace gather dump):
 *   G[0] 45 words: SET_CLASS + output(0xE00-0xE0A) + processing(0x500)
 *        + stats(0x100) + syncpt incrs(cond=4,5,6) + trigger(0x00C=0x05)
 *   G[1] 2 words: immediate syncpt incr (stream)
 *
 * Stock does NOT write ISP_ENABLE (0x015) per-frame — it's set once in S5.
 * Stock processing 0x500 word[0] = 0, dimension = input (sensor) resolution.
 */
int isp_t124_process_frame(struct tegra_isp_t124 *isp,
			   dma_addr_t out_dma, dma_addr_t stats_dma)
{
	struct nvhost_job *job;
	u32 *cmd;
	dma_addr_t cmd_phys;
	int err;
	int n;
	int cal_off, cal_words, cal_sp_off;
	int g1_off, g1_words, g2_off, g2_words, g3_off;
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

	dev_dbg(&isp->pdev->dev,
		"frame: out=0x%08x Y=0x%08x U=0x%08x V=0x%08x stats=0x%08x\n",
		(u32)out_dma, (u32)out_y, (u32)out_u, (u32)out_v,
		(u32)stats_dma);

	/* ============================================================
	 * Stock per-frame = 3 submits:
	 *   Submit 1: cal_data (~1544w) + imm syncpt  (cal update)
	 *   Submit 2: per-frame (45w) + imm syncpt    (frame trigger)
	 *   Submit 3: WAIT_SYNCPT (8w) + imm syncpt   (post-frame)
	 * ============================================================ */

	/* ---- Submit 1: Cal update (stock ~1544 words) ---- */
	cal_off = n;
	memcpy(&cmd[n], isp->cal_data, isp->cal_words * 4);
	n += isp->cal_words;
	/* Patch 0x053=1, 0x054=work_buf (last 2 data words of cal) */
	cmd[n - 2] = 0x00000001;
	cmd[n - 1] = (u32)isp->work_buf.dma;
	/* NO trigger — stock per-frame cal has no trigger 0x0F */
	cal_words = n - cal_off;

	/* Immediate syncpt for cal submit */
	cal_sp_off = n;
	cmd[n++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_immediate_v(),
		isp->syncpt_stream);
	cmd[n++] = NVHOST_OPCODE_NOOP;

	/* Submit cal job: 2 gathers, 1 syncpt (stream) */
	{
		struct nvhost_job *cal_job;
		cal_job = nvhost_job_alloc(isp->channel, 2, 0, 0, 1);
		if (!cal_job) {
			nvhost_module_idle(isp->pdev);
			return -ENOMEM;
		}
		cal_job->sp[0].id = isp->syncpt_stream;
		cal_job->sp[0].incrs = 1;
		cal_job->num_syncpts = 1;

		nvhost_job_add_gather(cal_job, 0, cal_words, 0,
				      isp->class_id, 0);
		cal_job->gathers[0].mem_base = cmd_phys + cal_off * 4;

		nvhost_job_add_gather(cal_job, 0, 2, 0, isp->class_id, 0);
		cal_job->gathers[1].mem_base = cmd_phys + cal_sp_off * 4;

		err = nvhost_channel_submit(cal_job);
		if (err) {
			dev_err(&isp->pdev->dev,
				"ISP cal submit failed: %d\n", err);
			nvhost_job_put(cal_job);
			nvhost_module_idle(isp->pdev);
			return err;
		}
		nvhost_job_put(cal_job);
	}

	/* ---- Submit 2: Per-frame (stock 45 words) ---- */
	g1_off = n;

	/* SET_CLASS */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);

	/* Output width/height/format/color — stock: INCR(0xE00,1)..INCR(0xE03,1) */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_WIDTH, 1);
	cmd[n++] = ((W - 1) & 0x3FFF) << 16;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_HEIGHT, 1);
	cmd[n++] = ((H - 1) & 0x3FFF) << 16;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_FORMAT, 1);
	cmd[n++] = ISP_FORMAT_STOCK;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_COLOR, 1);
	cmd[n++] = 0x00000000;

	/* Output Y/U/V surfaces: INCR(0xE04,3), INCR(0xE07,3), INCR(0xE0A,3) */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_Y, 3);
	cmd[n++] = (u32)out_y;
	cmd[n++] = 0x00000000;
	cmd[n++] = y_stride;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_U, 3);
	cmd[n++] = (u32)out_u;
	cmd[n++] = 0x00000000;
	cmd[n++] = uv_stride;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_V, 3);
	cmd[n++] = (u32)out_v;
	cmd[n++] = 0x00000000;
	cmd[n++] = uv_stride;

	/* Processing INCR(0x500,6): stock = [0, 0, 0, 0, 0, (H<<16)|W]
	 * Stock uses INPUT (sensor) resolution here, not output!
	 * IMX179 (ISP-A) = 3280x2464, OV5693 (ISP-B) = 2592x1944 */
	{
	u32 in_w, in_h;
	if (isp->class_id == ISP_A_CLASS_ID) {
		in_w = 3280; in_h = 2464;
	} else {
		in_w = 2592; in_h = 1944;
	}
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_PROCESSING, 6);
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = (in_h << 16) | in_w;
	}

	/* NO ISP_ENABLE here — stock sets it once in S5 init, not per-frame */

	/* Stats buffer INCR(0x100,4): [IOVA, 0, 0, 0] */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_STATS_BUF, 4);
	cmd[n++] = (u32)stats_dma;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;

	/* Conditional syncpt incrs — stock: SET_CLASS between each pair */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_nonincr(0x000, 1);
	cmd[n++] = (ISP_SYNCPT_COND_OP_DONE << 8) | isp->syncpt_memory;
	cmd[n++] = nvhost_opcode_nonincr(0x000, 1);
	cmd[n++] = (ISP_SYNCPT_COND_STATS_DONE << 8) | isp->syncpt_stats;
	cmd[n++] = nvhost_opcode_nonincr(0x000, 1);
	cmd[n++] = (ISP_SYNCPT_COND_RD_DONE << 8) | isp->syncpt_loadv;

	/* Runtime trigger: SET_CLASS + NONINCR(0x00C,1) = 0x05 */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_nonincr(ISP_METHOD_CONTROL, 1);
	cmd[n++] = ISP_TRIGGER_RUNTIME;

	g1_words = n - g1_off;

	/* Dump per-frame gather for comparison with stock */
	{
		int i;
		dev_info(&isp->pdev->dev,
			 "PER-FRAME G[0]: %d words\n", g1_words);
		for (i = g1_off; i < g1_off + g1_words; i += 8) {
			int rem = g1_words - (i - g1_off);
			if (rem >= 8)
				dev_info(&isp->pdev->dev,
					 "CMD[%d]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
					 i - g1_off,
					 cmd[i], cmd[i+1], cmd[i+2], cmd[i+3],
					 cmd[i+4], cmd[i+5], cmd[i+6], cmd[i+7]);
			else {
				char buf[128];
				int pos = 0, j;
				for (j = i; j < g1_off + g1_words; j++)
					pos += snprintf(buf + pos,
						sizeof(buf) - pos,
						"%08x ", cmd[j]);
				dev_info(&isp->pdev->dev,
					 "CMD[%d]: %s\n", i - g1_off, buf);
			}
		}
	}

	/* ---- G[1]: immediate syncpt incr for stream ---- */
	g2_off = n;
	cmd[n++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_immediate_v(),
		isp->syncpt_stream);
	cmd[n++] = NVHOST_OPCODE_NOOP;
	g2_words = n - g2_off;

	/* ---- G[2]: post-frame WAIT_SYNCPT (stock does this) ----
	 * Filled AFTER main submit to get correct fence values */
	g3_off = n;
	/* Reserve 10 words: 8 for WAIT_SYNCPT + 2 for syncpt incr */
	n += 10;

	/* Job: 2 gathers (G[0] + G[1]), 4 syncpts — G[2] submitted separately */
	job = nvhost_job_alloc(isp->channel, 2, 0, 0, 4);
	if (!job) {
		nvhost_module_idle(isp->pdev);
		return -ENOMEM;
	}

	/* SP[0] = memory, SP[1] = stats, SP[2] = loadv, SP[3] = stream */
	job->sp[0].id = isp->syncpt_memory;
	job->sp[0].incrs = 1;
	job->sp[1].id = isp->syncpt_stats;
	job->sp[1].incrs = 1;
	job->sp[2].id = isp->syncpt_loadv;
	job->sp[2].incrs = 1;
	job->sp[3].id = isp->syncpt_stream;
	job->sp[3].incrs = 1;
	job->num_syncpts = 4;

	nvhost_job_add_gather(job, 0, g1_words, 0, isp->class_id, 0);
	job->gathers[0].mem_base = cmd_phys + g1_off * 4;

	nvhost_job_add_gather(job, 0, g2_words, 0, isp->class_id, 0);
	job->gathers[1].mem_base = cmd_phys + g2_off * 4;

	err = nvhost_channel_submit(job);
	if (err) {
		dev_err(&isp->pdev->dev, "ISP frame submit failed: %d\n", err);
		nvhost_job_put(job);
		nvhost_module_idle(isp->pdev);
		return err;
	}

	/* Save fence thresholds for wait_frame and post-frame WAIT_SYNCPT */
	isp->frame_fence_memory = job->sp[0].fence;
	isp->frame_fence_stats = job->sp[1].fence;

	nvhost_job_put(job);

	/* Now fill G[2] post-frame with correct fence values
	 * WAIT_SYNCPT format: NONINCR(0x008,1) then (id << 24) | (thresh & 0xFFFFFF) */
	{
		int pn = g3_off;
		cmd[pn++] = nvhost_opcode_setclass(NV_HOST1X_CLASS_ID, 0, 0);
		cmd[pn++] = nvhost_opcode_nonincr(0x008, 1);
		cmd[pn++] = (isp->syncpt_memory << 24) |
			    (isp->frame_fence_memory & 0xFFFFFF);
		cmd[pn++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
		cmd[pn++] = nvhost_opcode_setclass(NV_HOST1X_CLASS_ID, 0, 0);
		cmd[pn++] = nvhost_opcode_nonincr(0x008, 1);
		cmd[pn++] = (isp->syncpt_stats << 24) |
			    (isp->frame_fence_stats & 0xFFFFFF);
		cmd[pn++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	}

	/* G[3]: immediate syncpt incr for post-frame stream */
	{
		int g4_off = g3_off + 8;
		struct nvhost_job *pf_job;

		cmd[g4_off] = nvhost_opcode_imm_incr_syncpt(
			host1x_uclass_incr_syncpt_cond_immediate_v(),
			isp->syncpt_stream);
		cmd[g4_off + 1] = NVHOST_OPCODE_NOOP;

		/* Submit post-frame: 2 gathers (WAIT_SYNCPT + syncpt incr) */
		pf_job = nvhost_job_alloc(isp->channel, 2, 0, 0, 1);
		if (pf_job) {
			pf_job->sp[0].id = isp->syncpt_stream;
			pf_job->sp[0].incrs = 1;
			pf_job->num_syncpts = 1;

			nvhost_job_add_gather(pf_job, 0, 8, 0,
					      isp->class_id, 0);
			pf_job->gathers[0].mem_base = cmd_phys + g3_off * 4;

			nvhost_job_add_gather(pf_job, 0, 2, 0,
					      isp->class_id, 0);
			pf_job->gathers[1].mem_base = cmd_phys + g4_off * 4;

			err = nvhost_channel_submit(pf_job);
			if (err)
				dev_err(&isp->pdev->dev,
					"ISP post-frame submit failed: %d\n",
					err);
			nvhost_job_put(pf_job);
		}
	}

	return 0;
}
EXPORT_SYMBOL(isp_t124_process_frame);

/*
 * isp_t124_process_frame_reprocess() - ISP reprocess mode (read from memory)
 *
 * Bypasses VI→ISP hardware pixel path. VI writes RAW to memory,
 * ISP reads from memory via input surfaces (0xE34).
 * ISP_ENABLE = 0x07 (full pipeline, memory input).
 *
 * 3 submits: cal update + reprocess frame + post-frame WAIT_SYNCPT.
 */
int isp_t124_process_frame_reprocess(struct tegra_isp_t124 *isp,
				     dma_addr_t raw_dma,
				     dma_addr_t out_dma,
				     dma_addr_t stats_dma)
{
	struct nvhost_job *job;
	u32 *cmd;
	dma_addr_t cmd_phys;
	int err;
	int n;
	int cal_off, cal_words, cal_sp_off;
	int g1_off, g1_words, g2_off, g2_words, g3_off;
	u32 W = isp->width, H = isp->height;
	u32 y_stride = isp->y_stride;
	u32 uv_stride = isp->uv_stride;
	u32 raw_stride = isp->in_stride;
	size_t y_size = (size_t)y_stride * H;
	size_t uv_size = (size_t)uv_stride * (H / 2);
	dma_addr_t out_y = out_dma;
	dma_addr_t out_u = out_dma + y_size;
	dma_addr_t out_v = out_dma + y_size + uv_size;
	u32 in_w, in_h;

	if (!isp->streaming || !isp->cmdbuf)
		return -ENODEV;

	/* ISP kept powered from stream_init — no module_busy needed */

	/* Sensor resolution for processing dim */
	if (isp->class_id == ISP_A_CLASS_ID) {
		in_w = 3280; in_h = 2464;
	} else {
		in_w = 2592; in_h = 1944;
	}

	cmd = isp->cmdbuf + ISP_CMDBUF_WORDS;
	cmd_phys = isp->cmdbuf_phys + ISP_CMDBUF_SIZE;
	n = 0;

	/* ---- Submit 1: Cal update ---- */
	cal_off = n;
	memcpy(&cmd[n], isp->cal_data, isp->cal_words * 4);
	n += isp->cal_words;
	cmd[n - 2] = 0x00000001;
	cmd[n - 1] = (u32)isp->work_buf.dma;
	cal_words = n - cal_off;

	cal_sp_off = n;
	cmd[n++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_immediate_v(),
		isp->syncpt_stream);
	cmd[n++] = NVHOST_OPCODE_NOOP;

	{
		struct nvhost_job *cal_job;
		cal_job = nvhost_job_alloc(isp->channel, 2, 0, 0, 1);
		if (!cal_job) {
			return -ENOMEM;
		}
		cal_job->sp[0].id = isp->syncpt_stream;
		cal_job->sp[0].incrs = 1;
		cal_job->num_syncpts = 1;

		nvhost_job_add_gather(cal_job, 0, cal_words, 0,
				      isp->class_id, 0);
		cal_job->gathers[0].mem_base = cmd_phys + cal_off * 4;

		nvhost_job_add_gather(cal_job, 0, 2, 0, isp->class_id, 0);
		cal_job->gathers[1].mem_base = cmd_phys + cal_sp_off * 4;

		err = nvhost_channel_submit(cal_job);
		if (err) {
			dev_err(&isp->pdev->dev,
				"ISP reprocess cal submit failed: %d\n", err);
			nvhost_job_put(cal_job);
			return err;
		}
		nvhost_job_put(cal_job);
	}

	/* ---- Submit 2: Reprocess per-frame ---- */
	g1_off = n;

	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);

	/* Output */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_WIDTH, 1);
	cmd[n++] = ((W - 1) & 0x3FFF) << 16;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_HEIGHT, 1);
	cmd[n++] = ((H - 1) & 0x3FFF) << 16;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_FORMAT, 1);
	cmd[n++] = ISP_FORMAT_STOCK;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_COLOR, 1);
	cmd[n++] = 0x00000000;

	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_Y, 3);
	cmd[n++] = (u32)out_y;
	cmd[n++] = 0x00000000;
	cmd[n++] = y_stride;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_U, 3);
	cmd[n++] = (u32)out_u;
	cmd[n++] = 0x00000000;
	cmd[n++] = uv_stride;
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_OUT_SURF_V, 3);
	cmd[n++] = (u32)out_v;
	cmd[n++] = 0x00000000;
	cmd[n++] = uv_stride;

	/* Processing — flags=3 (enable processing pipeline) */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_PROCESSING, 6);
	cmd[n++] = 0x00000003;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = (in_h << 16) | in_w;

	/* Output (same as streaming — already written above) */

	/* Stats buffer */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_STATS_BUF, 4);
	cmd[n++] = (u32)stats_dma;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;

	/* Syncpt incrs (cond=4,5,6) — before input to arm conditions */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_nonincr(0x000, 1);
	cmd[n++] = (ISP_SYNCPT_COND_OP_DONE << 8) | isp->syncpt_memory;
	cmd[n++] = nvhost_opcode_nonincr(0x000, 1);
	cmd[n++] = (ISP_SYNCPT_COND_STATS_DONE << 8) | isp->syncpt_stats;
	cmd[n++] = nvhost_opcode_nonincr(0x000, 1);
	cmd[n++] = (ISP_SYNCPT_COND_RD_DONE << 8) | isp->syncpt_loadv;

	/* Input — stock order: dims → format → surface → strip → enable → trigger */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_IN_DIMS, 1);
	cmd[n++] = (H << 16) | W;

	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_IN_FORMAT, 1);
	cmd[n++] = 0x11000020;

	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_IN_SURF0, 3);
	cmd[n++] = (u32)raw_dma;
	cmd[n++] = 0x00000000;
	cmd[n++] = raw_stride;

	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_IN_STRIP, 1);
	cmd[n++] = W & 0x3FFF;

	/* ISP_ENABLE = 0x07 (reprocess mode — stock RE confirmed) */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_ENABLE, 1);
	cmd[n++] = ISP_ENABLE_FULL_PIPELINE;

	/* Input trigger = 1 — FIRES ISP to read from memory */
	cmd[n++] = nvhost_opcode_incr(ISP_METHOD_IN_TRIGGER, 1);
	cmd[n++] = 0x00000001;

	/* Runtime trigger — NvRmStreamEnd adds this automatically in stock */
	cmd[n++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	cmd[n++] = nvhost_opcode_nonincr(ISP_METHOD_CONTROL, 1);
	cmd[n++] = ISP_TRIGGER_RUNTIME;

	g1_words = n - g1_off;

	/* Dump reprocess gather on first frame */
	if (isp->frame_fence_memory == 0) {
		int i;
		dev_info(&isp->pdev->dev,
			 "REPROCESS G[0]: %d words\n", g1_words);
		for (i = g1_off; i < g1_off + g1_words; i += 8) {
			int rem = g1_words - (i - g1_off);
			if (rem >= 8)
				dev_info(&isp->pdev->dev,
					 "RCMD[%d]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
					 i - g1_off,
					 cmd[i], cmd[i+1], cmd[i+2], cmd[i+3],
					 cmd[i+4], cmd[i+5], cmd[i+6], cmd[i+7]);
			else {
				char buf[128];
				int pos = 0, j;
				for (j = i; j < g1_off + g1_words; j++)
					pos += snprintf(buf + pos,
						sizeof(buf) - pos,
						"%08x ", cmd[j]);
				dev_info(&isp->pdev->dev,
					 "RCMD[%d]: %s\n", i - g1_off, buf);
			}
		}
	}

	/* G[1]: immediate syncpt */
	g2_off = n;
	cmd[n++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_immediate_v(),
		isp->syncpt_stream);
	cmd[n++] = NVHOST_OPCODE_NOOP;
	g2_words = n - g2_off;

	/* G[2]+G[3]: post-frame (filled after submit) */
	g3_off = n;
	n += 10;

	/* Submit per-frame job */
	job = nvhost_job_alloc(isp->channel, 2, 0, 0, 4);
	if (!job) {
		return -ENOMEM;
	}

	job->sp[0].id = isp->syncpt_memory;
	job->sp[0].incrs = 1;
	job->sp[1].id = isp->syncpt_stats;
	job->sp[1].incrs = 1;
	job->sp[2].id = isp->syncpt_loadv;
	job->sp[2].incrs = 1;
	job->sp[3].id = isp->syncpt_stream;
	job->sp[3].incrs = 1;
	job->num_syncpts = 4;

	nvhost_job_add_gather(job, 0, g1_words, 0, isp->class_id, 0);
	job->gathers[0].mem_base = cmd_phys + g1_off * 4;

	nvhost_job_add_gather(job, 0, g2_words, 0, isp->class_id, 0);
	job->gathers[1].mem_base = cmd_phys + g2_off * 4;

	err = nvhost_channel_submit(job);
	if (err) {
		dev_err(&isp->pdev->dev,
			"ISP reprocess frame submit failed: %d\n", err);
		nvhost_job_put(job);
		return err;
	}

	isp->frame_fence_memory = job->sp[0].fence;
	isp->frame_fence_stats = job->sp[1].fence;

	nvhost_job_put(job);

	/* Diagnostic: check if per-frame methods reach ISP hardware.
	 * S5 sets 0x015=0x04040007, reprocess per-frame sets 0x015=0x07.
	 * If readback shows 0x07 — methods work. If 0x04040007 — they don't. */
	if (isp->frame_fence_memory <= 1) {
		void __iomem *base;
		msleep(50); /* give CDMA time to execute */
		base = ioremap(
			(isp->class_id == ISP_A_CLASS_ID) ?
			0x54600000 : 0x54680000, 0x100);
		if (base) {
			dev_info(&isp->pdev->dev,
				 "REPROCESS DIAG: ENABLE(015)=0x%08x (expect 0x07)\n",
				 readl(base + 0x015 * 4));
			iounmap(base);
		}
	}

	/* Post-frame WAIT_SYNCPT */
	{
		int pn = g3_off;
		cmd[pn++] = nvhost_opcode_setclass(NV_HOST1X_CLASS_ID, 0, 0);
		cmd[pn++] = nvhost_opcode_nonincr(0x008, 1);
		cmd[pn++] = (isp->syncpt_memory << 24) |
			    (isp->frame_fence_memory & 0xFFFFFF);
		cmd[pn++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
		cmd[pn++] = nvhost_opcode_setclass(NV_HOST1X_CLASS_ID, 0, 0);
		cmd[pn++] = nvhost_opcode_nonincr(0x008, 1);
		cmd[pn++] = (isp->syncpt_stats << 24) |
			    (isp->frame_fence_stats & 0xFFFFFF);
		cmd[pn++] = nvhost_opcode_setclass(isp->class_id, 0, 0);
	}

	{
		int g4_off = g3_off + 8;
		struct nvhost_job *pf_job;

		cmd[g4_off] = nvhost_opcode_imm_incr_syncpt(
			host1x_uclass_incr_syncpt_cond_immediate_v(),
			isp->syncpt_stream);
		cmd[g4_off + 1] = NVHOST_OPCODE_NOOP;

		pf_job = nvhost_job_alloc(isp->channel, 2, 0, 0, 1);
		if (pf_job) {
			pf_job->sp[0].id = isp->syncpt_stream;
			pf_job->sp[0].incrs = 1;
			pf_job->num_syncpts = 1;

			nvhost_job_add_gather(pf_job, 0, 8, 0,
					      isp->class_id, 0);
			pf_job->gathers[0].mem_base = cmd_phys + g3_off * 4;

			nvhost_job_add_gather(pf_job, 0, 2, 0,
					      isp->class_id, 0);
			pf_job->gathers[1].mem_base = cmd_phys + g4_off * 4;

			err = nvhost_channel_submit(pf_job);
			if (err)
				dev_err(&isp->pdev->dev,
					"ISP reprocess post-frame failed: %d\n",
					err);
			nvhost_job_put(pf_job);
		}
	}

	return 0;
}
EXPORT_SYMBOL(isp_t124_process_frame_reprocess);

/*
 * isp_t124_wait_frame() - Wait for ISP frame completion
 *
 * Waits for cond=4 OP_DONE (syncpt_memory) — signals ISP output write done.
 * Call after VI capture done.
 */
int isp_t124_wait_frame(struct tegra_isp_t124 *isp)
{
	int err;

	if (!isp->streaming)
		return -ENODEV;

	/* Wait on syncpt_memory (OP_DONE), not syncpt_stream (submit) */
	err = nvhost_syncpt_wait_timeout_ext(isp->pdev,
			isp->syncpt_memory, isp->frame_fence_memory,
			msecs_to_jiffies(500), NULL, NULL);
	if (err)
		dev_err(&isp->pdev->dev,
			"ISP frame timeout: %d (sp=%u thresh=%u)\n",
			err, isp->syncpt_memory, isp->frame_fence_memory);
	else
		dev_info(&isp->pdev->dev,
			 "ISP frame OK (sp=%u thresh=%u)\n",
			 isp->syncpt_memory, isp->frame_fence_memory);

	if (!isp->reprocess)
		nvhost_module_idle(isp->pdev);
	return err;
}
EXPORT_SYMBOL(isp_t124_wait_frame);

/* ----------------------------------------------------------------
 * debugfs
 * ---------------------------------------------------------------- */

static int isp_t124_debugfs_ping_show(struct seq_file *s, void *data)
{
	struct tegra_isp_t124 *isp = s->private;
	ktime_t start = ktime_get();
	int ret = isp_t124_ping(isp);
	s64 us = ktime_us_delta(ktime_get(), start);

	seq_printf(s, "ISP%s %s, syncpts: mem=%u stats=%u stream=%u loadv=%u (%lld us)\n",
		   isp->class_id == ISP_A_CLASS_ID ? "-A" : "-B",
		   ret ? "FAILED" : "alive",
		   isp->syncpt_memory, isp->syncpt_stats,
		   isp->syncpt_stream, isp->syncpt_loadv,
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
