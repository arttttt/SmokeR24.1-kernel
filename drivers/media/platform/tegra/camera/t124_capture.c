/*
 * t124_capture.c — Tegra T124 continuous capture with pre-queue pipeline
 *
 * Shadow register model (like Rockchip ISP1):
 *   After each FRAME_START, immediately program next surface + arm syncpt.
 *   Start-thread waits on the pre-armed syncpt, not re-arming.
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 * Copyright (c) 2026, arttttt. Pre-queue pipeline.
 */

#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/nvhost.h>
#include <linux/dma-mapping.h>

#include <media/videobuf2-core.h>
#include <media/camera_common.h>

#include "camera/mc_common.h"
#include "camera/registers.h"
#include "camera/t124_registers.h"
#include "vi_capture.h"

extern int t124_single_shot;

/* ---- helpers ---- */

static void surface_setup(struct tegra_channel *chan,
			  int index, dma_addr_t addr, int stride)
{
	csi_write(chan, index, TEGRA_VI_CSI_SURFACE0_OFFSET_MSB, 0x0);
	csi_write(chan, index, TEGRA_VI_CSI_SURFACE0_OFFSET_LSB, addr);
	csi_write(chan, index, TEGRA_VI_CSI_SURFACE0_STRIDE, stride);
}

static void arm_frame_start(struct tegra_channel *chan,
			    int index, u32 *thresh)
{
	u32 cond = (chan->port[index] == 0) ?
		T124_PPA_FRAME_START : T124_PPB_FRAME_START;

	*thresh = nvhost_syncpt_incr_max_ext(chan->vi->ndev,
					     chan->syncpt[index], 1);
	if (!chan->syncpoint_fifo[index])
		tegra_channel_write(chan, TEGRA_VI_CFG_VI_INCR_SYNCPT,
			VI_CFG_VI_INCR_SYNCPT_COND(cond) |
			chan->syncpt[index]);
	else
		chan->syncpoint_fifo[index]--;
}

static int wait_syncpt(struct tegra_channel *chan,
		       int index, u32 thresh, struct timespec *ts)
{
	return nvhost_syncpt_wait_timeout_ext(chan->vi->ndev,
		chan->syncpt[index], thresh, chan->timeout, NULL, ts);
}

static int arm_wait_mw_ack(struct tegra_channel *chan,
			   int index, struct timespec *ts)
{
	u32 cond = (chan->port[index] == 0) ?
		T124_MWA_ACK_DONE : T124_MWB_ACK_DONE;
	u32 thresh;

	thresh = nvhost_syncpt_incr_max_ext(chan->vi->ndev,
					    chan->syncpt_mw[index], 1);
	tegra_channel_write(chan, TEGRA_VI_CFG_VI_INCR_SYNCPT,
		VI_CFG_VI_INCR_SYNCPT_COND(cond) | chan->syncpt_mw[index]);

	return nvhost_syncpt_wait_timeout_ext(chan->vi->ndev,
		chan->syncpt_mw[index], thresh, chan->timeout, NULL, ts);
}

static void buffer_done(struct tegra_channel *chan, struct vb2_buffer *vb,
			struct timespec *ts, int state)
{
	vb->v4l2_buf.sequence = chan->sequence++;
	vb->v4l2_buf.field = V4L2_FIELD_NONE;
	vb->v4l2_buf.timestamp.tv_sec = ts->tv_sec;
	vb->v4l2_buf.timestamp.tv_usec = ts->tv_nsec / NSEC_PER_USEC;
	vb2_set_plane_payload(vb, 0, tegra_channel_get_sizeimage(chan));
	vb2_buffer_done(vb, state);
}

/* Wait for CSI to become idle (between frames).
 * Polls READONLY_STATUS — 0 means no active transfer.
 * This confirms we're in vertical blanking, safe to change surface. */
static void csi_pending(struct tegra_channel *chan, int port)
{
	int timeout = 1000;
	u32 port_mask = (chan->port[port] == 0) ? 0x1 : 0x2;

	while (--timeout) {
		if (!(tegra_channel_read(chan, T124_CSI_READONLY_STATUS)
		      & port_mask))
			break;
		usleep_range(1, 2);
	}
	if (!timeout)
		dev_warn(&chan->video.dev, "csi_pending timeout port %d\n",
			 port);
}

/* Pre-queue: program next surface + arm FRAME_START syncpt.
 * Called after MW_ACK + csi_pending — we're in vertical blanking,
 * safe to change surface address. */
static void prequeue_next(struct tegra_channel *chan)
{
	struct tegra_channel_buffer *next = NULL;
	int bpl = tegra_channel_get_bytesperline(chan);
	int index;

	spin_lock(&chan->start_lock);
	if (!list_empty(&chan->capture)) {
		next = list_entry(chan->capture.next,
				  struct tegra_channel_buffer, queue);
		list_del_init(&next->queue);
	}
	spin_unlock(&chan->start_lock);

	if (next) {
		for (index = 0; index < chan->valid_ports; index++) {
			surface_setup(chan, index,
				      next->addr + chan->buffer_offset[index],
				      bpl);
			arm_frame_start(chan, index,
					&chan->next_thresh[index]);
		}
		if (t124_single_shot)
			for (index = 0; index < chan->valid_ports; index++)
				csi_write(chan, index,
					  TEGRA_VI_CSI_SINGLE_SHOT,
					  SINGLE_SHOT_CAPTURE);
		chan->next_buf = next;
		chan->next_armed = true;
	} else {
		/* No buffer — dummy surface, don't arm syncpt.
		 * Start-thread will wait for userspace QBUF,
		 * then do full setup. */
		if (chan->dummy_buf_dma)
			for (index = 0; index < chan->valid_ports; index++)
				surface_setup(chan, index,
					      chan->dummy_buf_dma, bpl);
		chan->next_buf = NULL;
		chan->next_armed = false;
	}
}

/* ---- start phase (start-thread) ---- */

static int warmup(struct tegra_channel *chan, struct tegra_channel_buffer *buf)
{
	struct timespec ts;
	int index, err = 0;

	for (index = 0; index < chan->valid_ports; index++) {
		u32 thresh;
		int i;

		for (i = 0; i < 2; i++) {
			arm_frame_start(chan, index, &thresh);
			if (t124_single_shot)
				csi_write(chan, index,
					  TEGRA_VI_CSI_SINGLE_SHOT,
					  SINGLE_SHOT_CAPTURE);
			err = wait_syncpt(chan, index, thresh, &ts);
			if (err) {
				dev_err(&chan->video.dev,
					"warmup FRAME_START %d timeout\n", i);
				return err;
			}
		}
	}

	for (index = 0; index < chan->valid_ports; index++) {
		u32 val = csi_read(chan, index, TEGRA_VI_CSI_IMAGE_DEF);
		csi_write(chan, index, TEGRA_VI_CSI_IMAGE_DEF,
			  val | IMAGE_DEF_DEST_MEM);
	}

	dev_info(&chan->video.dev, "warmup complete: pipeline ready\n");
	return 0;
}

static int t124_capture_start(struct tegra_channel *chan,
			      struct tegra_channel_buffer *buf)
{
	int err, index;
	int bpl = tegra_channel_get_bytesperline(chan);
	u32 thresh[TEGRA_CSI_BLOCKS] = { 0 };
	struct timespec ts;

	/* First frame: full setup */
	if (!chan->bfirst_fstart) {
		err = tegra_channel_enable_stream(chan);
		if (err) {
			chan->capture_state = CAPTURE_ERROR;
			buffer_done(chan, &buf->buf, &ts, VB2_BUF_STATE_ERROR);
			return err;
		}

		for (index = 0; index < chan->valid_ports; index++)
			surface_setup(chan, index,
				      buf->addr + chan->buffer_offset[index],
				      bpl);

		err = warmup(chan, buf);
		if (err) {
			chan->capture_state = CAPTURE_ERROR;
			buffer_done(chan, &buf->buf, &ts, VB2_BUF_STATE_ERROR);
			return err;
		}
		chan->bfirst_fstart = true;

		/* Arm + wait for first frame */
		for (index = 0; index < chan->valid_ports; index++)
			arm_frame_start(chan, index, &thresh[index]);

		if (t124_single_shot)
			for (index = 0; index < chan->valid_ports; index++)
				csi_write(chan, index,
					  TEGRA_VI_CSI_SINGLE_SHOT,
					  SINGLE_SHOT_CAPTURE);
	} else if (chan->next_armed) {
		/* Pre-armed by prequeue_next — use stored thresholds */
		for (index = 0; index < chan->valid_ports; index++)
			thresh[index] = chan->next_thresh[index];
		chan->next_armed = false;
	} else {
		/* Recovering from dummy fallback — full setup */
		for (index = 0; index < chan->valid_ports; index++) {
			surface_setup(chan, index,
				      buf->addr + chan->buffer_offset[index],
				      bpl);
			arm_frame_start(chan, index, &thresh[index]);
		}

		if (t124_single_shot)
			for (index = 0; index < chan->valid_ports; index++)
				csi_write(chan, index,
					  TEGRA_VI_CSI_SINGLE_SHOT,
					  SINGLE_SHOT_CAPTURE);
	}

	/* Wait FRAME_START */
	chan->capture_state = CAPTURE_GOOD;
	for (index = 0; index < chan->valid_ports; index++) {
		err = wait_syncpt(chan, index, thresh[index], &ts);
		if (err) {
			dev_err(&chan->video.dev,
				"FRAME_START timeout port %d\n", index);
			tegra_channel_ec_recover(chan);
			chan->capture_state = CAPTURE_TIMEOUT;
			buffer_done(chan, &buf->buf, &ts, VB2_BUF_STATE_ERROR);
			return err;
		}
	}

	/* DMA started — hand off to done-thread for MW_ACK.
	 * Surface is reprogrammed AFTER MW_ACK (in capture_done),
	 * not here — T124 VI has no shadow registers, writing
	 * surface during DMA causes tearing. */

	/* Hand off to done-thread for MW_ACK */
	atomic_set(&chan->dma_active, 1);
	spin_lock(&chan->done_lock);
	list_add_tail(&buf->queue, &chan->done);
	spin_unlock(&chan->done_lock);
	wake_up_interruptible(&chan->done_wait);

	return 0;
}

/* ---- done phase (done-thread) ---- */

static void t124_capture_done(struct tegra_channel *chan,
			      struct tegra_channel_buffer *buf)
{
	struct timespec ts;
	int err = 0, index;
	int state = VB2_BUF_STATE_DONE;

	for (index = 0; index < chan->valid_ports; index++) {
		err = arm_wait_mw_ack(chan, index, &ts);
		if (err) {
			dev_err(&chan->video.dev,
				"MW_ACK_DONE timeout port %d\n", index);
			state = VB2_BUF_STATE_ERROR;
			break;
		}
	}

	atomic_set(&chan->dma_active, 0);
	wake_up_interruptible(&chan->dma_wait);

	if (!err) {
		err = tegra_channel_error_status(chan);
		if (err) {
			state = VB2_BUF_STATE_ERROR;
			chan->capture_state = CAPTURE_ERROR;
		}
	}

	/* Wait for CSI idle + pre-queue next surface.
	 * csi_pending confirms we're in vertical blanking.
	 * Then safe to change surface address + return buffer. */
	if (!err) {
		int idx;
		for (idx = 0; idx < chan->valid_ports; idx++)
			csi_pending(chan, idx);
		prequeue_next(chan);
	}

	if (!err)
		getrawmonotonic(&ts);
	buffer_done(chan, &buf->buf, &ts, state);
}

static int kthread_done(void *data)
{
	struct tegra_channel *chan = data;
	struct tegra_channel_buffer *buf;
	struct sched_param param = { .sched_priority = 2 };

	sched_setscheduler(current, SCHED_FIFO, &param);
	set_freezable();

	while (1) {
		try_to_freeze();
		wait_event_interruptible(chan->done_wait,
					 !list_empty(&chan->done) ||
					 kthread_should_stop());

		if (kthread_should_stop() && list_empty(&chan->done)) {
			complete(&chan->capture_done_comp);
			break;
		}

		spin_lock(&chan->done_lock);
		if (list_empty(&chan->done)) {
			spin_unlock(&chan->done_lock);
			continue;
		}
		buf = list_entry(chan->done.next,
				 struct tegra_channel_buffer, queue);
		list_del_init(&buf->queue);
		spin_unlock(&chan->done_lock);

		t124_capture_done(chan, buf);
	}
	return 0;
}

/* ---- ops ---- */

static int t124_start_streaming(struct tegra_channel *chan)
{
	int i;
	size_t sz;

	for (i = 0; i < chan->valid_ports; i++)
		nvhost_syncpt_set_min_eq_max_ext(chan->vi->ndev,
						 chan->syncpt_mw[i]);

	sz = PAGE_ALIGN(chan->format.sizeimage);
	chan->dummy_buf_cpu = dma_alloc_coherent(chan->vi->dev, sz,
						&chan->dummy_buf_dma, GFP_KERNEL);
	if (!chan->dummy_buf_cpu)
		dev_warn(&chan->video.dev,
			 "dummy buf alloc failed (%zu)\n", sz);

	chan->next_buf = NULL;
	chan->next_armed = false;

	chan->kthread_capture_done = kthread_run(kthread_done, chan,
						"%s-done", chan->video.name);
	if (IS_ERR(chan->kthread_capture_done)) {
		int ret = PTR_ERR(chan->kthread_capture_done);
		chan->kthread_capture_done = NULL;
		return ret;
	}
	return 0;
}

static void t124_stop_streaming(struct tegra_channel *chan)
{
	struct tegra_channel_buffer *buf;

	if (chan->kthread_capture_done) {
		kthread_stop(chan->kthread_capture_done);
		wait_for_completion(&chan->capture_done_comp);
		chan->kthread_capture_done = NULL;
	}

	if (chan->next_buf) {
		vb2_buffer_done(&chan->next_buf->buf, VB2_BUF_STATE_ERROR);
		chan->next_buf = NULL;
	}
	chan->next_armed = false;

	spin_lock(&chan->done_lock);
	while (!list_empty(&chan->done)) {
		buf = list_entry(chan->done.next,
				 struct tegra_channel_buffer, queue);
		list_del_init(&buf->queue);
		vb2_buffer_done(&buf->buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock(&chan->done_lock);

	if (chan->dummy_buf_cpu) {
		dma_free_coherent(chan->vi->dev,
				  PAGE_ALIGN(chan->format.sizeimage),
				  chan->dummy_buf_cpu, chan->dummy_buf_dma);
		chan->dummy_buf_cpu = NULL;
		chan->dummy_buf_dma = 0;
	}
}

const struct tegra_vi_capture_ops tegra_vi_t124_capture_ops = {
	.start_streaming = t124_start_streaming,
	.stop_streaming  = t124_stop_streaming,
	.capture_start   = t124_capture_start,
};
