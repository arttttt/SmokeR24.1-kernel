/*
 * t124_singleshot.c — Tegra T124 per-frame single-shot capture
 *
 * Based on vi2.c from the same kernel tree (soc_camera/tegra_camera/).
 * PP is set to single-shot mode once at stream start. Each frame is
 * triggered by VI_CSI_SINGLE_SHOT register write. No PP disable/re-enable
 * per frame — PP waits for the next SINGLE_SHOT command automatically.
 *
 * 2-kthread pipeline:
 *   start-thread: surface setup → VI SINGLE_SHOT → wait FRAME_START
 *   done-thread:  MW_ACK wait → buffer_done
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 * Copyright (c) 2026, arttttt. Single-shot 2-kthread pipeline.
 */

#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/nvhost.h>

#include <media/videobuf2-core.h>
#include <media/camera_common.h>

#include "camera/mc_common.h"
#include "camera/registers.h"
#include "camera/t124_registers.h"
#include "vi_capture.h"
#include "isp_t124.h"

extern int isp_reprocess;

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

/* ---- start phase (start-thread) ---- */

static int t124_ss_capture_start(struct tegra_channel *chan,
				 struct tegra_channel_buffer *buf)
{
	int err, index;
	int bpl = tegra_channel_get_bytesperline(chan);
	u32 thresh[TEGRA_CSI_BLOCKS] = { 0 };
	struct timespec ts;

	/* First frame: enable stream + set destination */
	if (!chan->bfirst_fstart) {
		err = tegra_channel_enable_stream(chan);
		if (err) {
			chan->capture_state = CAPTURE_ERROR;
			buffer_done(chan, &buf->buf, &ts, VB2_BUF_STATE_ERROR);
			return err;
		}
		for (index = 0; index < chan->valid_ports; index++) {
			u32 val = csi_read(chan, index,
					   TEGRA_VI_CSI_IMAGE_DEF);
			if (chan->use_isp && !isp_reprocess)
				/* ISP streaming: VI → ISP directly */
				csi_write(chan, index,
					  TEGRA_VI_CSI_IMAGE_DEF,
					  val | ((chan->port[0] == 0) ?
					  IMAGE_DEF_DEST_ISP_A :
					  IMAGE_DEF_DEST_ISP_B));
			else
				/* Normal or ISP reprocess: VI → memory */
				csi_write(chan, index,
					  TEGRA_VI_CSI_IMAGE_DEF,
					  val | IMAGE_DEF_DEST_MEM);
		}
		chan->bfirst_fstart = true;
	}

	/* Program surface address based on ISP mode */
	for (index = 0; index < chan->valid_ports; index++) {
		dma_addr_t addr;
		int stride;

		if (chan->use_isp && !isp_reprocess) {
			/* ISP streaming: no memory write from VI */
			addr = 0;
			stride = 0;
		} else if (chan->use_isp) {
			/* ISP reprocess: VI writes raw to isp_raw_dma */
			addr = chan->isp_raw_dma;
			stride = chan->format.width * 2;
		} else {
			/* Normal: VI writes to V4L2 buffer */
			addr = buf->addr + chan->buffer_offset[index];
			stride = bpl;
		}
		surface_setup(chan, index, addr, stride);
	}

	/* Re-arm PP single-shot per frame (vi2.c does this every frame) */
	for (index = 0; index < chan->valid_ports; index++) {
		u32 pp_reg = (chan->port[index] == 0) ?
			T124_PP_A_PIXEL_STREAM_PP_COMMAND :
			T124_PP_B_PIXEL_STREAM_PP_COMMAND;
		tegra_channel_write(chan, pp_reg,
			(0xF << CSI_PP_START_MARKER_FRAME_MAX_OFFSET) |
			CSI_PP_SINGLE_SHOT_ENABLE | CSI_PP_ENABLE);
	}

	/* Arm FRAME_START syncpt + trigger SINGLE_SHOT */
	for (index = 0; index < chan->valid_ports; index++)
		arm_frame_start(chan, index, &thresh[index]);
	for (index = 0; index < chan->valid_ports; index++)
		csi_write(chan, index, TEGRA_VI_CSI_SINGLE_SHOT,
			  SINGLE_SHOT_CAPTURE);

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

	/* DMA started — hand off to done-thread for MW_ACK */
	spin_lock(&chan->done_lock);
	list_add_tail(&buf->queue, &chan->done);
	spin_unlock(&chan->done_lock);
	wake_up_interruptible(&chan->done_wait);

	return 0;
}

/* ---- done phase (done-thread) ---- */

static void t124_ss_capture_done(struct tegra_channel *chan,
				 struct tegra_channel_buffer *buf)
{
	struct timespec ts;
	int err = 0, index;
	int state = VB2_BUF_STATE_DONE;

	if (chan->use_isp && !isp_reprocess) {
		/* ISP streaming: VI→ISP directly, no memory write from VI.
		 * Submit ISP job + wait for ISP OP_DONE. */
		err = isp_t124_process_frame(chan->isp,
				chan->isp_out_dma, 0);
		if (err) {
			dev_err(&chan->video.dev,
				"ISP process_frame failed: %d\n", err);
			state = VB2_BUF_STATE_ERROR;
		}
		if (!err) {
			err = isp_t124_wait_frame(chan->isp);
			if (err) {
				dev_err(&chan->video.dev,
					"ISP wait_frame timeout\n");
				state = VB2_BUF_STATE_ERROR;
			}
		}
	} else if (chan->use_isp && isp_reprocess) {
		/* ISP reprocess: VI→memory→ISP.
		 * First wait MW_ACK (VI write to isp_raw_dma),
		 * then submit ISP reprocess job. */
		for (index = 0; index < chan->valid_ports; index++) {
			err = arm_wait_mw_ack(chan, index, &ts);
			if (err) {
				dev_err(&chan->video.dev,
					"MW_ACK_DONE timeout port %d\n",
					index);
				state = VB2_BUF_STATE_ERROR;
				break;
			}
		}
		if (!err) {
			err = isp_t124_process_frame_reprocess(chan->isp,
					chan->isp_raw_dma,
					chan->isp_out_dma, 0);
			if (err) {
				dev_err(&chan->video.dev,
					"ISP reprocess failed: %d\n", err);
				state = VB2_BUF_STATE_ERROR;
			}
		}
		if (!err) {
			err = isp_t124_wait_frame(chan->isp);
			if (err) {
				dev_err(&chan->video.dev,
					"ISP wait_frame timeout\n");
				state = VB2_BUF_STATE_ERROR;
			}
		}
	} else {
		/* Normal: VI→memory, wait MW_ACK */
		for (index = 0; index < chan->valid_ports; index++) {
			err = arm_wait_mw_ack(chan, index, &ts);
			if (err) {
				dev_err(&chan->video.dev,
					"MW_ACK_DONE timeout port %d\n",
					index);
				state = VB2_BUF_STATE_ERROR;
				break;
			}
		}
	}

	if (!err) {
		err = tegra_channel_error_status(chan);
		if (err) {
			state = VB2_BUF_STATE_ERROR;
			chan->capture_state = CAPTURE_ERROR;
		}
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

		t124_ss_capture_done(chan, buf);
	}
	return 0;
}

/* ---- ops ---- */

static int t124_ss_start_streaming(struct tegra_channel *chan)
{
	int i;

	for (i = 0; i < chan->valid_ports; i++)
		nvhost_syncpt_set_min_eq_max_ext(chan->vi->ndev,
						 chan->syncpt_mw[i]);

	chan->kthread_capture_done = kthread_run(kthread_done, chan,
						"%s-done", chan->video.name);
	if (IS_ERR(chan->kthread_capture_done)) {
		int ret = PTR_ERR(chan->kthread_capture_done);
		chan->kthread_capture_done = NULL;
		return ret;
	}
	return 0;
}

static void t124_ss_stop_streaming(struct tegra_channel *chan)
{
	struct tegra_channel_buffer *buf;

	if (chan->kthread_capture_done) {
		kthread_stop(chan->kthread_capture_done);
		wait_for_completion(&chan->capture_done_comp);
		chan->kthread_capture_done = NULL;
	}

	spin_lock(&chan->done_lock);
	while (!list_empty(&chan->done)) {
		buf = list_entry(chan->done.next,
				 struct tegra_channel_buffer, queue);
		list_del_init(&buf->queue);
		vb2_buffer_done(&buf->buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock(&chan->done_lock);
}

const struct tegra_vi_capture_ops tegra_vi_t124_singleshot_ops = {
	.start_streaming = t124_ss_start_streaming,
	.stop_streaming  = t124_ss_stop_streaming,
	.capture_start   = t124_ss_capture_start,
};
