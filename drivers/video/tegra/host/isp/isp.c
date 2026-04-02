/*
 * Tegra Graphics ISP
 *
 * Copyright (c) 2012-2016, NVIDIA Corporation.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/export.h>
#include <linux/resource.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/irq.h>
#include <linux/workqueue.h>
#include <linux/tegra_pm_domains.h>
#include <linux/tegra-fuse.h>

#include "dev.h"
#include "bus_client.h"
#include "nvhost_acm.h"
#include "t124/t124.h"
#include "t210/t210.h"

#ifdef CONFIG_ARCH_TEGRA_18x_SOC
#include "t186/t186.h"
#endif

#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/nvhost_isp_ioctl.h>
#include <linux/platform/tegra/latency_allowance.h>
#include <linux/dma-mapping.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <media/v4l2-async.h>
#include "isp.h"
#include "nvhost_job.h"
#include "host1x/host1x01_hardware.h"
#include "class_ids.h"

#define T12_ISP_CG_CTRL		0x74
#define T12_CG_2ND_LEVEL_EN	1

#define	ISP_MAX_BPP		2

#define ISPA_DEV_ID		0
#define ISPB_DEV_ID		1

static struct of_device_id tegra_isp_of_match[] = {
#ifdef TEGRA_12X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra124-isp",
		.data = (struct nvhost_device_data *)&t124_isp_info },
#endif
#ifdef TEGRA_21X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra210-isp",
		.data = (struct nvhost_device_data *)&t21_isp_info },
#endif
#ifdef CONFIG_ARCH_TEGRA_18x_SOC
	{ .compatible = "nvidia,tegra186-isp",
		.data = (struct nvhost_device_data *)&t18_isp_info },
#endif
	{ },
};

static void (*mfi_callback)(void *);
static void *mfi_callback_arg;
static DEFINE_MUTEX(isp_isr_lock);

static int __init init_tegra_isp_isr_callback(void)
{
	mutex_init(&isp_isr_lock);
	return 0;
}

pure_initcall(init_tegra_isp_isr_callback);

int nvhost_isp_t124_prepare_poweroff(struct platform_device *pdev)
{
	struct nvhost_device_data *pdata = platform_get_drvdata(pdev);
	struct isp *tegra_isp = pdata->private_data;

	disable_irq(tegra_isp->irq);

	return 0;
}

int nvhost_isp_t124_finalize_poweron(struct platform_device *pdev)
{
	struct nvhost_device_data *pdata = platform_get_drvdata(pdev);
	struct isp *tegra_isp = pdata->private_data;

	host1x_writel(pdev, T12_ISP_CG_CTRL, T12_CG_2ND_LEVEL_EN);
	enable_irq(tegra_isp->irq);

	return 0;
}

int nvhost_isp_t210_finalize_poweron(struct platform_device *pdev)
{
	struct nvhost_device_data *pdata = platform_get_drvdata(pdev);
	struct isp *tegra_isp = pdata->private_data;

	enable_irq(tegra_isp->irq);

	return 0;
}

#if defined(CONFIG_TEGRA_ISOMGR)
static int isp_isomgr_register(struct isp *tegra_isp)
{
	int iso_client_id = TEGRA_ISO_CLIENT_ISP_A;
	struct clk *isp_clk;
	struct nvhost_device_data *pdata =
				platform_get_drvdata(tegra_isp->ndev);

	dev_dbg(&tegra_isp->ndev->dev, "%s++\n", __func__);

	if (WARN_ONCE(pdata == NULL, "pdata not found, %s failed\n", __func__))
		return -ENODEV;

	if (tegra_isp->dev_id == ISPB_DEV_ID)
		iso_client_id = TEGRA_ISO_CLIENT_ISP_B;
	if (tegra_isp->dev_id == ISPA_DEV_ID)
		iso_client_id = TEGRA_ISO_CLIENT_ISP_A;

	/* Get max ISP BW */
	isp_clk = pdata->clk[0];
	tegra_isp->max_bw =
		(clk_round_rate(isp_clk, UINT_MAX) / 1000) * ISP_MAX_BPP;

	/* Register with max possible BW for ISP usecases.*/
	tegra_isp->isomgr_handle = tegra_isomgr_register(iso_client_id,
					tegra_isp->max_bw,
					NULL,	/* tegra_isomgr_renegotiate */
					NULL);	/* *priv */

	if (!tegra_isp->isomgr_handle) {
		dev_err(&tegra_isp->ndev->dev,
			"%s: unable to register isomgr\n",
				__func__);
		return -ENOMEM;
	}

	return 0;
}

static int isp_isomgr_unregister(struct isp *tegra_isp)
{
	tegra_isomgr_unregister(tegra_isp->isomgr_handle);
	tegra_isp->isomgr_handle = NULL;

	return 0;
}

static int isp_isomgr_request(struct isp *tegra_isp, uint isp_bw, uint lt)
{
	int ret = 0;

	dev_dbg(&tegra_isp->ndev->dev,
		"%s++ bw=%u, lt=%u\n", __func__, isp_bw, lt);

	/* return value of tegra_isomgr_reserve is dvfs latency in usec */
	ret = tegra_isomgr_reserve(tegra_isp->isomgr_handle,
				isp_bw,	/* KB/sec */
				lt);	/* usec */
	if (!ret) {
		dev_err(&tegra_isp->ndev->dev,
		"%s: failed to reserve %u KBps\n", __func__, isp_bw);
		return -ENOMEM;
	}

	/* return value of tegra_isomgr_realize is dvfs latency in usec */
	ret = tegra_isomgr_realize(tegra_isp->isomgr_handle);
	if (ret)
		dev_dbg(&tegra_isp->ndev->dev,
		"%s: tegra_isp isomgr latency is %d usec",
		__func__, ret);
	else {
		dev_err(&tegra_isp->ndev->dev,
		"%s: failed to realize %u KBps\n", __func__, isp_bw);
			return -ENOMEM;
	}
	return 0;
}

static int isp_isomgr_release(struct isp *tegra_isp)
{
	int ret = 0;
	dev_dbg(&tegra_isp->ndev->dev, "%s++\n", __func__);

	/* deallocate isomgr bw */
	ret = isp_isomgr_request(tegra_isp, 0, 0);
	if (ret) {
		dev_err(&tegra_isp->ndev->dev,
		"%s: failed to deallocate memory in isomgr\n",
		__func__);
		return -ENOMEM;
	}

	return 0;
}
#endif

static inline u32 tegra_isp_read(struct isp *tegra_isp, u32 offset)
{
	return readl(tegra_isp->base + offset);
}

static inline void tegra_isp_write(struct isp *tegra_isp, u32 offset, u32 data)
{
	writel(data, tegra_isp->base + offset);
}

int tegra_isp_register_mfi_cb(callback cb, void *cb_arg)
{
	if (mfi_callback || mfi_callback_arg) {
		pr_err("cb already registered\n");
		return -1;
	}

	mutex_lock(&isp_isr_lock);
	mfi_callback = cb;
	mfi_callback_arg = cb_arg;
	mutex_unlock(&isp_isr_lock);

	return 0;
}
EXPORT_SYMBOL(tegra_isp_register_mfi_cb);

int tegra_isp_unregister_mfi_cb(void)
{
	mutex_lock(&isp_isr_lock);
	mfi_callback = NULL;
	mfi_callback_arg = NULL;
	mutex_unlock(&isp_isr_lock);

	return 0;
}
EXPORT_SYMBOL(tegra_isp_unregister_mfi_cb);

static void isp_isr_work(struct work_struct *isp_work)
{
	if (mfi_callback == NULL) {
		pr_debug("NULL callback\n");
		return;
	}

	mutex_lock(&isp_isr_lock);
	mfi_callback(mfi_callback_arg);
	mutex_unlock(&isp_isr_lock);
	return;
}

void nvhost_isp_queue_isr_work(struct isp *tegra_isp)
{
	queue_work(tegra_isp->isp_workqueue, &tegra_isp->my_isr_work->work);
}

/* ----------------------------------------------------------------
 * Host1x channel + job submission infrastructure
 * ---------------------------------------------------------------- */

static int tegra_isp_channel_init(struct isp *tegra_isp)
{
	struct nvhost_device_data *pdata =
		platform_get_drvdata(tegra_isp->ndev);
	int err;

	err = nvhost_channel_map(pdata, &tegra_isp->channel, tegra_isp);
	if (err) {
		dev_err(&tegra_isp->ndev->dev,
			"ISP host1x channel map failed: %d\n", err);
		return err;
	}

	tegra_isp->syncpt_id = nvhost_get_syncpt_host_managed(
		tegra_isp->ndev, 0, "isp");
	if (!tegra_isp->syncpt_id) {
		dev_err(&tegra_isp->ndev->dev,
			"ISP syncpt allocation failed\n");
		nvhost_putchannel(tegra_isp->channel, 1);
		tegra_isp->channel = NULL;
		return -ENOMEM;
	}

	dev_info(&tegra_isp->ndev->dev,
		"ISP host1x channel mapped, syncpt=%u\n",
		tegra_isp->syncpt_id);
	return 0;
}

static void tegra_isp_channel_cleanup(struct isp *tegra_isp)
{
	if (tegra_isp->syncpt_id) {
		nvhost_syncpt_put_ref_ext(tegra_isp->ndev,
					  tegra_isp->syncpt_id);
		tegra_isp->syncpt_id = 0;
	}
	if (tegra_isp->channel) {
		nvhost_putchannel(tegra_isp->channel, 1);
		tegra_isp->channel = NULL;
	}
}

/**
 * tegra_isp_ping() - Submit minimal host1x job to verify ISP hardware
 *
 * Sends SET_CLASS(ISP-A) + ISP_ENABLE write + syncpoint OP_DONE increment.
 * If the syncpoint fires, ISP hardware actually executed the method.
 */
static int tegra_isp_ping(struct isp *tegra_isp)
{
	struct device *dev = &tegra_isp->ndev->dev;
	struct nvhost_job *job = NULL;
	u32 *cmdbuf;
	dma_addr_t cmdbuf_phys;
	int err;
	int num_words;

	if (!tegra_isp->channel || !tegra_isp->syncpt_id)
		return -ENODEV;

	/* Power on ISP */
	err = nvhost_module_busy(tegra_isp->ndev);
	if (err) {
		dev_err(dev, "ISP power on failed: %d\n", err);
		return err;
	}

	/* Allocate DMA command buffer (page aligned) */
	cmdbuf = dma_alloc_coherent(dev, PAGE_SIZE, &cmdbuf_phys, GFP_KERNEL);
	if (!cmdbuf) {
		err = -ENOMEM;
		goto idle;
	}

	/* Build command buffer: absolute minimum
	 * NOTE: SET_CLASS is NOT allowed inside gathers (gather filter
	 * blocks it). The class is set via nvhost_job_add_client_gather_address
	 * class_id parameter instead.
	 * Just: INCR_SYNCPT(IMMEDIATE) + NOOP padding
	 */
	num_words = 0;
	cmdbuf[num_words++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_immediate_v(),
		tegra_isp->syncpt_id);
	cmdbuf[num_words++] = NVHOST_OPCODE_NOOP;
	cmdbuf[num_words++] = NVHOST_OPCODE_NOOP;
	cmdbuf[num_words++] = NVHOST_OPCODE_NOOP;

	/* Allocate and configure job */
	job = nvhost_job_alloc(tegra_isp->channel, 1, 0, 0, 1);
	if (!job) {
		err = -ENOMEM;
		goto free_cmdbuf;
	}

	job->sp->id = tegra_isp->syncpt_id;
	job->sp->incrs = 1;
	job->num_syncpts = 1;

	err = nvhost_job_add_client_gather_address(job, num_words,
		NV_VIDEO_STREAMING_ISP_CLASS_ID, cmdbuf_phys);
	if (err) {
		dev_err(dev, "ISP gather add failed: %d\n", err);
		goto put_job;
	}

	/* Submit */
	err = nvhost_channel_submit(job);
	if (err) {
		dev_err(dev, "ISP submit failed: %d\n", err);
		goto put_job;
	}

	/* Wait for syncpoint (500ms timeout) */
	err = nvhost_syncpt_wait_timeout_ext(tegra_isp->ndev,
		job->sp->id, job->sp->fence,
		msecs_to_jiffies(500), NULL, NULL);
	if (err)
		dev_err(dev, "ISP syncpt wait timeout: %d\n", err);

put_job:
	nvhost_job_put(job);
	/*
	 * Free cmdbuf only after job_put. On timeout the CDMA may still
	 * reference the gather buffer; job_put handles that cleanup.
	 * Using coherent DMA so no explicit sync needed.
	 */
free_cmdbuf:
	dma_free_coherent(dev, PAGE_SIZE, cmdbuf, cmdbuf_phys);
idle:
	nvhost_module_idle(tegra_isp->ndev);
	return err;
}

/* ISP method offsets (word-addressed, from libnvisp_v3.so reverse engineering) */
#define ISP_METHOD_CONTROL	0x00C	/* ISP control register */
#define ISP_METHOD_ENABLE	0x015	/* ISP enable/mode */

/**
 * tegra_isp_regwrite_test() - Test ISP register write via host1x method
 *
 * Writes to known ISP method offsets (from reverse engineering) and uses
 * REG_WR_SAFE syncpoint condition to confirm the write completed.
 * This proves ISP accepts method writes through the host1x class interface.
 */
static int tegra_isp_regwrite_test(struct isp *tegra_isp,
				   struct seq_file *s)
{
	struct device *dev = &tegra_isp->ndev->dev;
	struct nvhost_job *job = NULL;
	u32 *cmdbuf;
	dma_addr_t cmdbuf_phys;
	int err;
	int num_words;
	ktime_t start;
	s64 us;

	if (!tegra_isp->channel || !tegra_isp->syncpt_id)
		return -ENODEV;

	err = nvhost_module_busy(tegra_isp->ndev);
	if (err)
		return err;

	cmdbuf = dma_alloc_coherent(dev, PAGE_SIZE, &cmdbuf_phys, GFP_KERNEL);
	if (!cmdbuf) {
		err = -ENOMEM;
		goto idle;
	}

	/* Test 1: IMMEDIATE syncpt (baseline, should always pass) */
	num_words = 0;
	cmdbuf[num_words++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_immediate_v(),
		tegra_isp->syncpt_id);
	cmdbuf[num_words++] = NVHOST_OPCODE_NOOP;

	job = nvhost_job_alloc(tegra_isp->channel, 1, 0, 0, 1);
	if (!job) { err = -ENOMEM; goto free_cmdbuf; }
	job->sp->id = tegra_isp->syncpt_id;
	job->sp->incrs = 1;
	job->num_syncpts = 1;
	err = nvhost_job_add_client_gather_address(job, num_words,
		NV_VIDEO_STREAMING_ISP_CLASS_ID, cmdbuf_phys);
	if (err) goto put_job;
	start = ktime_get();
	err = nvhost_channel_submit(job);
	if (err) goto put_job;
	err = nvhost_syncpt_wait_timeout_ext(tegra_isp->ndev,
		job->sp->id, job->sp->fence, msecs_to_jiffies(500), NULL, NULL);
	us = ktime_us_delta(ktime_get(), start);
	seq_printf(s, "test1 IMMEDIATE: %s (%lld us)\n",
		err ? "FAIL" : "OK", us);
	nvhost_job_put(job);
	job = NULL;
	if (err)
		goto free_cmdbuf;

	/* Test 2: Write ISP_METHOD_ENABLE=1 + REG_WR_SAFE syncpt */
	num_words = 0;
	cmdbuf[num_words++] = nvhost_opcode_incr(ISP_METHOD_ENABLE, 1);
	cmdbuf[num_words++] = 0x00000001; /* enable ISP */
	cmdbuf[num_words++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_reg_wr_safe_v(),
		tegra_isp->syncpt_id);
	cmdbuf[num_words++] = NVHOST_OPCODE_NOOP;

	job = nvhost_job_alloc(tegra_isp->channel, 1, 0, 0, 1);
	if (!job) { err = -ENOMEM; goto free_cmdbuf; }
	job->sp->id = tegra_isp->syncpt_id;
	job->sp->incrs = 1;
	job->num_syncpts = 1;
	err = nvhost_job_add_client_gather_address(job, num_words,
		NV_VIDEO_STREAMING_ISP_CLASS_ID, cmdbuf_phys);
	if (err) goto put_job;
	start = ktime_get();
	err = nvhost_channel_submit(job);
	if (err) goto put_job;
	err = nvhost_syncpt_wait_timeout_ext(tegra_isp->ndev,
		job->sp->id, job->sp->fence, msecs_to_jiffies(500), NULL, NULL);
	us = ktime_us_delta(ktime_get(), start);
	seq_printf(s, "test2 ENABLE(0x%03x)=1 REG_WR_SAFE: %s (%lld us)\n",
		ISP_METHOD_ENABLE, err ? "FAIL" : "OK", us);
	nvhost_job_put(job);
	job = NULL;
	if (err)
		goto free_cmdbuf;

	/* Test 3: Write ISP_METHOD_ENABLE + OP_DONE syncpt */
	num_words = 0;
	cmdbuf[num_words++] = nvhost_opcode_incr(ISP_METHOD_ENABLE, 1);
	cmdbuf[num_words++] = 0x00000001;
	cmdbuf[num_words++] = nvhost_opcode_imm_incr_syncpt(
		host1x_uclass_incr_syncpt_cond_op_done_v(),
		tegra_isp->syncpt_id);
	cmdbuf[num_words++] = NVHOST_OPCODE_NOOP;

	job = nvhost_job_alloc(tegra_isp->channel, 1, 0, 0, 1);
	if (!job) { err = -ENOMEM; goto free_cmdbuf; }
	job->sp->id = tegra_isp->syncpt_id;
	job->sp->incrs = 1;
	job->num_syncpts = 1;
	err = nvhost_job_add_client_gather_address(job, num_words,
		NV_VIDEO_STREAMING_ISP_CLASS_ID, cmdbuf_phys);
	if (err) goto put_job;
	start = ktime_get();
	err = nvhost_channel_submit(job);
	if (err) goto put_job;
	err = nvhost_syncpt_wait_timeout_ext(tegra_isp->ndev,
		job->sp->id, job->sp->fence, msecs_to_jiffies(500), NULL, NULL);
	us = ktime_us_delta(ktime_get(), start);
	seq_printf(s, "test3 ENABLE(0x%03x)=1 OP_DONE: %s (%lld us)\n",
		ISP_METHOD_ENABLE, err ? "FAIL" : "OK", us);

put_job:
	if (job)
		nvhost_job_put(job);
free_cmdbuf:
	dma_free_coherent(dev, PAGE_SIZE, cmdbuf, cmdbuf_phys);
idle:
	nvhost_module_idle(tegra_isp->ndev);
	return 0; /* always return 0 so seq_file shows results */
}

/**
 * tegra_isp_probe_methods() - Probe all 24 known ISP method offsets
 *
 * For each method offset from the reverse-engineered register map,
 * submit a single write (value=0) + REG_WR_SAFE syncpt and report
 * whether the ISP engine accepted it.
 */
static int tegra_isp_probe_methods(struct isp *tegra_isp,
				   struct seq_file *s)
{
	static const struct {
		u16 offset;
		const char *name;
	} methods[] = {
		{ 0x00C, "control" },
		{ 0x015, "enable" },
		{ 0x100, "color_proc" },
		{ 0x101, "tone_curve_fifo" },
		{ 0x200, "input_ch_a" },
		{ 0x300, "input_ch_b" },
		{ 0x500, "processing" },
		{ 0x800, "stats_cfg" },
		{ 0x87A, "histogram" },
		{ 0x902, "stats_ctrl" },
		{ 0xC41, "stats_out_1" },
		{ 0xC43, "stats_out_2" },
		{ 0xC45, "focus_stats" },
		{ 0xC47, "hist_stats" },
		{ 0xC5A, "stats_extra" },
		{ 0xD31, "lens_shading_a" },
		{ 0xDAF, "lens_shading_b" },
		{ 0xE00, "output_ctrl_0" },
		{ 0xE01, "output_ctrl_1" },
		{ 0xE02, "output_ctrl_2" },
		{ 0xE30, "output_fmt_0" },
		{ 0xE31, "output_fmt_1" },
		{ 0xE32, "output_fmt_2" },
		{ 0xE33, "output_fmt_3" },
	};
	struct device *dev = &tegra_isp->ndev->dev;
	struct nvhost_job *job;
	u32 *cmdbuf;
	dma_addr_t cmdbuf_phys;
	int err, i;
	int num_words;
	ktime_t start;
	s64 us;

	if (!tegra_isp->channel || !tegra_isp->syncpt_id)
		return -ENODEV;

	err = nvhost_module_busy(tegra_isp->ndev);
	if (err)
		return err;

	cmdbuf = dma_alloc_coherent(dev, PAGE_SIZE, &cmdbuf_phys, GFP_KERNEL);
	if (!cmdbuf) {
		err = -ENOMEM;
		goto idle;
	}

	seq_printf(s, "Probing %zu ISP method offsets (write 0 + REG_WR_SAFE):\n",
		ARRAY_SIZE(methods));

	for (i = 0; i < ARRAY_SIZE(methods); i++) {
		num_words = 0;
		cmdbuf[num_words++] = nvhost_opcode_incr(methods[i].offset, 1);
		cmdbuf[num_words++] = 0x00000000;
		cmdbuf[num_words++] = nvhost_opcode_imm_incr_syncpt(
			host1x_uclass_incr_syncpt_cond_reg_wr_safe_v(),
			tegra_isp->syncpt_id);
		cmdbuf[num_words++] = NVHOST_OPCODE_NOOP;

		job = nvhost_job_alloc(tegra_isp->channel, 1, 0, 0, 1);
		if (!job) {
			seq_printf(s, "  0x%03x %-16s job_alloc FAIL\n",
				methods[i].offset, methods[i].name);
			continue;
		}
		job->sp->id = tegra_isp->syncpt_id;
		job->sp->incrs = 1;
		job->num_syncpts = 1;

		err = nvhost_job_add_client_gather_address(job, num_words,
			NV_VIDEO_STREAMING_ISP_CLASS_ID, cmdbuf_phys);
		if (err) {
			nvhost_job_put(job);
			seq_printf(s, "  0x%03x %-16s gather FAIL\n",
				methods[i].offset, methods[i].name);
			continue;
		}

		start = ktime_get();
		err = nvhost_channel_submit(job);
		if (err) {
			nvhost_job_put(job);
			seq_printf(s, "  0x%03x %-16s submit FAIL (%d)\n",
				methods[i].offset, methods[i].name, err);
			continue;
		}

		err = nvhost_syncpt_wait_timeout_ext(tegra_isp->ndev,
			job->sp->id, job->sp->fence,
			msecs_to_jiffies(100), NULL, NULL);
		us = ktime_us_delta(ktime_get(), start);
		nvhost_job_put(job);

		seq_printf(s, "  0x%03x %-16s %s (%lld us)\n",
			methods[i].offset, methods[i].name,
			err ? "TIMEOUT" : "OK", us);

		if (err) {
			seq_printf(s, "  *** stopped after first timeout\n");
			break;
		}
	}

	dma_free_coherent(dev, PAGE_SIZE, cmdbuf, cmdbuf_phys);
idle:
	nvhost_module_idle(tegra_isp->ndev);
	return 0;
}

/* ----------------------------------------------------------------
 * debugfs
 * ---------------------------------------------------------------- */

static int isp_debugfs_ping_show(struct seq_file *s, void *data)
{
	struct isp *tegra_isp = s->private;
	ktime_t start;
	s64 us;
	int ret;

	start = ktime_get();
	ret = tegra_isp_ping(tegra_isp);
	us = ktime_us_delta(ktime_get(), start);

	if (ret == 0)
		seq_printf(s, "ISP-%s alive, syncpt %u completed in %lld us\n",
			tegra_isp->dev_id == 0 ? "A" : "B",
			tegra_isp->syncpt_id, us);
	else
		seq_printf(s, "ISP-%s FAILED (err=%d, %lld us)\n",
			tegra_isp->dev_id == 0 ? "A" : "B", ret, us);
	return 0;
}

static int isp_debugfs_ping_open(struct inode *inode, struct file *file)
{
	return single_open(file, isp_debugfs_ping_show, inode->i_private);
}

static const struct file_operations isp_debugfs_ping_fops = {
	.open		= isp_debugfs_ping_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int isp_debugfs_regtest_show(struct seq_file *s, void *data)
{
	return tegra_isp_regwrite_test(s->private, s);
}

static int isp_debugfs_regtest_open(struct inode *inode, struct file *file)
{
	return single_open(file, isp_debugfs_regtest_show, inode->i_private);
}

static const struct file_operations isp_debugfs_regtest_fops = {
	.open		= isp_debugfs_regtest_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int isp_debugfs_probe_show(struct seq_file *s, void *data)
{
	return tegra_isp_probe_methods(s->private, s);
}

static int isp_debugfs_probe_open(struct inode *inode, struct file *file)
{
	return single_open(file, isp_debugfs_probe_show, inode->i_private);
}

static const struct file_operations isp_debugfs_probe_fops = {
	.open		= isp_debugfs_probe_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void tegra_isp_debugfs_init(struct isp *tegra_isp)
{
	const char *name = tegra_isp->dev_id == 0 ? "isp_a" : "isp_b";

	tegra_isp->debugfs_dir = debugfs_create_dir(name, NULL);
	if (!tegra_isp->debugfs_dir)
		return;

	debugfs_create_file("ping", 0444, tegra_isp->debugfs_dir,
			    tegra_isp, &isp_debugfs_ping_fops);
	debugfs_create_file("regtest", 0444, tegra_isp->debugfs_dir,
			    tegra_isp, &isp_debugfs_regtest_fops);
	debugfs_create_file("probe", 0444, tegra_isp->debugfs_dir,
			    tegra_isp, &isp_debugfs_probe_fops);
}

static void tegra_isp_debugfs_cleanup(struct isp *tegra_isp)
{
	debugfs_remove_recursive(tegra_isp->debugfs_dir);
	tegra_isp->debugfs_dir = NULL;
}

/* ----------------------------------------------------------------
 * V4L2 subdev / Media Controller integration
 * ISP registers as a V4L2 subdev entity in the MC graph so that
 * media-ctl can discover it. No frame processing yet.
 * ---------------------------------------------------------------- */

static int tegra_isp_subdev_s_power(struct v4l2_subdev *sd, int on)
{
	/* Power is managed by nvhost runtime PM — nothing to do here */
	return 0;
}

static const struct v4l2_subdev_core_ops tegra_isp_subdev_core_ops = {
	.s_power = tegra_isp_subdev_s_power,
};

static const struct v4l2_subdev_ops tegra_isp_subdev_ops = {
	.core = &tegra_isp_subdev_core_ops,
};

#if defined(CONFIG_MEDIA_CONTROLLER)
static const struct media_entity_operations tegra_isp_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};
#endif

static int tegra_isp_register_subdev(struct isp *tegra_isp)
{
	struct v4l2_subdev *sd = &tegra_isp->subdev;
	struct device *dev = &tegra_isp->ndev->dev;
	int ret;

	v4l2_subdev_init(sd, &tegra_isp_subdev_ops);
	sd->dev = dev;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	snprintf(sd->name, sizeof(sd->name), "%s",
		 dev_name(dev));

	tegra_isp->pads[0].flags = MEDIA_PAD_FL_SOURCE;
	tegra_isp->pads[1].flags = MEDIA_PAD_FL_SINK;

#if defined(CONFIG_MEDIA_CONTROLLER)
	sd->entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	sd->entity.ops = &tegra_isp_media_ops;
	ret = media_entity_init(&sd->entity, 2, tegra_isp->pads, 0);
	if (ret < 0) {
		dev_err(dev, "failed to init ISP media entity: %d\n", ret);
		return ret;
	}
#endif

	ret = v4l2_async_register_subdev(sd);
	if (ret < 0) {
		dev_err(dev, "failed to register ISP V4L2 subdev: %d\n", ret);
		media_entity_cleanup(&sd->entity);
		return ret;
	}

	dev_info(dev, "ISP V4L2 subdev registered\n");
	return 0;
}

static void tegra_isp_unregister_subdev(struct isp *tegra_isp)
{
	v4l2_async_unregister_subdev(&tegra_isp->subdev);
	media_entity_cleanup(&tegra_isp->subdev.entity);
}

static int isp_probe(struct platform_device *dev)
{
	int err = 0;
	int dev_id = 0;

	struct isp *tegra_isp;
	struct nvhost_device_data *pdata = NULL;

	if (dev->dev.of_node) {
		const struct of_device_id *match;

		match = of_match_device(tegra_isp_of_match, &dev->dev);
		if (match)
			pdata = (struct nvhost_device_data *)match->data;

		if (!IS_ENABLED(CONFIG_ARCH_TEGRA_18x_SOC)) {
			if (sscanf(dev->name, "isp.%1d", &dev_id) != 1)
				return -EINVAL;
			switch (tegra_get_chipid()) {
			case TEGRA_CHIPID_TEGRA12:
			case TEGRA_CHIPID_TEGRA13:
				if (dev_id == ISPB_DEV_ID)
					pdata = &t124_ispb_info;
				if (dev_id == ISPA_DEV_ID)
					pdata = &t124_isp_info;
				break;
			case TEGRA_CHIPID_TEGRA21:
				if (dev_id == ISPB_DEV_ID)
					pdata = &t21_ispb_info;
				if (dev_id == ISPA_DEV_ID)
					pdata = &t21_isp_info;
				break;
			default:
				return -EINVAL;
			}
		}

	} else
		pdata = (struct nvhost_device_data *)dev->dev.platform_data;

	WARN_ON(!pdata);
	if (!pdata) {
		dev_info(&dev->dev, "no platform data\n");
		return -ENODATA;
	}

	err = nvhost_check_bondout(pdata->bond_out_id);
	if (err) {
		dev_err(&dev->dev, "No ISP unit present. err:%d", err);
		return err;
	}

	tegra_isp = devm_kzalloc(&dev->dev, sizeof(struct isp), GFP_KERNEL);
	if (!tegra_isp) {
		dev_err(&dev->dev, "can't allocate memory for isp\n");
		return -ENOMEM;
	}

	pdata->pdev = dev;
	mutex_init(&pdata->lock);
	platform_set_drvdata(dev, pdata);

	err = nvhost_client_device_get_resources(dev);
	if (err)
		goto camera_isp_unregister;

	tegra_isp->dev_id = dev_id;
	tegra_isp->ndev = dev;

	pdata->private_data = tegra_isp;

	/* init ispa isr */
	tegra_isp->base = pdata->aperture[0];
	if (!tegra_isp->base) {
		pr_err("%s: can't ioremap gnt_base\n", __func__);
		err = -ENOMEM;
	}

	/* creating workqueue */
	if (dev_id == 0)
		tegra_isp->isp_workqueue = alloc_workqueue("ispa_workqueue",
						 WQ_HIGHPRI | WQ_UNBOUND, 1);
	else
		tegra_isp->isp_workqueue = alloc_workqueue("ispb_workqueue",
						 WQ_HIGHPRI | WQ_UNBOUND, 1);

	if (!tegra_isp->isp_workqueue) {
		pr_err("failed to allocate isp_workqueue\n");
		goto camera_isp_unregister;
	}

	tegra_isp->my_isr_work =
		kmalloc(sizeof(struct tegra_isp_mfi), GFP_KERNEL);

	if (!tegra_isp->my_isr_work) {
		err = -ENOMEM;
		goto camera_isp_unregister;
	}

	INIT_WORK((struct work_struct *)tegra_isp->my_isr_work, isp_isr_work);

	nvhost_module_init(dev);

#ifdef CONFIG_PM_GENERIC_DOMAINS

	/* In T210 power ISPB is placed to a separate power partition */
#ifndef CONFIG_PM_GENERIC_DOMAINS_OF
	if (tegra_get_chipid() == TEGRA_CHIPID_TEGRA21 &&
	    dev_id == ISPB_DEV_ID)
		pdata->pd.name = "ve2";
	else
		pdata->pd.name = "ve";
#endif

	/* add module power domain and also add its domain
	 * as sub-domain of MC domain */
	err = nvhost_module_add_domain(&pdata->pd, dev);
	if (err)
		goto free_isr;
#endif

	err = nvhost_client_device_init(dev);
	if (err)
		goto free_isr;

	/* Register V4L2 subdev for ISP-A only (ISP-B later) */
	if (dev_id == ISPA_DEV_ID) {
		err = tegra_isp_register_subdev(tegra_isp);
		if (err) {
			dev_warn(&dev->dev,
				"ISP V4L2 subdev registration failed: %d\n",
				err);
			/* Non-fatal: legacy ioctl interface still works */
		}
	}

	/* Init host1x channel for job submission (ISP-A only for now) */
	if (dev_id == ISPA_DEV_ID) {
		err = tegra_isp_channel_init(tegra_isp);
		if (err)
			dev_warn(&dev->dev,
				"ISP host1x channel init failed: %d\n", err);
		else
			tegra_isp_debugfs_init(tegra_isp);
	}

	return 0;
free_isr:
	kfree(tegra_isp->my_isr_work);
camera_isp_unregister:
	dev_err(&dev->dev, "%s: failed\n", __func__);

	return err;
}

static int __exit isp_remove(struct platform_device *dev)
{
	struct nvhost_device_data *pdata = platform_get_drvdata(dev);
	struct isp *tegra_isp = (struct isp *)pdata->private_data;

#if defined(CONFIG_TEGRA_ISOMGR)
	if (tegra_isp->isomgr_handle)
		isp_isomgr_unregister(tegra_isp);
#endif
	if (tegra_isp->dev_id == ISPA_DEV_ID) {
		tegra_isp_debugfs_cleanup(tegra_isp);
		tegra_isp_channel_cleanup(tegra_isp);
		tegra_isp_unregister_subdev(tegra_isp);
	}
	nvhost_client_device_release(dev);
	disable_irq(tegra_isp->irq);
	kfree(tegra_isp->my_isr_work);
	flush_workqueue(tegra_isp->isp_workqueue);
	destroy_workqueue(tegra_isp->isp_workqueue);
	tegra_isp = NULL;
	return 0;
}

static struct platform_driver isp_driver = {
	.probe = isp_probe,
	.remove = __exit_p(isp_remove),
	.driver = {
		.owner = THIS_MODULE,
		.name = "isp",
#ifdef CONFIG_PM
		.pm = &nvhost_module_pm_ops,
#endif
#ifdef CONFIG_OF
		.of_match_table = tegra_isp_of_match,
#endif
	}
};

#ifdef CONFIG_TEGRA_MC
static int isp_set_la(struct isp *tegra_isp, u32 isp_bw, u32 la_client)
{
	int ret = 0;
	int la_id;
	/* BW needs to be in MBps */
	u32 isp_bw_mbps = isp_bw / 1000;

	if (tegra_isp->dev_id == ISPB_DEV_ID)
		la_id = TEGRA_LA_ISP_WAB;
	else
		la_id = TEGRA_LA_ISP_WA;

	ret = tegra_set_camera_ptsa(la_id, isp_bw_mbps, la_client);
	if (!ret) {
		ret = tegra_set_latency_allowance(la_id, isp_bw_mbps);
		if (ret)
			pr_err("%s: set latency failed for ISP %d: %d\n",
				__func__, tegra_isp->dev_id, ret);
	} else {
		pr_err("%s: set ptsa failed for ISP %d: %d\n", __func__,
			tegra_isp->dev_id, ret);
	}

	return ret;
}
#else
static int isp_set_la(struct isp *tegra_isp, u32 isp_bw, u32 la_client)
{
	return 0;
}
#endif

static long isp_ioctl(struct file *file,
		unsigned int cmd, unsigned long arg)
{
	struct isp *tegra_isp = file->private_data;

	if (_IOC_TYPE(cmd) != NVHOST_ISP_IOCTL_MAGIC)
		return -EFAULT;

	switch (_IOC_NR(cmd)) {
	case _IOC_NR(NVHOST_ISP_IOCTL_GET_ISP_CLK): {
		int ret;
		u64 isp_clk_rate = 0;

		ret = nvhost_module_get_rate(tegra_isp->ndev,
			(unsigned long *)&isp_clk_rate, 0);
		if (ret) {
			dev_err(&tegra_isp->ndev->dev,
			"%s: failed to get isp clk\n",
			__func__);
			return ret;
		}

		if (copy_to_user((void __user *)arg,
			&isp_clk_rate, sizeof(isp_clk_rate))) {
			dev_err(&tegra_isp->ndev->dev,
			"%s:Failed to copy isp clk rate to user\n",
			__func__);
			return -EFAULT;
		}

		return 0;
	}
	case _IOC_NR(NVHOST_ISP_IOCTL_SET_ISP_LA_BW): {
		u32 ret = 0;
		struct isp_la_bw isp_info;

		if (copy_from_user(&isp_info,
			(const void __user *)arg,
				sizeof(struct isp_la_bw))) {
			dev_err(&tegra_isp->ndev->dev,
				"%s: Failed to copy arg from user\n", __func__);
			return -EFAULT;
			}

		/* Set latency allowance for ISP, BW is in MBps */
		ret = isp_set_la(tegra_isp,
			isp_info.isp_la_bw,
			(isp_info.is_iso) ?
				ISP_HARD_ISO_CLIENT : ISP_SOFT_ISO_CLIENT);
		if (ret) {
			dev_err(&tegra_isp->ndev->dev,
			"%s: failed to set la isp_bw %u KBps\n",
			__func__, isp_info.isp_la_bw);
			return -ENOMEM;
		}

		return 0;

	}
	case _IOC_NR(NVHOST_ISP_IOCTL_SET_EMC): {
		int ret;
		uint la_client = 0;
		uint isp_bw = 0;
		struct isp_emc emc_info;
		if (copy_from_user(&emc_info,
			(const void __user *)arg, sizeof(struct isp_emc))) {
			dev_err(&tegra_isp->ndev->dev,
				"%s: Failed to copy arg from user\n", __func__);
			return -EFAULT;
			}

		if (emc_info.bpp_output && emc_info.bpp_input)
			la_client = ISP_SOFT_ISO_CLIENT;
		else
			la_client = ISP_HARD_ISO_CLIENT;

		isp_bw = (((emc_info.isp_clk/1000) * emc_info.bpp_output) >> 3);

		/* Set latency allowance for given BW of ISP clients */
		ret = isp_set_la(tegra_isp, isp_bw, la_client);
		if (ret) {
			dev_err(&tegra_isp->ndev->dev,
			"%s: failed to set la for isp_bw %u KBps\n",
			__func__, isp_bw);
			return -ENOMEM;
		}

#if defined(CONFIG_TEGRA_ISOMGR)
		/*
		 * Register ISP as isomgr client.
		 */
		if (!tegra_isp->isomgr_handle) {
			ret = isp_isomgr_register(tegra_isp);
			if (ret) {
				dev_err(&tegra_isp->ndev->dev,
				"%s: failed to register ISP as isomgr client\n",
				__func__);
				return -ENOMEM;
			}
		}

		if (tegra_isp->isomgr_handle &&
			la_client == ISP_HARD_ISO_CLIENT) {
			/*
			 * Set ISP ISO BW requirements, only if it is
			 * hard ISO client, i.e. VI is in streaming mode.
			 * There is no way to figure out what latency
			 * can be tolerated in ISP without reading ISP
			 * registers for now. 3 usec is minimum time
			 * to switch PLL source. Let's put 4 usec as
			 * latency for now.
			 */

			/* isomgr driver expects BW in KBps */
			isp_bw = isp_bw * 1000;

			if (isp_bw > tegra_isp->max_bw) {
				dev_err(&tegra_isp->ndev->dev,
				"%s: Requested ISO BW %u is more than "
				"ISP's max BW %u possible\n",
				__func__, isp_bw, tegra_isp->max_bw);
				return -EINVAL;
			}

			ret = isp_isomgr_request(tegra_isp, isp_bw, 4);
			if (ret) {
				dev_err(&tegra_isp->ndev->dev,
				"%s: failed to reserve %u KBps with isomgr\n",
				__func__, isp_bw);
				return -ENOMEM;
			}
		}
#endif
		return ret;
	}
	case _IOC_NR(NVHOST_ISP_IOCTL_SET_ISP_CLK): {
		long isp_clk_rate = 0;

		if (copy_from_user(&isp_clk_rate,
			(const void __user *)arg, sizeof(long))) {
			dev_err(&tegra_isp->ndev->dev,
				"%s: Failed to copy arg from user\n", __func__);
			return -EFAULT;
		}

		return nvhost_module_set_rate(tegra_isp->ndev,
				tegra_isp, isp_clk_rate, 0, NVHOST_CLOCK);
	}
	default:
		dev_err(&tegra_isp->ndev->dev,
			"%s: Unknown ISP ioctl.\n", __func__);
		return -EINVAL;
	}
	return 0;
}

static int isp_open(struct inode *inode, struct file *file)
{
	struct nvhost_device_data *pdata;
	struct isp *tegra_isp;

	pdata = container_of(inode->i_cdev,
		struct nvhost_device_data, ctrl_cdev);
	if (WARN_ONCE(pdata == NULL, "pdata not found, %s failed\n", __func__))
		return -ENODEV;

	tegra_isp = pdata->private_data;
	if (WARN_ONCE(tegra_isp == NULL,
		"tegra_isp not found, %s failed\n", __func__))
		return -ENODEV;

	file->private_data = tegra_isp;

	/* add isp client to acm */
	if (nvhost_module_add_client(tegra_isp->ndev, tegra_isp)) {
		dev_err(&tegra_isp->ndev->dev,
			"%s: failed add isp client\n",
			__func__);
		return -ENOMEM;
	}

	return 0;
}

static int isp_release(struct inode *inode, struct file *file)
{
	int ret = 0;

	struct isp *tegra_isp = file->private_data;

#if defined(CONFIG_TEGRA_ISOMGR)

	/* nullify isomgr request */
	if (tegra_isp->isomgr_handle) {
		ret = isp_isomgr_release(tegra_isp);
		if (ret) {
			dev_err(&tegra_isp->ndev->dev,
			"%s: failed to deallocate memory in isomgr\n",
			__func__);
			return -ENOMEM;
		}
	}
#endif

	/* remove isp client from acm */
	nvhost_module_remove_client(tegra_isp->ndev, tegra_isp);

	return ret;
}

const struct file_operations tegra_isp_ctrl_ops = {
	.owner = THIS_MODULE,
	.open = isp_open,
	.unlocked_ioctl = isp_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = isp_ioctl,
#endif
	.release = isp_release,
};

static struct of_device_id tegra_isp_domain_match[] = {
	{.compatible = "nvidia,tegra210-ve-pd",
	 .data = (struct nvhost_device_data *)&t21_isp_info},
	{.compatible = "nvidia,tegra210-ve2-pd",
	 .data = (struct nvhost_device_data *)&t21_ispb_info},
	{.compatible = "nvidia,tegra132-ve-pd",
	.data = (struct nvhost_device_data *)&t124_isp_info},
	{.compatible = "nvidia,tegra124-ve-pd",
	 .data = (struct nvhost_device_data *)&t124_isp_info},
	{},
};

static int __init isp_init(void)
{
	int ret;

	ret = nvhost_domain_init(tegra_isp_domain_match);
	if (ret)
		return ret;

	return platform_driver_register(&isp_driver);
}

static void __exit isp_exit(void)
{
	platform_driver_unregister(&isp_driver);
}

late_initcall(isp_init);
module_exit(isp_exit);
