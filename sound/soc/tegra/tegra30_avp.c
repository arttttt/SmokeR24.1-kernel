/*
 * tegra30_avp.c - Tegra AVP audio driver
 *
 * Author: Sumit Bhattacharya <sumitb@nvidia.com>
 * Copyright (c) 2014-2015, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

/*
#define VERBOSE_DEBUG 1
#define DEBUG 1
#define DEBUG_AVP
*/

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/completion.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include <linux/kthread.h>

#include <linux/dmaengine.h>

#include <sound/compress_offload.h>
#include <sound/compress_params.h>
#include <sound/pcm.h>

#include <linux/tegra_avp_audio.h>
#include "tegra_offload.h"

#include <linux/tegra_nvavp.h>
#include "../../drivers/media/platform/tegra/nvavp/nvavp_os.h"

#define DRV_NAME "tegra30-avp-audio"

/******************************************************************************
 * DEFINES, ENUMS & GLOBALS
 *****************************************************************************/
#define AVP_UCODE_HEADER		"NVAVPAPP"
#define AVP_UCODE_HEADER_SIZE		(sizeof(AVP_UCODE_HEADER) - 1)
#define AVP_INIT_SAMPLE_RATE		48000

#define AVP_COMPR_THRESHOLD		(4 * 1024)
#define AVP_UNITY_STREAM_VOLUME		0x10000

#define AVP_CMD_BUFFER_SIZE		256

enum avp_compr_formats {
	avp_compr_mp3,
	avp_compr_aac,
	avp_compr_max,
};

#ifdef DEBUG_AVP
#define DUMP_AVP_STATUS(s) \
	pr_info("%s %d : id %d:: SS %d Halt %d RP %d %d PP %d LP %d FD %d "\
		"WP %d WC %d DS %d DBWP %d SR %d DBRP %d\n",\
		__func__, __LINE__,\
		s->id,\
		(int)s->stream->stream_state_current,\
		s->stream->halted,\
		s->stream->source_buffer_read_position,\
		s->stream->source_buffer_read_position_fraction,\
		s->stream->source_buffer_presentation_position,\
		s->stream->source_buffer_linear_position,\
		s->stream->source_buffer_frames_decoded,\
		s->stream->source_buffer_write_position,\
		s->stream->source_buffer_write_count,\
		(int)s->audio_avp->audio_engine->device_state_current,\
		s->audio_avp->audio_engine->device_buffer_write_position,\
		s->stream->stream_notification_request, \
		s->audio_avp->audio_engine->device_buffer_read_position);
#else
#define DUMP_AVP_STATUS(s)
#endif

/******************************************************************************
 * STRUCTURES
 *****************************************************************************/

static const struct tegra30_avp_ucode_desc {
	int max_mem_size;
	const char *bin_name;
} avp_ucode_desc[] = {
	[CODEC_PCM] = {25000, "nvavp_aud_ucode.bin" },
	[CODEC_MP3] = {45000, "nvavp_mp3dec_ucode.bin" },
	[CODEC_AAC] = {80000, "nvavp_aacdec_ucode.bin" },
};

struct tegra30_avp_audio_dma {
	struct tegra_offload_dma_params	params;
	struct dma_chan			*chan;
	struct dma_async_tx_descriptor	*chan_desc;
	struct dma_slave_config		chan_slave_config;
	dma_cookie_t			chan_cookie;

	atomic_t			is_dma_allocated;
	atomic_t			active_count;
};

struct tegra30_avp_stream {
	struct tegra30_avp_audio	*audio_avp;
	struct tegra_offload_mem	source_buf;
	struct stream_data		*stream;
	enum avp_audio_stream_id	id;
	int				period_size;

	/* TODO : Use spinlock in appropriate places */
	spinlock_t			lock;

	unsigned int			last_notification_offset;
	unsigned int			notification_received;
	unsigned int			source_buffer_offset;

	void		(*notify_cb)(void *args, unsigned int is_eos);
	void		*notify_args;
	unsigned int	is_drain_called;
};

struct tegra30_avp_audio {
	struct device			*dev;

	nvavp_clientctx_t		nvavp_client;
	struct audio_engine_data	*audio_engine;
	struct tegra30_avp_stream	avp_stream[RENDERSW_MAX_STREAMS];

	struct tegra_offload_mem	ucode_mem;
	struct tegra_offload_mem	cmd_buf_mem;
	struct tegra_offload_mem	param_mem;

	unsigned int			*cmd_buf;
	int				cmd_buf_idx;
	atomic_t		stream_active_count;
	struct tegra30_avp_audio_dma	audio_dma;
	spinlock_t			lock;
};

struct snd_compr_caps tegra30_avp_compr_caps[SND_COMPRESS_CAPTURE + 1] = {
	[SND_COMPRESS_PLAYBACK] = {
		.num_codecs = avp_compr_max,
		.direction = SND_COMPRESS_PLAYBACK,
		.min_fragment_size = 1024,
		.max_fragment_size = 1024 * 1024,	/* 1 MB */
		.min_fragments = 2,
		.max_fragments = 1024,
		.codecs = {
			[0] = SND_AUDIOCODEC_MP3,
			[1] = SND_AUDIOCODEC_AAC,
		},
	},
	[SND_COMPRESS_CAPTURE] = {
		.num_codecs = 0,
		.direction = SND_COMPRESS_CAPTURE,
	},
};

struct snd_compr_codec_caps tegra30_avp_compr_codec_caps[] = {
	[avp_compr_mp3] = {
		.codec = SND_AUDIOCODEC_MP3,
		.num_descriptors = 1,
		.descriptor = {
			[0] = {
				.max_ch = 2,
				.sample_rates = {
					[0] = SNDRV_PCM_RATE_44100 |
						SNDRV_PCM_RATE_48000,
				},
				.bit_rate = {
					[0] = 32000,
					[1] = 64000,
					[2] = 128000,
					[3] = 256000,
					[4] = 320000,
				},
				.num_bitrates = 5,
				.rate_control =
					SND_RATECONTROLMODE_CONSTANTBITRATE |
					SND_RATECONTROLMODE_VARIABLEBITRATE,
				.profiles = 0,
				.modes = SND_AUDIOCHANMODE_MP3_STEREO,
				.formats = SND_AUDIOSTREAMFORMAT_UNDEFINED,
				.min_buffer = 1024,
			},
		},
	},
	[avp_compr_aac] = {
		.codec = SND_AUDIOCODEC_AAC,
		.num_descriptors = 1,
		.descriptor = {
			[0] = {
				.max_ch = 2,
				.sample_rates = {
					[0] = SNDRV_PCM_RATE_44100 |
						SNDRV_PCM_RATE_48000,
				},
				.bit_rate = {
					[0] = 32000,
					[1] = 64000,
					[2] = 128000,
					[3] = 256000,
					[4] = 320000,
				},
				.num_bitrates = 5,
				.rate_control =
					SND_RATECONTROLMODE_CONSTANTBITRATE |
					SND_RATECONTROLMODE_VARIABLEBITRATE,
				.profiles = SND_AUDIOPROFILE_AAC,
				.modes = SND_AUDIOMODE_AAC_LC,
				.formats = SND_AUDIOSTREAMFORMAT_RAW,
				.min_buffer = 1024,
			},
		},
	},
};

static struct tegra30_avp_audio *avp_audio_ctx;

/******************************************************************************
 * PRIVATE FUNCTIONS
 *****************************************************************************/

static int tegra30_avp_mem_alloc(struct tegra_offload_mem *mem, size_t size)
{
	mem->virt_addr = dma_alloc_coherent(avp_audio_ctx->dev, size,
						&mem->phys_addr, GFP_KERNEL);
	if (!mem->virt_addr) {
		dev_err(avp_audio_ctx->dev, "Failed to allocate memory");
		return -ENOMEM;
	}
	mem->bytes = size;
	mem->dev = avp_audio_ctx->dev;
	memset(mem->virt_addr, 0, mem->bytes);

	dev_vdbg(mem->dev, "%s::virt %p phys %llx size %d\n", __func__,
		mem->virt_addr, (u64)mem->phys_addr, (int)size);
	return 0;
}

static void tegra30_avp_mem_free(struct tegra_offload_mem *mem)
{
	if (mem->virt_addr)
		dma_free_coherent(mem->dev, mem->bytes,
			mem->virt_addr, mem->phys_addr);
}

static int tegra30_avp_load_ucode(void)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct audio_engine_data *audio_engine;
	const struct firmware *ucode_fw;
	const struct tegra30_avp_ucode_desc *ucode_desc;
	ssize_t ucode_size = 0, ucode_offset = 0, total_ucode_size = 0;
	int i, ret = 0;

	dev_vdbg(audio_avp->dev, "%s", __func__);

	for (i = 0; i < ARRAY_SIZE(avp_ucode_desc); i++)
		total_ucode_size += avp_ucode_desc[i].max_mem_size;

	/* allocate memory for Ucode */
	ret = tegra30_avp_mem_alloc(&audio_avp->ucode_mem, total_ucode_size);
	if (ret < 0) {
		dev_err(audio_avp->dev, "Failed to allocate ucode memory");
		return ret;
	}

	/* allocate memory for parameters */
	ret = tegra30_avp_mem_alloc(&audio_avp->param_mem,
		sizeof(struct audio_engine_data));
	if (ret < 0) {
		dev_err(audio_avp->dev, "Failed to allocate param memory");
		goto err_ucode_mem_free;
	}
	audio_engine =
		(struct audio_engine_data *)audio_avp->param_mem.virt_addr;
	audio_avp->audio_engine = audio_engine;
	audio_engine->codec_ucode_base_address =
		audio_avp->ucode_mem.phys_addr;

	for (i = 0; i < ARRAY_SIZE(avp_ucode_desc); i++) {
		ucode_desc = &avp_ucode_desc[i];

		ret = request_firmware(&ucode_fw, ucode_desc->bin_name,
			audio_avp->dev);
		if (ret < 0) {
			dev_err(audio_avp->dev, "cannot read audio firmware %s",
				ucode_desc->bin_name);
			goto err_param_mem_free;
		}

		ucode_size = ucode_fw->size;
		if (ucode_size <= 0 ||
			ucode_size > avp_ucode_desc[i].max_mem_size) {
			dev_err(audio_avp->dev, "Invalid ucode size.");
			ret = -EINVAL;
			release_firmware(ucode_fw);
			goto err_param_mem_free;
		}
		dev_vdbg(audio_avp->dev, "%s ucode size = %zd bytes",
			ucode_desc->bin_name, ucode_size);

		/* Read ucode */
		if (strncmp((const char *)ucode_fw->data, AVP_UCODE_HEADER,
			AVP_UCODE_HEADER_SIZE)) {
			dev_err(audio_avp->dev, "ucode Header mismatch");
			ret = -EINVAL;
			release_firmware(ucode_fw);
			goto err_param_mem_free;
		}

		memcpy((char *)audio_avp->ucode_mem.virt_addr + ucode_offset,
			ucode_fw->data + AVP_UCODE_HEADER_SIZE,
			ucode_size - AVP_UCODE_HEADER_SIZE);

		audio_engine->codec_ucode_desc[i][UCODE_DESC_OFFSET] =
								ucode_offset;
		audio_engine->codec_ucode_desc[i][UCODE_DESC_SIZE] =
						ucode_desc->max_mem_size;
		ucode_offset += ucode_desc->max_mem_size;

		release_firmware(ucode_fw);
	}

	/* allocate memory for command buffer */
	ret = tegra30_avp_mem_alloc(&audio_avp->cmd_buf_mem,
			AVP_CMD_BUFFER_SIZE);
	if (ret < 0) {
		dev_err(audio_avp->dev, "Failed to allocate cmd buffer memory");
		goto err_param_mem_free;
	}
	audio_avp->cmd_buf = (unsigned int *)audio_avp->cmd_buf_mem.virt_addr;

	/* Set command */
	audio_avp->cmd_buf_idx = 0;
	audio_avp->cmd_buf[audio_avp->cmd_buf_idx++] =
		NVE26E_CH_OPCODE_INCR(NVE276_SET_MICROCODE_A, 3);
	audio_avp->cmd_buf[audio_avp->cmd_buf_idx++] = 0;
	audio_avp->cmd_buf[audio_avp->cmd_buf_idx++] =
		audio_avp->ucode_mem.phys_addr;
	audio_avp->cmd_buf[audio_avp->cmd_buf_idx++] =
		avp_ucode_desc[CODEC_PCM].max_mem_size - 8;
	audio_avp->cmd_buf[audio_avp->cmd_buf_idx++] =
		NVE26E_CH_OPCODE_INCR(NVE276_PARAMETER_METHOD(0), 1);
	audio_avp->cmd_buf[audio_avp->cmd_buf_idx++] =
		audio_avp->param_mem.phys_addr;

	ret = nvavp_pushbuffer_submit_audio(audio_avp->nvavp_client,
					    audio_avp->cmd_buf_mem.phys_addr,
					    audio_avp->cmd_buf_idx);
	if (ret < 0) {
		dev_err(audio_avp->dev, "pushbuffer_submit failed %d", ret);
		ret = -EINVAL;
		goto err_cmd_buf_mem_free;
	}
	dev_vdbg(audio_avp->dev, "Successfully loaded audio ucode");
	return 0;

err_cmd_buf_mem_free:
	tegra30_avp_mem_free(&audio_avp->cmd_buf_mem);
err_param_mem_free:
	audio_avp->audio_engine = NULL;
	tegra30_avp_mem_free(&audio_avp->param_mem);
err_ucode_mem_free:
	tegra30_avp_mem_free(&audio_avp->ucode_mem);
	return ret;
}

static void tegra30_avp_audio_engine_init(void)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct audio_engine_data *audio_engine = audio_avp->audio_engine;
	int i = 0;

	/* Initialize audio_engine_data */
#if defined(CONFIG_ARCH_TEGRA_14x_SOC) || defined(CONFIG_ARCH_TEGRA_12x_SOC)
	audio_engine->chip_id = NV_TEGRA_T148;
#else
	audio_engine->chip_id = NV_TEGRA_T114;
#endif
	audio_engine->device_state_target = KSSTATE_STOP;
	audio_engine->device_state_current = KSSTATE_STOP;

#ifdef DEBUG_AVP
	audio_engine->profile_state = PROFILE_RENDER_OVERALL_PROCESSING;
#endif
	audio_engine->device_format.rate = AVP_INIT_SAMPLE_RATE;
	audio_engine->device_format.bits_per_sample = 16;
	audio_engine->device_format.channels = 2;

	/* Initialize stream memory */
	for (i = 0; i < max_stream_id; i++) {
		struct tegra30_avp_stream *avp_stream;
		struct stream_data *stream;
		int j = 0;

		avp_stream = &audio_avp->avp_stream[i];
		memset(avp_stream, 0, sizeof(struct tegra30_avp_stream));

		avp_stream->id = i;

		stream = &audio_engine->stream[avp_stream->id];
		avp_stream->stream = stream;
		spin_lock_init(&avp_stream->lock);

		stream->stream_state_target = KSSTATE_STOP;
		stream->source_buffer_write_position = 0;
		stream->source_buffer_write_count = 0;
		stream->stream_params.rate = AVP_INIT_SAMPLE_RATE;

		for (j = 0; j < RENDERSW_MAX_CHANNELS; j++)
			stream->stream_volume[j] = AVP_UNITY_STREAM_VOLUME;

		stream->source_buffer_read_position = 0;
		stream->source_buffer_presentation_position = 0;
		stream->source_buffer_linear_position = 0;
		stream->source_buffer_presentation_position = 0;
		stream->source_buffer_frames_decoded = 0;
		stream->stream_state_current = KSSTATE_STOP;

		stream->stream_params.rate = AVP_INIT_SAMPLE_RATE;
		stream->stream_params.bits_per_sample = 16;
		stream->stream_params.channels = 2;

		avp_stream->audio_avp = audio_avp;
	}
}

static int tegra30_avp_audio_alloc_dma(struct tegra_offload_dma_params *params)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_audio_dma *dma = &audio_avp->audio_dma;
	struct audio_engine_data *audio_engine = audio_avp->audio_engine;
	dma_cap_mask_t mask;
	int ret = 0;

	dev_vdbg(audio_avp->dev, "%s: is_dma_allocated %d",
			__func__, atomic_read(&dma->is_dma_allocated));

	if (atomic_read(&dma->is_dma_allocated) == 1)
		return 0;

	memcpy(&dma->params, params, sizeof(struct tegra_offload_dma_params));

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);
	dma_cap_set(DMA_CYCLIC, mask);
	dma->chan = dma_request_channel(mask, NULL, NULL);
	if (dma->chan == NULL) {
		dev_err(audio_avp->dev, "Failed to allocate DMA chan.");
		return -ENOMEM;
	}

	/* Only playback is supported */
	dma->chan_slave_config.direction = DMA_MEM_TO_DEV;
	dma->chan_slave_config.dst_addr_width = dma->params.width;
	dma->chan_slave_config.dst_addr = dma->params.addr;
	dma->chan_slave_config.dst_maxburst = dma->params.max_burst;
	dma->chan_slave_config.slave_id = dma->params.req_sel;

	ret = dmaengine_slave_config(dma->chan, &dma->chan_slave_config);
	if (ret < 0) {
		dev_err(audio_avp->dev, "dma slave config failed.err %d.", ret);
		return ret;
	}
	audio_engine->apb_channel_handle = dma->chan->chan_id;
	atomic_set(&dma->is_dma_allocated, 1);

	return 0;
}

static void tegra30_avp_audio_free_dma(void)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_audio_dma *dma = &audio_avp->audio_dma;

	dev_vdbg(audio_avp->dev, "%s: is_dma_allocated %d",
			__func__, atomic_read(&dma->is_dma_allocated));

	if (atomic_read(&dma->is_dma_allocated) == 1) {
		dma_release_channel(dma->chan);
		atomic_set(&dma->is_dma_allocated, 0);
	}

	return;
}

static int tegra30_avp_audio_start_dma(void)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_audio_dma *dma = &audio_avp->audio_dma;
	struct audio_engine_data *audio_engine = audio_avp->audio_engine;

	dev_vdbg(audio_avp->dev, "%s: active %d", __func__,
			atomic_read(&dma->active_count));

	if (atomic_inc_return(&dma->active_count) > 1)
		return 0;

	dma->chan_desc = dmaengine_prep_dma_cyclic(dma->chan,
				(dma_addr_t)audio_engine->device_buffer_avp,
				DEVICE_BUFFER_SIZE,
				DEVICE_BUFFER_SIZE,
				dma->chan_slave_config.direction,
				DMA_CTRL_ACK);
	if (!dma->chan_desc) {
		dev_err(audio_avp->dev, "Failed to prep cyclic dma");
		return -ENODEV;
	}
	dma->chan_cookie = dmaengine_submit(dma->chan_desc);
	dma_async_issue_pending(dma->chan);
	return 0;
}

static int tegra30_avp_audio_stop_dma(void)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_audio_dma *dma = &audio_avp->audio_dma;

	dev_vdbg(audio_avp->dev, "%s: active %d.", __func__,
			atomic_read(&dma->active_count));

	if (atomic_dec_and_test(&dma->active_count))
		dmaengine_terminate_all(dma->chan);

	return 0;
}

static int tegra30_avp_audio_execute(void)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	uint32_t cmd_idx = audio_avp->cmd_buf_idx;
	int ret = 0;

	dev_vdbg(audio_avp->dev, "%s", __func__);

	/* Set command */
	audio_avp->cmd_buf[cmd_idx++] =
		NVE26E_CH_OPCODE_INCR(NVE276_EXECUTE, 1);
	audio_avp->cmd_buf[cmd_idx++] = 0; /* NVE276_EXECUTE_APPID */

	ret = nvavp_pushbuffer_submit_audio(audio_avp->nvavp_client,
		audio_avp->cmd_buf_mem.phys_addr,
		cmd_idx);
	if (ret < 0) {
		dev_err(audio_avp->dev, "nvavp_pushbuffer_submit_audio failed");
		return -EINVAL;
	}
	return 0;
}

static int tegra30_avp_audio_set_state(enum KSSTATE new_state)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct audio_engine_data *audio_engine = audio_avp->audio_engine;
	enum KSSTATE old_state;

	old_state = audio_engine->device_state_target;

	dev_vdbg(audio_avp->dev, "%s : state %d -> %d",
		__func__, old_state, new_state);

	if (old_state == new_state)
		return 0;

	audio_engine->device_state_target = new_state;
	/* TODO : Need a way to wait till AVP device state changes */
	if (new_state == KSSTATE_RUN)
		tegra30_avp_audio_execute();

	return 0;
}


/* Call this function with stream lock held */
static int tegra30_avp_stream_set_state(int id, enum KSSTATE new_state)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream = &audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;
	enum KSSTATE old_state = stream->stream_state_target;
	int ret = 0;

	dev_vdbg(audio_avp->dev, "%s : id %d state %d -> %d", __func__, id,
		 old_state, new_state);

	if (old_state == new_state)
		return 0;

	if (old_state == KSSTATE_STOP) {
		switch (stream->stream_format) {
		case FORMAT_MP3:
			nvavp_enable_audio_clocks(audio_avp->nvavp_client,
				NVAVP_MODULE_ID_VCP);
			break;
		case FORMAT_AAC:
			nvavp_enable_audio_clocks(audio_avp->nvavp_client,
				NVAVP_MODULE_ID_VCP);
			break;
		default:
			break;
		}
	}

	stream->stream_state_target = new_state;
	/* TODO : Need a way to wait till AVP stream state changes */

	if (new_state == KSSTATE_STOP) {
		switch (stream->stream_format) {
		case FORMAT_MP3:
			nvavp_disable_audio_clocks(audio_avp->nvavp_client,
				NVAVP_MODULE_ID_VCP);
			break;
		case FORMAT_AAC:
			nvavp_disable_audio_clocks(audio_avp->nvavp_client,
				NVAVP_MODULE_ID_VCP);
			break;
		default:
			break;
		}
	}

	if (new_state == KSSTATE_STOP) {
		stream->source_buffer_write_position = 0;
		stream->source_buffer_write_count = 0;
		avp_stream->last_notification_offset = 0;
		avp_stream->notification_received = 0;
		avp_stream->source_buffer_offset = 0;
	}

	if (new_state == KSSTATE_RUN)
		tegra30_avp_audio_start_dma();
	else if (old_state == KSSTATE_RUN)
		tegra30_avp_audio_stop_dma();

	return ret;
}

/* TODO : review ISR to make it more optimized if possible */
static void tegra30_avp_stream_notify(void)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream;
	struct stream_data *stream;
	int i;

	/* dev_vdbg(audio_avp->dev, "tegra30_avp_stream_notify"); */

	for (i = 0; i < max_stream_id; i++) {
		avp_stream = &audio_avp->avp_stream[i];
		stream = avp_stream->stream;

		if (!stream->stream_allocated)
			continue;

		if (avp_stream->is_drain_called &&
		   (stream->source_buffer_read_position ==
			stream->source_buffer_write_position) &&
		   (avp_stream->notification_received >=
			stream->stream_notification_request)) {
			/* End of stream occured and noitfy same with value 1 */
			avp_stream->notify_cb(avp_stream->notify_args, 1);
			tegra30_avp_stream_set_state(i, KSSTATE_STOP);
		} else if (stream->stream_notification_request >
			avp_stream->notification_received) {
			avp_stream->notification_received++;

			avp_stream->notify_cb(avp_stream->notify_args, 0);
		}
	}
}

/******************************************************************************
 * EXPORTED FUNCTIONS
 *****************************************************************************/
/* Device APIs */
static int tegra30_avp_set_hw_rate(int rate)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct audio_engine_data *audio_engine = audio_avp->audio_engine;

	dev_vdbg(audio_avp->dev, "%s rate %d", __func__, rate);

	if (!audio_engine) {
		dev_err(audio_avp->dev, "AVP platform not initialized.");
		return -ENODEV;
	}

	audio_engine->device_format.rate = rate;
	return 0;
}

static int tegra30_avp_alloc_shared_mem(struct tegra_offload_mem *mem,
					int bytes)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	int ret = 0;

	ret = tegra30_avp_mem_alloc(mem, bytes);
	if (ret < 0) {
		dev_err(audio_avp->dev, "%s: Failed. ret %d", __func__, ret);
		return ret;
	}
	return 0;
}

static void tegra30_avp_free_shared_mem(struct tegra_offload_mem *mem)
{
	tegra30_avp_mem_free(mem);
}

/* Loopback APIs */
static int tegra30_avp_loopback_set_params(int id,
		struct tegra_offload_pcm_params *params)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream = &audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;
	int ret = 0;

	dev_vdbg(audio_avp->dev, "%s:entry\n", __func__);

	/* TODO : check validity of parameters */
	if (!stream) {
		dev_err(audio_avp->dev, "AVP platform not initialized.");
		return -ENODEV;
	}


	stream->stream_notification_interval = params->period_size;
	stream->stream_notification_enable = 1;
	stream->stream_params.rate = params->rate;
	stream->stream_params.channels = params->channels;
	stream->stream_params.bits_per_sample = params->bits_per_sample;


	avp_stream->period_size = params->period_size;
	avp_stream->notify_cb = params->period_elapsed_cb;
	avp_stream->notify_args = params->period_elapsed_args;

	stream->source_buffer_system =
		(uintptr_t)(params->source_buf.virt_addr);
	stream->source_buffer_avp = params->source_buf.phys_addr;
	stream->source_buffer_size = params->buffer_size;
	return ret;
}

static int tegra30_avp_loopback_set_state(int id, int state)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream = &audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;

	dev_vdbg(audio_avp->dev, "%s : id %d state %d", __func__, id, state);

	if (!stream) {
		dev_err(audio_avp->dev, "AVP platform not initialized.");
		return -ENODEV;
	}

	switch (state) {
	case SNDRV_PCM_TRIGGER_START:
		stream->stream_state_target = KSSTATE_RUN;
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		stream->stream_state_target = KSSTATE_STOP;
		stream->source_buffer_write_position = 0;
		stream->source_buffer_write_count = 0;
		avp_stream->last_notification_offset = 0;
		avp_stream->notification_received = 0;
		avp_stream->source_buffer_offset = 0;
		return 0;
	default:
		dev_err(audio_avp->dev, "Unsupported state.");
		return -EINVAL;
	}
}

static size_t tegra30_avp_loopback_get_position(int id)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream = &audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;
	size_t pos = 0;

	pos = (size_t)stream->source_buffer_read_position;

	dev_vdbg(audio_avp->dev, "%s id %d pos %d", __func__, id, (u32)pos);

	return pos;
}

static void tegra30_avp_loopback_data_ready(int id, int bytes)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream = &audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;

	dev_vdbg(audio_avp->dev, "%s :id %d size %d", __func__, id, bytes);

	stream->source_buffer_write_position += bytes;
	stream->source_buffer_write_position %= stream->source_buffer_size;

	avp_stream->source_buffer_offset += bytes;
	while (avp_stream->source_buffer_offset >=
		stream->stream_notification_interval) {
		stream->source_buffer_write_count++;
		avp_stream->source_buffer_offset -=
		stream->stream_notification_interval;
	}
	return;
}

/* PCM APIs */
static int tegra30_avp_pcm_open(int *id, char *stream)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct audio_engine_data *audio_engine = audio_avp->audio_engine;
	struct tegra30_avp_stream *avp_stream;
	int ret = 0;

	dev_vdbg(audio_avp->dev, "%s", __func__);

	if (!audio_avp->nvavp_client) {
		ret = tegra_nvavp_audio_client_open(&audio_avp->nvavp_client);
		if (ret < 0) {
			dev_err(audio_avp->dev, "Failed to open nvavp.");
			return ret;
		}
	}
	if (!audio_engine) {
		ret = tegra30_avp_load_ucode();
		if (ret < 0) {
			dev_err(audio_avp->dev, "Failed to load ucode.");
			return ret;
		}
		tegra30_avp_audio_engine_init();
		nvavp_register_audio_cb(audio_avp->nvavp_client,
			tegra30_avp_stream_notify);
		audio_engine = audio_avp->audio_engine;
	}

	if (strcmp(stream, "pcm") == 0) {
		avp_stream = &audio_avp->avp_stream[1];
		if (!audio_engine->stream[pcm_stream_id].stream_allocated) {
			*id = pcm_stream_id;
			audio_engine->stream[*id].stream_allocated = 1;
			atomic_inc(&audio_avp->stream_active_count);
		} else if (
		!audio_engine->stream[pcm2_stream_id].stream_allocated) {
			*id = pcm2_stream_id;
			audio_engine->stream[*id].stream_allocated = 1;
			atomic_inc(&audio_avp->stream_active_count);
		} else {
			dev_err(audio_avp->dev, "All AVP PCM streams are busy");
			return -EBUSY;
		}
	} else if (strcmp(stream, "loopback") == 0) {
		avp_stream = &audio_avp->avp_stream[0];
		if
		(!audio_engine->stream[loopback_stream_id].stream_allocated) {
			dev_vdbg(audio_avp->dev,
			"Assigning loopback id:%d\n", loopback_stream_id);
			audio_engine->stream[*id].stream_allocated = 1;
			*id = loopback_stream_id;
		} else {
			dev_err(audio_avp->dev, "AVP loopback streams is busy");
			return -EBUSY;
		}
	}

	tegra30_avp_audio_set_state(KSSTATE_RUN);
	return 0;
}

static int tegra30_avp_pcm_set_params(int id,
		struct tegra_offload_pcm_params *params)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream =	&audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;
	int ret = 0;

	dev_vdbg(audio_avp->dev,
		"%s id %d rate %d chan %d bps %d period size %d buf size %d",
		 __func__, id, params->rate, params->channels,
		 params->bits_per_sample, params->period_size,
		 params->buffer_size);

	/* TODO : check validity of parameters */
	if (!stream) {
		dev_err(audio_avp->dev, "AVP platform not initialized.");
		return -ENODEV;
	}

	stream->stream_notification_interval = params->period_size;
	stream->stream_notification_enable = 1;
	stream->stream_params.rate = params->rate;
	stream->stream_params.channels = params->channels;
	stream->stream_params.bits_per_sample = params->bits_per_sample;
	avp_stream->period_size = params->period_size;

	avp_stream->notify_cb = params->period_elapsed_cb;
	avp_stream->notify_args = params->period_elapsed_args;

	stream->source_buffer_system =
			(uintptr_t) (params->source_buf.virt_addr);
	stream->source_buffer_avp = params->source_buf.phys_addr;
	stream->source_buffer_size = params->buffer_size;

	/* Set DMA params */
	ret = tegra30_avp_audio_alloc_dma(&params->dma_params);
	if (ret < 0) {
		dev_err(audio_avp->dev, "Failed to allocate DMA. ret %d", ret);
		return ret;
	}
	return 0;
}

static int tegra30_avp_pcm_set_state(int id, int state)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream =	&audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;

	dev_vdbg(audio_avp->dev, "%s : id %d state %d", __func__, id, state);

	if (!stream) {
		dev_err(audio_avp->dev, "AVP platform not initialized.");
		return -ENODEV;
	}

	switch (state) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		tegra30_avp_stream_set_state(id, KSSTATE_RUN);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		tegra30_avp_stream_set_state(id, KSSTATE_STOP);
		return 0;
	default:
		dev_err(audio_avp->dev, "Unsupported state.");
		return -EINVAL;
	}
}

static void tegra30_avp_pcm_data_ready(int id, int bytes)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream =	&audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;

	dev_vdbg(audio_avp->dev, "%s :id %d size %d", __func__, id, bytes);

	stream->source_buffer_write_position += bytes;
	stream->source_buffer_write_position %= stream->source_buffer_size;

	avp_stream->source_buffer_offset += bytes;
	while (avp_stream->source_buffer_offset >=
		stream->stream_notification_interval) {
		stream->source_buffer_write_count++;
		avp_stream->source_buffer_offset -=
			stream->stream_notification_interval;
	}
	return;
}

static size_t tegra30_avp_pcm_get_position(int id)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream =	&audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;
	size_t pos = 0;

	pos = (size_t)stream->source_buffer_read_position;

	dev_vdbg(audio_avp->dev, "%s id %d pos %d", __func__, id, (u32)pos);

	return pos;
}

/* Compress APIs */
static int tegra30_avp_compr_open(int *id)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct audio_engine_data *audio_engine = audio_avp->audio_engine;
	int ret = 0;

	dev_vdbg(audio_avp->dev, "%s", __func__);

	if (!audio_avp->nvavp_client) {
		ret = tegra_nvavp_audio_client_open(&audio_avp->nvavp_client);
		if (ret < 0) {
			dev_err(audio_avp->dev, "Failed to open nvavp.");
			return ret;
		}
	}
	if (!audio_engine) {
		ret = tegra30_avp_load_ucode();
		if (ret < 0) {
			dev_err(audio_avp->dev, "Failed to load ucode.");
			return ret;
		}
		tegra30_avp_audio_engine_init();
		nvavp_register_audio_cb(audio_avp->nvavp_client,
			tegra30_avp_stream_notify);
		audio_engine = audio_avp->audio_engine;
	}

	if (!audio_engine->stream[decode_stream_id].stream_allocated)
		*id = decode_stream_id;
	else if (!audio_engine->stream[decode2_stream_id].stream_allocated)
		*id = decode2_stream_id;
	else {
		dev_err(audio_avp->dev, "All AVP COMPR streams are busy");
		return -EBUSY;
	}
	audio_avp->avp_stream[*id].is_drain_called = 0;
	audio_engine->stream[*id].stream_allocated = 1;

	atomic_inc(&audio_avp->stream_active_count);
	tegra30_avp_audio_set_state(KSSTATE_RUN);

	return 0;
}

static int tegra30_avp_compr_set_params(int id,
		struct tegra_offload_compr_params *params)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream = &audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;
	int ret = 0;

	dev_vdbg(audio_avp->dev,
		"%s: id %d codec %d rate %d ch %d bps %d buf %d x %d",
		 __func__, id, params->codec_type, params->rate,
		 params->channels, params->bits_per_sample,
		 params->fragment_size, params->fragments);

	/* TODO : check validity of parameters */
	if (!stream) {
		dev_err(audio_avp->dev, "AVP platform not initialized.");
		return -ENODEV;
	}

	if (params->codec_type == SND_AUDIOCODEC_MP3) {
		stream->stream_format = FORMAT_MP3;
	} else if (params->codec_type == SND_AUDIOCODEC_AAC) {
		stream->stream_format = FORMAT_AAC;

		/* AAC-LC is only supported profile*/
		stream->u.aac.audio_profile = AAC_PROFILE_LC;
		stream->u.aac.sampling_freq = params->rate;
		stream->u.aac.payload_type  = AAC_PAYLOAD_RAW;
		switch (params->rate) {
		case 8000:
			stream->u.aac.sampling_freq_index = 0xb;
			break;
		case 11025:
			stream->u.aac.sampling_freq_index = 0xa;
			break;
		case 12000:
			stream->u.aac.sampling_freq_index = 0x9;
			break;
		case 16000:
			stream->u.aac.sampling_freq_index = 0x8;
			break;
		case 22050:
			stream->u.aac.sampling_freq_index = 0x7;
			break;
		case 24000:
			stream->u.aac.sampling_freq_index = 0x6;
			break;
		case 32000:
			stream->u.aac.sampling_freq_index = 0x5;
			break;
		case 44100:
			stream->u.aac.sampling_freq_index = 0x4;
			break;
		case 48000:
			stream->u.aac.sampling_freq_index = 0x3;
			break;
		case 64000:
			stream->u.aac.sampling_freq_index = 0x2;
			break;
		case 88200:
			stream->u.aac.sampling_freq_index = 0x1;
			break;
		case 96000:
			stream->u.aac.sampling_freq_index = 0x0;
			break;
		default:
			dev_err(audio_avp->dev, "Unsupported rate");
			return -EINVAL;
		}
		/* Only Stereo data is supported */
		stream->u.aac.channel_configuration = params->channels;
	} else {
		dev_err(audio_avp->dev, "Invalid stream/codec type.");
		return -EINVAL;
	}

	stream->stream_params.rate = params->rate;
	stream->stream_params.channels = params->channels;
	stream->stream_params.bits_per_sample = params->bits_per_sample;
	avp_stream->period_size = params->fragment_size;

	avp_stream->notify_cb = params->fragments_elapsed_cb;
	avp_stream->notify_args = params->fragments_elapsed_args;

	stream->source_buffer_size = (params->fragments *
			params->fragment_size);
	ret = tegra30_avp_mem_alloc(&avp_stream->source_buf,
			      stream->source_buffer_size);
	if (ret < 0) {
		dev_err(audio_avp->dev, "Failed to allocate source buf memory");
		return ret;
	}

	stream->source_buffer_system =
			(uintptr_t) avp_stream->source_buf.virt_addr;
	stream->source_buffer_avp = avp_stream->source_buf.phys_addr;

	if (stream->source_buffer_size > AVP_COMPR_THRESHOLD) {
		stream->stream_notification_interval =
			stream->source_buffer_size - AVP_COMPR_THRESHOLD;
	} else {
		stream->stream_notification_interval = avp_stream->period_size;
	}
	stream->stream_notification_enable = 1;

	/* Set DMA params */
	ret = tegra30_avp_audio_alloc_dma(&params->dma_params);
	if (ret < 0) {
		dev_err(audio_avp->dev, "Failed to allocate DMA. ret %d", ret);
		return ret;
	}

	if ((params->codec_type == SND_AUDIOCODEC_MP3) ||
	    (params->codec_type == SND_AUDIOCODEC_AAC)) {
		dev_info(audio_avp->dev, "\n*** STARTING %s Offload PLAYBACK ***\n",
		   (params->codec_type == SND_AUDIOCODEC_MP3) ? "MP3" : "AAC");
	}
	return 0;
}

static int tegra30_avp_compr_set_state(int id, int state)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream = &audio_avp->avp_stream[id];

	dev_vdbg(audio_avp->dev, "%s : id %d state %d",
		 __func__, id, state);

	switch (state) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		tegra30_avp_stream_set_state(id, KSSTATE_RUN);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		tegra30_avp_stream_set_state(id, KSSTATE_STOP);
		return 0;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		tegra30_avp_stream_set_state(id, KSSTATE_PAUSE);
		return 0;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		tegra30_avp_stream_set_state(id, KSSTATE_RUN);
		return 0;
	case SND_COMPR_TRIGGER_DRAIN:
	case SND_COMPR_TRIGGER_PARTIAL_DRAIN:
		avp_stream->is_drain_called = 1;
		return 0;
	default:
		dev_err(audio_avp->dev, "Unsupported state.");
		return -EINVAL;
	}
}

static void tegra30_avp_compr_data_ready(int id, int bytes)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream = &audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;

	dev_vdbg(audio_avp->dev, "%s : id %d size %d", __func__, id, bytes);

	stream->source_buffer_write_position += bytes;
	stream->source_buffer_write_position %= stream->source_buffer_size;

	avp_stream->source_buffer_offset += bytes;
	while (avp_stream->source_buffer_offset >=
		stream->stream_notification_interval) {
		stream->source_buffer_write_count++;
		avp_stream->source_buffer_offset -=
			stream->stream_notification_interval;
	}
	return;
}

static int tegra30_avp_compr_write(int id, char __user *buf, int bytes)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream = &audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;
	void *dst = (char *)(uintptr_t)stream->source_buffer_system +
		stream->source_buffer_write_position;
	int avail = 0;
	int write = 0;
	int ret = 0;

	avail = stream->source_buffer_read_position -
		stream->source_buffer_write_position;
	if ((avail < 0) || (!avail &&
		(stream->source_buffer_write_count ==
		stream->stream_notification_request)))
		avail += stream->source_buffer_size;

	dev_vdbg(audio_avp->dev, "%s : id %d size %d", __func__, id, bytes);

	/* TODO: check enough free space is available before writing */
	bytes = (bytes > avail) ? avail : bytes;
	if (!bytes) {
		dev_dbg(audio_avp->dev, "No free space in ring buffer.");
		DUMP_AVP_STATUS(avp_stream);
		return bytes;
	}

	write = stream->source_buffer_size -
		stream->source_buffer_write_position;
	if (write > bytes) {
		ret = copy_from_user(dst, buf, bytes);
		if (ret < 0) {
			dev_err(audio_avp->dev, "Failed to copy user data.");
			return -EFAULT;
		}
	} else {
		ret = copy_from_user(dst, buf, write);
		if (ret < 0) {
			dev_err(audio_avp->dev, "Failed to copy user data.");
			return -EFAULT;
		}

		ret = copy_from_user((void *)(uintptr_t)stream->source_buffer_system,
				buf + write, bytes - write);
		if (ret < 0) {
			dev_err(audio_avp->dev, "Failed to copy user data.");
			return -EFAULT;
		}
	}

	stream->source_buffer_write_position += bytes;
	stream->source_buffer_write_position %= stream->source_buffer_size;

	avp_stream->source_buffer_offset += bytes;
	while (avp_stream->source_buffer_offset >=
		stream->stream_notification_interval) {
		stream->source_buffer_write_count++;
		avp_stream->source_buffer_offset -=
			stream->stream_notification_interval;
	}
	DUMP_AVP_STATUS(avp_stream);
	return bytes;
}

static int tegra30_avp_compr_get_position(int id,
			struct snd_compr_tstamp *tstamp)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream = &audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;

	tstamp->byte_offset = stream->source_buffer_write_position;
	tstamp->copied_total = stream->source_buffer_write_position +
		(stream->source_buffer_write_count *
		 stream->stream_notification_interval);
	tstamp->pcm_frames = stream->source_buffer_presentation_position;
	tstamp->pcm_io_frames = stream->source_buffer_presentation_position;
	tstamp->sampling_rate = stream->stream_params.rate;

	dev_vdbg(audio_avp->dev, "%s id %d off %d copied %d pcm %d pcm io %d",
		 __func__, id, (int)tstamp->byte_offset,
		 (int)tstamp->copied_total, (int)tstamp->pcm_frames,
		 (int)tstamp->pcm_io_frames);

	return 0;
}

static int tegra30_avp_compr_get_caps(struct snd_compr_caps *caps)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;

	dev_vdbg(audio_avp->dev, "%s : dir %d", __func__, caps->direction);

	if (caps->direction == SND_COMPRESS_PLAYBACK)
		memcpy(caps, &tegra30_avp_compr_caps[SND_COMPRESS_PLAYBACK],
			sizeof(struct snd_compr_caps));
	else
		memcpy(caps, &tegra30_avp_compr_caps[SND_COMPRESS_CAPTURE],
			sizeof(struct snd_compr_caps));
	return 0;
}

static int tegra30_avp_compr_get_codec_caps(struct snd_compr_codec_caps *codec)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;

	dev_vdbg(audio_avp->dev, "%s : codec %d", __func__, codec->codec);

	switch (codec->codec) {
	case SND_AUDIOCODEC_MP3:
		memcpy(codec, &tegra30_avp_compr_codec_caps[avp_compr_mp3],
			sizeof(struct snd_compr_codec_caps));
		return 0;
	case SND_AUDIOCODEC_AAC:
		memcpy(codec, &tegra30_avp_compr_codec_caps[avp_compr_aac],
			sizeof(struct snd_compr_codec_caps));
		return 0;
	default:
		dev_err(audio_avp->dev, "Unsupported codec %d", codec->codec);
		return -EINVAL;
	}
}

static int tegra30_avp_compr_set_volume(int id, int left, int right)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream = &audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;

	dev_vdbg(audio_avp->dev, "%s id %d left vol %d right vol %d",
		 __func__, id, left, right);

	if (!stream) {
		dev_err(audio_avp->dev, "AVP platform not initialized.");
		return -ENODEV;
	}

	stream->stream_volume[0] = left;
	stream->stream_volume[1] = right;

	return 0;
}

/* Common APIs for pcm and compress */
static void tegra30_avp_stream_close(int id)
{
	struct tegra30_avp_audio *audio_avp = avp_audio_ctx;
	struct tegra30_avp_stream *avp_stream = &audio_avp->avp_stream[id];
	struct stream_data *stream = avp_stream->stream;

	dev_vdbg(audio_avp->dev, "%s id %d", __func__, id);

	if (!stream) {
		dev_err(audio_avp->dev, "AVP platform not initialized.");
		return;
	}
	tegra30_avp_mem_free(&avp_stream->source_buf);
	stream->stream_allocated = 0;
	tegra30_avp_stream_set_state(id, KSSTATE_STOP);

	if (id == loopback_stream_id)
		return;

	if (atomic_dec_and_test(&audio_avp->stream_active_count)) {
		tegra30_avp_audio_free_dma();
		tegra30_avp_audio_set_state(KSSTATE_STOP);
	}
}

static struct tegra_offload_ops avp_audio_platform = {
	.device_ops = {
		.set_hw_rate = tegra30_avp_set_hw_rate,
		.alloc_shared_mem = tegra30_avp_alloc_shared_mem,
		.free_shared_mem = tegra30_avp_free_shared_mem,
	},
	.pcm_ops = {
		.stream_open = tegra30_avp_pcm_open,
		.stream_close = tegra30_avp_stream_close,
		.set_stream_params = tegra30_avp_pcm_set_params,
		.set_stream_state = tegra30_avp_pcm_set_state,
		.get_stream_position = tegra30_avp_pcm_get_position,
		.data_ready = tegra30_avp_pcm_data_ready,
	},
	.loopback_ops = {
		.stream_open = tegra30_avp_pcm_open,
		.stream_close = tegra30_avp_stream_close,
		.set_stream_params = tegra30_avp_loopback_set_params,
		.set_stream_state = tegra30_avp_loopback_set_state,
		.get_stream_position = tegra30_avp_loopback_get_position,
		.data_ready = tegra30_avp_loopback_data_ready,
	},
	.compr_ops = {
		.stream_open = tegra30_avp_compr_open,
		.stream_close = tegra30_avp_stream_close,
		.set_stream_params = tegra30_avp_compr_set_params,
		.set_stream_state = tegra30_avp_compr_set_state,
		.get_stream_position = tegra30_avp_compr_get_position,
		.data_ready = tegra30_avp_compr_data_ready,
		.write = tegra30_avp_compr_write,
		.get_caps = tegra30_avp_compr_get_caps,
		.get_codec_caps = tegra30_avp_compr_get_codec_caps,
		.set_stream_volume = tegra30_avp_compr_set_volume,
	},
};

static u64 tegra_dma_mask = DMA_BIT_MASK(32);
static int tegra30_avp_audio_probe(struct platform_device *pdev)
{
	struct tegra30_avp_audio *audio_avp;

	pr_debug("tegra30_avp_audio_platform_probe platform probe started\n");

	audio_avp = devm_kzalloc(&pdev->dev, sizeof(*audio_avp), GFP_KERNEL);
	if (!audio_avp) {
		dev_err(&pdev->dev, "Can't allocate tegra30_avp_audio\n");
		return -ENOMEM;
	}
	dev_set_drvdata(&pdev->dev, audio_avp);
	audio_avp->dev = &pdev->dev;

	/* set the ops */
	if (tegra_register_offload_ops(&avp_audio_platform)) {
		dev_err(&pdev->dev, "Failed to register avp audio device.");
		return -EPROBE_DEFER;
	}

	spin_lock_init(&audio_avp->lock);
	pdev->dev.dma_mask = &tegra_dma_mask;
	pdev->dev.coherent_dma_mask = tegra_dma_mask;
	avp_audio_ctx = audio_avp;
	pr_info("tegra30_avp_audio_platform_probe successful.");
	return 0;
}

static int tegra30_avp_audio_remove(struct platform_device *pdev)
{
	struct tegra30_avp_audio *audio_avp = dev_get_drvdata(&pdev->dev);

	dev_vdbg(&pdev->dev, "%s", __func__);

	tegra_deregister_offload_ops();
	if (audio_avp->nvavp_client)
		tegra_nvavp_audio_client_release(audio_avp->nvavp_client);
	tegra30_avp_mem_free(&audio_avp->cmd_buf_mem);
	tegra30_avp_mem_free(&audio_avp->param_mem);
	tegra30_avp_mem_free(&audio_avp->ucode_mem);

	return 0;
}

static const struct of_device_id tegra30_avp_audio_of_match[] = {
	{ .compatible = "nvidia,tegra30-avp-audio", },
	{},
};

static struct platform_driver tegra30_avp_audio_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = tegra30_avp_audio_of_match,
	},
	.probe = tegra30_avp_audio_probe,
	.remove = tegra30_avp_audio_remove,
};
module_platform_driver(tegra30_avp_audio_driver);

MODULE_AUTHOR("Sumit Bhattacharya <sumitb@nvidia.com>");
MODULE_DESCRIPTION("Tegra30 AVP Audio driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
MODULE_DEVICE_TABLE(of, tegra30_avp_audio_of_match);
