/*
 * singleshot_capture.c — Legacy single-shot capture implementation
 *
 * SINGLE_SHOT per frame, ring buffer with N+2 release.
 * Used for TPG, ISP pipeline, T210, and as fallback.
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 */

#include <linux/nvhost.h>
#include <linux/slab.h>

#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>
#include <media/camera_common.h>

#include "camera/mc_common.h"
#include "camera/registers.h"
#include "camera/t124_registers.h"
#include "camera/t210_registers.h"
#include "vi_capture.h"

extern int t124_csi_tpg;
extern int isp_reprocess;

/* ---- ring buffer ---- */

static void init_ring_buffer(struct tegra_channel *chan)
{
	chan->released_bufs = 0;
	chan->num_buffers = 0;
	chan->save_index = 0;
	chan->free_index = 0;
	chan->bfirst_fstart = false;
}

static void free_ring_buffers(struct tegra_channel *chan, int frames)
{
	struct vb2_buffer *vb;

	while (frames) {
		vb = chan->buffers[chan->free_index];
		vb->v4l2_buf.sequence = chan->sequence++;
		vb->v4l2_buf.field = V4L2_FIELD_NONE;
		vb2_set_plane_payload(vb, 0,
				      tegra_channel_get_sizeimage(chan));

		if (chan->capture_state != CAPTURE_GOOD ||
		    chan->released_bufs < 2)
			chan->buffer_state[chan->free_index] =
						VB2_BUF_STATE_ERROR;

		vb2_buffer_done(vb, chan->buffer_state[chan->free_index++]);

		if (chan->free_index >= QUEUED_BUFFERS)
			chan->free_index = 0;
		chan->num_buffers--;
		chan->released_bufs++;
		frames--;
	}
}

static void add_buffer_to_ring(struct tegra_channel *chan,
			       struct vb2_buffer *vb)
{
	chan->buffer_state[chan->save_index] = VB2_BUF_STATE_ERROR;
	chan->buffers[chan->save_index++] = vb;
	if (chan->save_index >= QUEUED_BUFFERS)
		chan->save_index = 0;
	chan->num_buffers++;
}

static void update_state_to_buffer(struct tegra_channel *chan, int state)
{
	int save_index = (chan->save_index - PREVIOUS_BUFFER_DEC_INDEX);

	if (save_index < 0)
		save_index += QUEUED_BUFFERS;
	chan->buffer_state[save_index] = state;

	if (chan->capture_state != CAPTURE_GOOD)
		chan->buffer_state[chan->save_index] = state;
}

static void ring_buffer(struct tegra_channel *chan,
			struct vb2_buffer *vb,
			struct timespec *ts, int state)
{
	if (!chan->bfirst_fstart)
		chan->bfirst_fstart = true;
	else
		update_state_to_buffer(chan, state);

	vb->v4l2_buf.timestamp.tv_sec = ts->tv_sec;
	vb->v4l2_buf.timestamp.tv_usec = ts->tv_nsec / NSEC_PER_USEC;

	if (chan->capture_state != CAPTURE_GOOD) {
		free_ring_buffers(chan, chan->num_buffers);
		init_ring_buffer(chan);
		return;
	}

	if (chan->num_buffers >= (QUEUED_BUFFERS - 1))
		free_ring_buffers(chan, 1);
}

/* ---- dequeue with ring buffer ---- */

static struct tegra_channel_buffer *dequeue_buffer_ring(
					struct tegra_channel *chan)
{
	struct tegra_channel_buffer *buf = NULL;

	spin_lock(&chan->start_lock);
	if (list_empty(&chan->capture))
		goto done;

	buf = list_entry(chan->capture.next,
			 struct tegra_channel_buffer, queue);
	list_del_init(&buf->queue);

	add_buffer_to_ring(chan, &buf->buf);
done:
	spin_unlock(&chan->start_lock);
	return buf;
}

/* ---- capture frame (single-shot per frame) ---- */

static int singleshot_capture_frame(struct tegra_channel *chan,
				    struct tegra_channel_buffer *buf)
{
	struct vb2_buffer *vb = &buf->buf;
	struct timespec ts;
	int err = 0;
	u32 val, frame_start;
	int bytes_per_line = tegra_channel_get_bytesperline(chan);
	int index = 0;
	u32 thresh[TEGRA_CSI_BLOCKS] = { 0 };
	int valid_ports = chan->valid_ports;
	int state = VB2_BUF_STATE_DONE;

	for (index = 0; index < valid_ports; index++) {
		dma_addr_t surface_addr;
		int surface_stride;

		if (chan->use_isp && !isp_reprocess) {
			surface_addr = 0;
			surface_stride = 0;
		} else if (chan->use_isp) {
			surface_addr = chan->isp_raw_dma;
			surface_stride = chan->format.width * 2;
		} else {
			surface_addr = buf->addr + chan->buffer_offset[index];
			surface_stride = bytes_per_line;
		}

		csi_write(chan, index, TEGRA_VI_CSI_SURFACE0_OFFSET_MSB, 0x0);
		csi_write(chan, index,
			  TEGRA_VI_CSI_SURFACE0_OFFSET_LSB, surface_addr);
		csi_write(chan, index,
			  TEGRA_VI_CSI_SURFACE0_STRIDE, surface_stride);

		/* Program syncpoints */
		thresh[index] = nvhost_syncpt_incr_max_ext(chan->vi->ndev,
					chan->syncpt[index], 1);
		if (!chan->syncpoint_fifo[index]) {
#if defined(CONFIG_ARCH_TEGRA_12x_SOC) || defined(CONFIG_ARCH_TEGRA_13x_SOC)
			frame_start = (chan->port[index] == 0) ?
				T124_PPA_FRAME_START : T124_PPB_FRAME_START;
#else
			frame_start = T210_VI_CSI_PP_FRAME_START(
							chan->port[index]);
#endif
			val = VI_CFG_VI_INCR_SYNCPT_COND(frame_start) |
				chan->syncpt[index];
			tegra_channel_write(chan,
				TEGRA_VI_CFG_VI_INCR_SYNCPT, val);
		} else {
			chan->syncpoint_fifo[index]--;
		}
	}

#if defined(CONFIG_ARCH_TEGRA_12x_SOC) || defined(CONFIG_ARCH_TEGRA_13x_SOC)
	if (t124_csi_tpg) {
		/* TPG path: ARM MW_ACK before SINGLE_SHOT, direct return */
		u32 mw_thresh;
		int mw_ack_cond = (chan->port[0] == 0) ?
			T124_MWA_ACK_DONE : T124_MWB_ACK_DONE;

		mw_thresh = nvhost_syncpt_incr_max_ext(chan->vi->ndev,
					chan->syncpt[0], 1);
		val = VI_CFG_VI_INCR_SYNCPT_COND(mw_ack_cond) |
			chan->syncpt[0];
		tegra_channel_write(chan, TEGRA_VI_CFG_VI_INCR_SYNCPT, val);

		if (!chan->bfirst_fstart) {
			err = tegra_channel_enable_stream(chan);
			if (err) {
				state = VB2_BUF_STATE_ERROR;
				chan->capture_state = CAPTURE_ERROR;
				ring_buffer(chan, vb, &ts, state);
				return err;
			}
			val = csi_read(chan, 0, TEGRA_VI_CSI_IMAGE_DEF);
			csi_write(chan, 0, TEGRA_VI_CSI_IMAGE_DEF,
				  val | IMAGE_DEF_DEST_MEM);
		}

		csi_write(chan, 0, TEGRA_VI_CSI_SINGLE_SHOT,
			  SINGLE_SHOT_CAPTURE);

		chan->capture_state = CAPTURE_GOOD;
		err = nvhost_syncpt_wait_timeout_ext(chan->vi->ndev,
				chan->syncpt[0], thresh[0],
				chan->timeout, NULL, &ts);
		if (err) {
			state = VB2_BUF_STATE_ERROR;
			chan->capture_state = CAPTURE_TIMEOUT;
		} else {
			err = nvhost_syncpt_wait_timeout_ext(chan->vi->ndev,
					chan->syncpt[0], mw_thresh,
					chan->timeout, NULL, &ts);
			if (err)
				state = VB2_BUF_STATE_ERROR;
		}

		vb->v4l2_buf.sequence = chan->sequence++;
		vb->v4l2_buf.field = V4L2_FIELD_NONE;
		getrawmonotonic(&ts);
		vb->v4l2_buf.timestamp.tv_sec = ts.tv_sec;
		vb->v4l2_buf.timestamp.tv_usec = ts.tv_nsec / NSEC_PER_USEC;
		vb2_set_plane_payload(vb, 0,
				      tegra_channel_get_sizeimage(chan));
		vb2_buffer_done(vb, state);
		return 0;
	}
#endif

	/* Standard single-shot sensor path */
	if (!chan->bfirst_fstart) {
		err = tegra_channel_enable_stream(chan);
		if (err) {
			state = VB2_BUF_STATE_ERROR;
			chan->capture_state = CAPTURE_ERROR;
			ring_buffer(chan, vb, &ts, state);
			return err;
		}
		for (index = 0; index < valid_ports; index++) {
			val = csi_read(chan, index, TEGRA_VI_CSI_IMAGE_DEF);
			if (chan->use_isp && !isp_reprocess)
				csi_write(chan, index,
					  TEGRA_VI_CSI_IMAGE_DEF,
					  val | ((chan->port[0] == 0) ?
					  IMAGE_DEF_DEST_ISP_A :
					  IMAGE_DEF_DEST_ISP_B));
			else
				csi_write(chan, index,
					  TEGRA_VI_CSI_IMAGE_DEF,
					  val | IMAGE_DEF_DEST_MEM);
		}
	}

	/* SINGLE_SHOT trigger */
	for (index = 0; index < valid_ports; index++)
		csi_write(chan, index,
			  TEGRA_VI_CSI_SINGLE_SHOT, SINGLE_SHOT_CAPTURE);

	/* Wait FRAME_START */
	chan->capture_state = CAPTURE_GOOD;
	for (index = 0; index < valid_ports; index++) {
		err = nvhost_syncpt_wait_timeout_ext(chan->vi->ndev,
			chan->syncpt[index], thresh[index],
			chan->timeout, NULL, &ts);
		if (err) {
			dev_err(&chan->video.dev,
				"frame start syncpt timeout!%d\n", index);
			state = VB2_BUF_STATE_ERROR;
			tegra_channel_ec_recover(chan);
			chan->capture_state = CAPTURE_TIMEOUT;
			break;
		}
	}

	/* Wait MW_ACK_DONE */
	if (!err) {
		for (index = 0; index < valid_ports; index++) {
			u32 mw_thresh, mw_cond;
#if defined(CONFIG_ARCH_TEGRA_12x_SOC) || defined(CONFIG_ARCH_TEGRA_13x_SOC)
			mw_cond = (chan->port[index] == 0) ?
				T124_MWA_ACK_DONE : T124_MWB_ACK_DONE;
#else
			mw_cond = T210_VI_CSI_MW_ACK_DONE(chan->port[index]);
#endif
			mw_thresh = nvhost_syncpt_incr_max_ext(chan->vi->ndev,
						chan->syncpt[index], 1);
			tegra_channel_write(chan,
				TEGRA_VI_CFG_VI_INCR_SYNCPT,
				VI_CFG_VI_INCR_SYNCPT_COND(mw_cond) |
				chan->syncpt[index]);
			err = nvhost_syncpt_wait_timeout_ext(chan->vi->ndev,
				chan->syncpt[index], mw_thresh,
				chan->timeout, NULL, &ts);
			if (err) {
				dev_err(&chan->video.dev,
					"MW_ACK_DONE timeout!%d\n", index);
				state = VB2_BUF_STATE_ERROR;
				break;
			}
		}
	}

	if (!err && !chan->vi->pg_mode) {
		err = tegra_channel_error_status(chan);
		if (err) {
			state = VB2_BUF_STATE_ERROR;
			chan->capture_state = CAPTURE_ERROR;
		}
	}

	ring_buffer(chan, vb, &ts, state);
	return 0;
}

/* ---- last-frame MW_ACK at stop_streaming ---- */

static void singleshot_capture_done_last(struct tegra_channel *chan)
{
	struct timespec ts;
	int index, err;
	int bytes_per_line = chan->format.bytesperline;
	u32 val, mw_ack_done;
	u32 thresh[TEGRA_CSI_BLOCKS] = { 0 };
	struct tegra_channel_buffer *buf;
	int state = VB2_BUF_STATE_DONE;

	buf = dequeue_buffer_ring(chan);
	if (!buf)
		return;

	for (index = 0; index < chan->valid_ports; index++) {
		csi_write(chan, index, TEGRA_VI_CSI_SURFACE0_OFFSET_MSB, 0x0);
		csi_write(chan, index, TEGRA_VI_CSI_SURFACE0_OFFSET_LSB,
			  buf->addr + chan->buffer_offset[index]);
		csi_write(chan, index, TEGRA_VI_CSI_SURFACE0_STRIDE,
			  bytes_per_line);

		thresh[index] = nvhost_syncpt_incr_max_ext(chan->vi->ndev,
					chan->syncpt[index], 1);
#if defined(CONFIG_ARCH_TEGRA_12x_SOC) || defined(CONFIG_ARCH_TEGRA_13x_SOC)
		mw_ack_done = (chan->port[index] == 0) ?
			T124_MWA_ACK_DONE : T124_MWB_ACK_DONE;
#else
		mw_ack_done = T210_VI_CSI_MW_ACK_DONE(chan->port[index]);
#endif
		val = VI_CFG_VI_INCR_SYNCPT_COND(mw_ack_done) |
			chan->syncpt[index];
		tegra_channel_write(chan, TEGRA_VI_CFG_VI_INCR_SYNCPT, val);

		csi_write(chan, index,
			  TEGRA_VI_CSI_SINGLE_SHOT, SINGLE_SHOT_CAPTURE);
	}

	for (index = 0; index < chan->valid_ports; index++) {
		err = nvhost_syncpt_wait_timeout_ext(chan->vi->ndev,
			chan->syncpt[index], thresh[index],
			chan->timeout, NULL, &ts);
		if (err) {
			state = VB2_BUF_STATE_ERROR;
			tegra_channel_ec_recover(chan);
			chan->capture_state = CAPTURE_TIMEOUT;
			break;
		}
	}
	chan->capture_state = CAPTURE_IDLE;
	ring_buffer(chan, &buf->buf, &ts, state);
}

/* ---- ops ---- */

static int ss_start_streaming(struct tegra_channel *chan)
{
	init_ring_buffer(chan);
	return 0;
}

static void ss_stop_streaming(struct tegra_channel *chan)
{
	bool is_streaming = atomic_read(&chan->is_streaming);

	if (is_streaming && chan->capture_state == CAPTURE_GOOD)
		singleshot_capture_done_last(chan);
	free_ring_buffers(chan, chan->num_buffers);
}

static int ss_capture_start(struct tegra_channel *chan,
			    struct tegra_channel_buffer *buf)
{
	return singleshot_capture_frame(chan, buf);
}

const struct tegra_vi_capture_ops tegra_vi_singleshot_capture_ops = {
	.start_streaming = ss_start_streaming,
	.stop_streaming  = ss_stop_streaming,
	.capture_start   = ss_capture_start,
};
