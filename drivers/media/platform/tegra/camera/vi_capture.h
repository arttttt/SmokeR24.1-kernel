/*
 * vi_capture.h — Tegra VI capture ops abstraction
 *
 * Implementations:
 *   t124_capture.c      — T124 continuous mode, 2-kthread pipeline
 *   t124_singleshot.c   — T124 per-frame single-shot, 2-kthread (no tearing)
 */

#ifndef __VI_CAPTURE_H__
#define __VI_CAPTURE_H__

struct tegra_channel;
struct tegra_channel_buffer;

struct tegra_vi_capture_ops {
	int  (*start_streaming)(struct tegra_channel *chan);
	void (*stop_streaming)(struct tegra_channel *chan);
	int  (*capture_start)(struct tegra_channel *chan,
			      struct tegra_channel_buffer *buf);
};

extern const struct tegra_vi_capture_ops tegra_vi_t124_capture_ops;
extern const struct tegra_vi_capture_ops tegra_vi_t124_singleshot_ops;

/* Shared helpers (channel.c) */
u32 tegra_channel_read(struct tegra_channel *chan, unsigned int addr);
void tegra_channel_write(struct tegra_channel *chan,
			 unsigned int addr, u32 val);
void csi_write(struct tegra_channel *chan, unsigned int index,
	       unsigned int addr, u32 val);
u32 csi_read(struct tegra_channel *chan, unsigned int index,
	     unsigned int addr);
unsigned int tegra_channel_get_sizeimage(struct tegra_channel *chan);
int tegra_channel_get_bytesperline(struct tegra_channel *chan);
int tegra_channel_enable_stream(struct tegra_channel *chan);
int tegra_channel_error_status(struct tegra_channel *chan);
void tegra_channel_ec_recover(struct tegra_channel *chan);
struct tegra_channel_buffer *dequeue_buffer_simple(struct tegra_channel *chan);

#endif
