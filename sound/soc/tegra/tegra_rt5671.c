/*
 * tegra_rt5671.c - Tegra machine ASoC driver for boards using ALC5671 codec.
 *
 * Copyright (c) 2013, NVIDIA CORPORATION. All rights reserved.
 * Copyright (C) 2016 XiaoMi, Inc.
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <asm/mach-types.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <mach/tegra_asoc_pdata.h>
#include <linux/platform_data/gpio-tegra.h>
#include <mach/tegra_rt5640_pdata.h>

#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "../codecs/rt5671.h"

#include "tegra_pcm.h"
#include "tegra_asoc_utils.h"
#include <linux/tfa9887.h>
#include "tegra30_ahub.h"
#include "tegra30_i2s.h"
#include "tegra30_dam.h"

#define DRV_NAME "tegra-snd-rt5671"

#define DAI_LINK_HIFI		0
#define DAI_LINK_LEFT_SPK	1
#define DAI_LINK_RIGHT_SPK	2
#define DAI_LINK_BTSCO		3
#define DAI_LINK_FM		4
#define DAI_LINK_FE_DEEP	5
#define DAI_LINK_FE_FAST	6
#define DAI_LINK_HIFI_BE	7
#define NUM_DAI_LINKS		8

/* Playback front ends, each feeding one DAM input */
#define FE_DEEP			0
#define FE_FAST			1
#define NUM_FE			2

/* The DAM output, and so I2S0 and AIF1 behind it */
#define DAM_OUT_RATE		48000

/* NVIDIA's machine drivers all program 0x1000 as unity; TRM omits CONV */
#define DAM_GAIN_UNITY		0x1000

/* DAM_CHx_CTRL DATA_SYNC: one bit per channel to wait for */
#define DAM_SYNC_NONE		0

const char *tegra_rt5671_i2s_dai_name[TEGRA30_NR_I2S_IFC] = {
	"tegra30-i2s.0",
	"tegra30-i2s.1",
	"tegra30-i2s.2",
	"tegra30-i2s.3",
	"tegra30-i2s.4",
};

#define GPIO_HP_MUTE    BIT(1)

struct tegra_rt5671 {
	struct tegra_asoc_utils_data util_data;
	struct tegra_asoc_platform_data *pdata;
	int gpio_requested;
	int clock_enabled;
	/* Streams holding the audio PLL rate, one bit per direction */
	unsigned int rate_locked;
	struct snd_soc_card *pcard;

	/* Playback mixer: the front ends go through a DAM into I2S */
	int dam_ifc;
	struct mutex dam_lock;		/* DAM setup against BE users */
	int dam_users;
	spinlock_t dam_trigger_lock;	/* DAM enables, from two streams */
	struct tegra30_i2s *be_i2s;
	enum tegra30_ahub_txcif fe_fifo_cif[NUM_FE];
	struct tegra_pcm_dma_params fe_dma_data[NUM_FE];
};

static const int tegra_rt5671_fe_dam_ch[NUM_FE] = {
	[FE_DEEP] = TEGRA30_DAM_CHIN0_SRC,
	[FE_FAST] = TEGRA30_DAM_CHIN1,
};

static int tegra_rt5671_set_clock(struct snd_soc_pcm_runtime *rtd,
				int sample_size, int channel, int srate)
{
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(card);
	int err, mclk, rate;
	unsigned int i2sclock;

	mclk = 256 * srate;
	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		/* Another stream holds the rate: go on only if its MCLK also
		 * divides down to ours. A set_mclk of 0 is no rate at all. */
		if (machine->util_data.set_mclk &&
		    !(machine->util_data.set_mclk % mclk)) {
			mclk = machine->util_data.set_mclk;
		} else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	rate = clk_get_rate(machine->util_data.clk_cdev1);
	err = snd_soc_dai_set_pll(codec_dai, 0, RT5671_PLL1_S_MCLK,
			rate, 512*srate);
	if (err < 0) {
		dev_err(card->dev, "codec_dai pll not set\n");
		return err;
	}
	err = snd_soc_dai_set_sysclk(codec_dai, RT5671_SCLK_S_PLL1,
			512*srate, SND_SOC_CLOCK_IN);
	if (err < 0) {
		dev_err(card->dev, "codec_dai clock not set\n");
		return err;
	}

	/*for 24 bit audio we support only S24_LE (S24_3LE is not supported)
	which is rendered on bus in 32 bits packet so consider as 32 bit
	depth in clock calculations, extra 4 is required by codec,
	God knows why ?*/
	if (sample_size == 24)
		i2sclock = srate * channel * 32 * 4;
	else
		i2sclock = 0;

	err = snd_soc_dai_set_sysclk(cpu_dai, 0,
			i2sclock, SND_SOC_CLOCK_OUT);
	if (err < 0) {
		dev_err(card->dev, "cpu_dai clock not set\n");
		return err;
	}

	return 0;
}

static int tegra_rt5671_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct tegra30_i2s *i2s = snd_soc_dai_get_drvdata(cpu_dai);

	/* I2S playback is fed by the DAM, so it plays only through the
	 * front ends; the plain link keeps capture */
	if (!rtd->dai_link->no_pcm &&
	    substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		dev_err(rtd->card->dev,
			"%s plays through the mixer front ends only\n",
			rtd->dai_link->name);
		return -EINVAL;
	}

	tegra_asoc_utils_tristate_dap(i2s->id, false);

	return 0;
}

static void tegra_rt5671_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct tegra30_i2s *i2s = snd_soc_dai_get_drvdata(cpu_dai);

	tegra_asoc_utils_tristate_dap(i2s->id, true);
}

/* Defined with the other link params below. The speaker link is codec to
 * codec, so it carries its rate in .params instead of taking it from a
 * stream; probe seeds it from platform data and hw_params() below keeps it
 * in step with AIF1. */
static struct snd_soc_pcm_stream tegra_rt5671_spk_params;

static int tegra_rt5671_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	int srate, i2s_daifmt, codec_daifmt;
	int err, sample_size;

	srate = params_rate(params);

	/* hw_params may come again without hw_free between; drop this
	 * stream's hold on the rate so it may change it, and take it back
	 * below once the stream is configured */
	if (machine->rate_locked & BIT(substream->stream)) {
		tegra_asoc_utils_lock_clk_rate(&machine->util_data, 0);
		machine->rate_locked &= ~BIT(substream->stream);
	}

	/* Keep the speaker link on the same rate as this one. sysclk is set
	 * below to PLL1 = 512 * srate, and the codec's get_clk_info() accepts
	 * only an exact sysclk/rate ratio, so AIF2 has no freedom here: any
	 * fixed value in tegra_rt5671_spk_params would be right for one srate
	 * and would either mis-clock the amplifiers or fail hw_params with
	 * -EINVAL for every other. */
	tegra_rt5671_spk_params.rate_min = srate;
	tegra_rt5671_spk_params.rate_max = srate;

	i2s_daifmt = SND_SOC_DAIFMT_NB_NF;
	i2s_daifmt |= pdata->i2s_param[HIFI_CODEC].is_i2s_master ?
			SND_SOC_DAIFMT_CBS_CFS : SND_SOC_DAIFMT_CBM_CFM;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S8:
		sample_size = 8;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
		sample_size = 16;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		sample_size = 24;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		sample_size = 32;
		break;
	default:
		return -EINVAL;
	}

	switch (pdata->i2s_param[HIFI_CODEC].i2s_mode) {
	case TEGRA_DAIFMT_I2S:
		i2s_daifmt |= SND_SOC_DAIFMT_I2S;
		break;
	case TEGRA_DAIFMT_DSP_A:
		i2s_daifmt |= SND_SOC_DAIFMT_DSP_A;
		break;
	case TEGRA_DAIFMT_DSP_B:
		i2s_daifmt |= SND_SOC_DAIFMT_DSP_B;
		break;
	case TEGRA_DAIFMT_LEFT_J:
		i2s_daifmt |= SND_SOC_DAIFMT_LEFT_J;
		break;
	case TEGRA_DAIFMT_RIGHT_J:
		i2s_daifmt |= SND_SOC_DAIFMT_RIGHT_J;
		break;
	default:
		dev_err(card->dev, "Can't configure i2s format\n");
		return -EINVAL;
	}

	err = tegra_rt5671_set_clock(rtd, sample_size, params_channels(params), srate);
	if (err < 0) {
		dev_err(card->dev, "Can't configure clocks\n");
		return err;
	}

	codec_daifmt = i2s_daifmt;

	/*invert the codec bclk polarity when codec is master
	in DSP mode this is done to match with the negative
	edge settings of tegra i2s*/
	if (((i2s_daifmt & SND_SOC_DAIFMT_FORMAT_MASK)
		== SND_SOC_DAIFMT_DSP_A) &&
		((i2s_daifmt & SND_SOC_DAIFMT_MASTER_MASK)
		== SND_SOC_DAIFMT_CBM_CFM)) {
		codec_daifmt &= ~(SND_SOC_DAIFMT_INV_MASK);
		codec_daifmt |= SND_SOC_DAIFMT_IB_NF;
	}

	err = snd_soc_dai_set_fmt(codec_dai, codec_daifmt);
	if (err < 0) {
		dev_err(card->dev, "codec_dai fmt not set\n");
		return err;
	}

	err = snd_soc_dai_set_fmt(cpu_dai, i2s_daifmt);
	if (err < 0) {
		dev_err(card->dev, "cpu_dai fmt not set\n");
		return err;
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);
	machine->rate_locked |= BIT(substream->stream);

	return 0;
}

static int tegra_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(rtd->card);

	if (machine->rate_locked & BIT(substream->stream)) {
		tegra_asoc_utils_lock_clk_rate(&machine->util_data, 0);
		machine->rate_locked &= ~BIT(substream->stream);
	}

	return 0;
}

static struct snd_soc_ops tegra_rt5671_ops = {
	.hw_params = tegra_rt5671_hw_params,
	.hw_free = tegra_hw_free,
	.startup = tegra_rt5671_startup,
	.shutdown = tegra_rt5671_shutdown,
};

/*
 * Playback mixer. Two front ends (deep buffer and fast) each own an APBIF
 * FIFO routed into one DAM input; the DAM mixes them in bypass at 48 kHz
 * and its output feeds the I2S of the back end, which carries AIF1.
 */

static int tegra_rt5671_dam_setup(struct tegra_rt5671 *machine)
{
	int ifc = machine->dam_ifc;
	int ret;

	tegra30_dam_enable_clock(ifc);

	ret = tegra30_dam_soft_reset(ifc);
	if (ret)
		goto err;

	ret = tegra30_dam_set_samplerate(ifc, TEGRA30_DAM_CHOUT, DAM_OUT_RATE);
	if (!ret)
		ret = tegra30_dam_set_samplerate(ifc, TEGRA30_DAM_CHIN0_SRC,
						 DAM_OUT_RATE);
	if (!ret)
		ret = tegra30_dam_set_samplerate(ifc, TEGRA30_DAM_CHIN1,
						 DAM_OUT_RATE);
	if (ret)
		goto err;

	tegra30_dam_set_gain(ifc, TEGRA30_DAM_CHIN0_SRC, DAM_GAIN_UNITY);
	tegra30_dam_set_gain(ifc, TEGRA30_DAM_CHIN1, DAM_GAIN_UNITY);

	ret = tegra30_dam_set_acif(ifc, TEGRA30_DAM_CHIN0_SRC, 2, 16, 2, 32);
	if (!ret)
		ret = tegra30_dam_set_acif(ifc, TEGRA30_DAM_CHIN1,
					   2, 16, 2, 32);
	if (!ret)
		ret = tegra30_dam_set_acif(ifc, TEGRA30_DAM_CHOUT,
					   2, 16, 2, 32);
	if (ret)
		goto err;

	/* CH0 starts in bypass; the deep buffer's hw_params may change it */
	tegra30_dam_enable_stereo_src(ifc, 0);
	ret = tegra30_dam_enable_stereo_mixing(ifc, 1);
	if (ret)
		goto err;

	tegra30_dam_ch0_set_datasync(ifc, DAM_SYNC_NONE);
	tegra30_dam_ch1_set_datasync(ifc, DAM_SYNC_NONE);

	return 0;

err:
	tegra30_dam_disable_clock(ifc);
	return ret;
}

static int tegra_rt5671_be_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra30_i2s *i2s = snd_soc_dai_get_drvdata(rtd->cpu_dai);
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(rtd->card);
	int ret = 0;

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return -EINVAL;

	mutex_lock(&machine->dam_lock);
	if (!machine->dam_users) {
		ret = tegra_rt5671_dam_setup(machine);
		if (ret) {
			dev_err(rtd->card->dev, "DAM setup failed: %d\n", ret);
			goto out;
		}
		tegra30_ahub_set_rx_cif_source(i2s->playback_i2s_cif,
				TEGRA30_AHUB_TXCIF_DAM0_TX0 + machine->dam_ifc);
	}
	machine->dam_users++;
out:
	mutex_unlock(&machine->dam_lock);
	if (ret)
		return ret;

	return tegra_rt5671_startup(substream);
}

static void tegra_rt5671_be_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra30_i2s *i2s = snd_soc_dai_get_drvdata(rtd->cpu_dai);
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(rtd->card);

	tegra_rt5671_shutdown(substream);

	mutex_lock(&machine->dam_lock);
	if (!--machine->dam_users) {
		tegra30_ahub_unset_rx_cif_source(i2s->playback_i2s_cif);
		tegra30_dam_enable_stereo_mixing(machine->dam_ifc, 0);
		tegra30_dam_disable_clock(machine->dam_ifc);
	}
	mutex_unlock(&machine->dam_lock);
}

/* The DAM mixes in bypass, so the I2S side runs at the DAM output format */
static int tegra_rt5671_be_fixup(struct snd_soc_pcm_runtime *rtd,
				 struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *format = hw_param_mask(params,
						SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = DAM_OUT_RATE;
	channels->min = channels->max = 2;
	snd_mask_none(format);
	snd_mask_set(format, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static int tegra_rt5671_be_init(struct snd_soc_pcm_runtime *rtd)
{
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(rtd->card);
	struct tegra30_i2s *i2s = snd_soc_dai_get_drvdata(rtd->cpu_dai);

	/* The DAM feeds this I2S; it must not take an APBIF FIFO itself */
	i2s->allocate_pb_fifo_cif = false;
	machine->be_i2s = i2s;

	return 0;
}

static int tegra_rt5671_fe_startup(struct snd_pcm_substream *substream,
				   int fe)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(rtd->card);
	struct tegra_pcm_dma_params *dma = &machine->fe_dma_data[fe];
	int ret;

	tegra30_ahub_enable_clocks();

	ret = tegra30_ahub_allocate_tx_fifo(&machine->fe_fifo_cif[fe],
					    &dma->addr, &dma->req_sel);
	if (ret) {
		dev_err(rtd->card->dev, "No APBIF FIFO for %s: %d\n",
			rtd->dai_link->name, ret);
		machine->fe_fifo_cif[fe] = -1;
		tegra30_ahub_disable_clocks();
		return ret;
	}
	dma->wrap = 4;
	dma->width = 32;
	rtd->cpu_dai->playback_dma_data = dma;

	tegra30_ahub_set_rx_cif_source(TEGRA30_AHUB_RXCIF_DAM0_RX0 +
			machine->dam_ifc * 2 + tegra_rt5671_fe_dam_ch[fe],
			machine->fe_fifo_cif[fe]);

	return 0;
}

static void tegra_rt5671_fe_shutdown(struct snd_pcm_substream *substream,
				     int fe)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(rtd->card);

	tegra30_ahub_unset_rx_cif_source(TEGRA30_AHUB_RXCIF_DAM0_RX0 +
			machine->dam_ifc * 2 + tegra_rt5671_fe_dam_ch[fe]);
	tegra30_ahub_free_tx_fifo(machine->fe_fifo_cif[fe]);
	machine->fe_fifo_cif[fe] = -1;
	rtd->cpu_dai->playback_dma_data = NULL;

	tegra30_ahub_disable_clocks();
}

/*
 * Run CH0 at the deep buffer's rate. Off the output rate it goes through the
 * converter, as stereo; stereo mixing needs bypass (TRM 20.10.4.1), so it is
 * on only at the output rate.
 */
static int tegra_rt5671_dam_ch0_rate(struct tegra_rt5671 *machine, int rate)
{
	int ifc = machine->dam_ifc;
	bool src = rate != DAM_OUT_RATE;
	int ret;

	mutex_lock(&machine->dam_lock);
	if (src)
		tegra30_dam_enable_stereo_mixing(ifc, 0);
	ret = tegra30_dam_set_samplerate(ifc, TEGRA30_DAM_CHIN0_SRC, rate);
	if (!ret) {
		tegra30_dam_enable_stereo_src(ifc, src);
		if (!src)
			ret = tegra30_dam_enable_stereo_mixing(ifc, 1);
	}
	mutex_unlock(&machine->dam_lock);

	return ret;
}

static int tegra_rt5671_fe_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params, int fe)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(rtd->card);
	enum tegra30_ahub_txcif cif = machine->fe_fifo_cif[fe];
	int ret;

	/* The front-end DAIs offer S16 only */
	if (params_format(params) != SNDRV_PCM_FORMAT_S16_LE)
		return -EINVAL;

	if (fe == FE_DEEP) {
		ret = tegra_rt5671_dam_ch0_rate(machine, params_rate(params));
		if (ret) {
			dev_err(rtd->card->dev, "DAM can't take %u Hz: %d\n",
				params_rate(params), ret);
			return ret;
		}
	}

	tegra30_ahub_set_tx_cif_channels(cif, params_channels(params),
					 params_channels(params));
	tegra30_ahub_set_tx_cif_bits(cif, TEGRA30_AUDIOCIF_BITS_16,
				     TEGRA30_AUDIOCIF_BITS_16);
	tegra30_ahub_set_tx_fifo_pack_mode(cif,
				TEGRA30_AHUB_CHANNEL_CTRL_TX_PACK_16);

	return 0;
}

/*
 * Called by the platform before the DMA starts and after it stops.
 *
 * Neither channel waits for the other (DATA_SYNC 0 on both, set up with the
 * DAM): the deep buffer and the fast stream are unrelated sounds with no
 * sample alignment to keep, and a wait would let a late period on one stall
 * the other.
 */
static int tegra_rt5671_fe_trigger(struct snd_pcm_substream *substream,
				   int cmd, int fe)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(rtd->card);
	int ifc = machine->dam_ifc;
	int ch = tegra_rt5671_fe_dam_ch[fe];
	unsigned long flags;

	spin_lock_irqsave(&machine->dam_trigger_lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		tegra30_dam_enable(ifc, TEGRA30_DAM_ENABLE, ch);
		tegra30_ahub_enable_tx_fifo(machine->fe_fifo_cif[fe]);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		tegra30_ahub_disable_tx_fifo(machine->fe_fifo_cif[fe]);
		tegra30_dam_enable(ifc, TEGRA30_DAM_DISABLE, ch);
		break;

	default:
		spin_unlock_irqrestore(&machine->dam_trigger_lock, flags);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&machine->dam_trigger_lock, flags);

	return 0;
}

#define TEGRA_RT5671_FE_OPS(name, fe)					\
static int tegra_rt5671_##name##_startup(struct snd_pcm_substream *s)	\
{									\
	return tegra_rt5671_fe_startup(s, fe);				\
}									\
static void tegra_rt5671_##name##_shutdown(struct snd_pcm_substream *s)	\
{									\
	tegra_rt5671_fe_shutdown(s, fe);				\
}									\
static int tegra_rt5671_##name##_hw_params(struct snd_pcm_substream *s,	\
					struct snd_pcm_hw_params *p)	\
{									\
	return tegra_rt5671_fe_hw_params(s, p, fe);			\
}									\
static int tegra_rt5671_##name##_trigger(struct snd_pcm_substream *s,	\
					 int cmd)			\
{									\
	return tegra_rt5671_fe_trigger(s, cmd, fe);			\
}									\
static struct snd_soc_ops tegra_rt5671_##name##_ops = {			\
	.startup = tegra_rt5671_##name##_startup,			\
	.shutdown = tegra_rt5671_##name##_shutdown,			\
	.hw_params = tegra_rt5671_##name##_hw_params,			\
	.trigger = tegra_rt5671_##name##_trigger,			\
}

TEGRA_RT5671_FE_OPS(fe_deep, FE_DEEP);
TEGRA_RT5671_FE_OPS(fe_fast, FE_FAST);

static struct snd_soc_ops tegra_rt5671_be_ops = {
	.hw_params = tegra_rt5671_hw_params,
	.hw_free = tegra_hw_free,
	.startup = tegra_rt5671_be_startup,
	.shutdown = tegra_rt5671_be_shutdown,
};

static int tegra_rt5671_event_hp(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;

	if (!(machine->gpio_requested & GPIO_HP_MUTE))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_hp_mute,
				SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static const struct snd_soc_dapm_widget ardbeg_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Int Left Spk", NULL),
	SND_SOC_DAPM_SPK("Int Right Spk", NULL),
	SND_SOC_DAPM_HP("Headphone Jack", tegra_rt5671_event_hp),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
	SND_SOC_DAPM_MIC("Int Mic", NULL),
	SND_SOC_DAPM_HP("BT Headphone", NULL),
	SND_SOC_DAPM_MIC("BT Mic", NULL),
	SND_SOC_DAPM_LINE("FM", NULL),
	SND_SOC_DAPM_MIXER("DAM Mixer", SND_SOC_NOPM, 0, 0, NULL, 0),
};

static const struct snd_soc_dapm_route ardbeg_audio_map[] = {
	{"Headphone Jack", NULL, "HPOR"},
	{"Headphone Jack", NULL, "HPOL"},
	{"IN1P", NULL, "Mic Jack"},
	{"IN1N", NULL, "Mic Jack"},
	{"micbias2", NULL, "Int Mic"},
	{"IN2P", NULL, "micbias2"},
	{"IN2N", NULL, "micbias2"},
	{"IN4P", NULL, "micbias2"},
	{"IN4N", NULL, "micbias2"},
	{"Int Left Spk", NULL, "Left Spk Playback"},
	{"Int Right Spk", NULL, "Right Spk Playback"},
	{"BT Headphone", NULL, "BT Playback"},
	{"BT Capture", NULL, "BT Mic"},
	{"FM Capture", NULL, "FM"},
	/* Playback front ends through the DAM into AIF1 */
	{"DAM Mixer", NULL, "FE0 Playback"},
	{"DAM Mixer", NULL, "FE1 Playback"},
	{"AIF1 Playback", NULL, "DAM Mixer"},
};

static const struct snd_kcontrol_new ardbeg_controls[] = {
	SOC_DAPM_PIN_SWITCH("Int Left Spk"),
	SOC_DAPM_PIN_SWITCH("Int Right Spk"),
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Mic Jack"),
	SOC_DAPM_PIN_SWITCH("Int Mic"),
	SOC_DAPM_PIN_SWITCH("BT Headphone"),
	SOC_DAPM_PIN_SWITCH("BT Mic"),
	SOC_DAPM_PIN_SWITCH("FM"),
};

static int tegra_rt5671_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	int ret;

	/*
	 * The headphone mute is the board's one audio GPIO. Speaker enable,
	 * microphone enables and the LDO and headphone-detect lines of
	 * NVIDIA's boards do not exist here: the amplifiers and codec are
	 * powered in hardware and jack detection lives in the codec driver.
	 */
	if (gpio_is_valid(pdata->gpio_hp_mute)) {
		ret = gpio_request_one(pdata->gpio_hp_mute,
				       GPIOF_OUT_INIT_LOW, "hp_mute");
		if (ret) {
			dev_err(card->dev, "cannot get hp_mute gpio: %d\n", ret);
			return ret;
		}
		machine->gpio_requested |= GPIO_HP_MUTE;
	}

	ret = tegra_asoc_utils_register_ctls(&machine->util_data);
	if (ret < 0)
		return ret;

	ret = tegra_rt5671_set_clock(rtd,
				pdata->i2s_param[HIFI_CODEC].sample_size,
				pdata->i2s_param[HIFI_CODEC].channels,
				pdata->i2s_param[HIFI_CODEC].rate);
	if (ret < 0)
		return ret;

	return 0;
}

/* No rate here on purpose: it comes from the same platform data that
 * configures AIF1, and is then tracked per stream. Declared above. */
static struct snd_soc_pcm_stream tegra_rt5671_spk_params = {
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.channels_min = 2,
	.channels_max = 2,

};

static const struct snd_soc_pcm_stream tegra_rt5671_bt_params = {
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rate_min = 8000,
	.rate_max = 8000,
	.channels_min = 1,
	.channels_max = 1,

};

/* Fixed, unlike the speaker link above: BCM4354 sends FM audio as 48 kHz
 * stereo and drives the AIF4 clock itself, so the rate is the chip's to
 * state rather than ours to follow. min == max, as for BT SCO. */
static const struct snd_soc_pcm_stream tegra_rt5671_fm_params = {
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,

};

static struct snd_soc_dai_link tegra_rt5671_dai[NUM_DAI_LINKS] = {
	[DAI_LINK_HIFI] = {
		.name = "rt5671",
		.stream_name = "rt5671 PCM",
		.codec_name = "rt5671.0-001c",
		.platform_name = "tegra30-i2s.0",
		.cpu_dai_name = "tegra30-i2s.0",
		.codec_dai_name = "rt5671-aif1",
		.ignore_pmdown_time = 1,
		.init = tegra_rt5671_init,
		.ops = &tegra_rt5671_ops,
	},
	[DAI_LINK_LEFT_SPK] = {
		.name = "rt5671 Left Speaker",
		.stream_name = "rt5671 Left SPK",
		.codec_name = "tfa98xx.0-0034",
		.cpu_dai_name = "rt5671-aif2",
		.codec_dai_name = "tfa98xx-dai",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
		.params = &tegra_rt5671_spk_params,
		.ignore_pmdown_time = 1,
	},
	[DAI_LINK_RIGHT_SPK] = {
		.name = "rt5671 Right Speaker",
		.stream_name = "rt5671 Right SPK",
		.codec_name = "tfa98xx.0-0037",
		.cpu_dai_name = "rt5671-aif2",
		.codec_dai_name = "tfa98xx-dai",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
		.params = &tegra_rt5671_spk_params,
		.ignore_pmdown_time = 1,
	},
	[DAI_LINK_BTSCO] = {
		.name = "BT-SCO",
		.stream_name = "BT SCO PCM",
		.codec_name = "rt5671.0-001c",
		.cpu_name = "spdif-dit.1",
		.codec_dai_name = "rt5671-aif3",
		.dai_fmt = SND_SOC_DAIFMT_DSP_A | SND_SOC_DAIFMT_IB_NF |
				SND_SOC_DAIFMT_CBM_CFM,
		.params = &tegra_rt5671_bt_params,
		.ignore_pmdown_time = 1,
	},

	[DAI_LINK_FM] = {
		.name = "rt5671 FM",
		.stream_name = "rt5671 FM",
		.codec_name = "rt5671.0-001c",
		.cpu_name = "spdif-dit.3",
		.codec_dai_name = "rt5671-aif4",
		/* BCM4354 drives the FM I2S clock as master; configure the
		 * codec side as bit-slave / frame-slave (CBS_CFS) so AIF4
		 * follows the chip's BCLK/LRCLK. With CBM_CFM both sides
		 * tried to drive the clock and the codec received garbage,
		 * which presented as constant hiss even though RDS came
		 * through fine (RDS lives in HCI events, not the audio I2S). */
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
		.params = &tegra_rt5671_fm_params,
		.ignore_pmdown_time = 1,
	},

	[DAI_LINK_FE_DEEP] = {
		.name = "rt5671 Deep Buffer",
		.stream_name = "rt5671 Deep Buffer",
		.codec_name = "snd-soc-dummy",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra-pcm-fe0",
		.codec_dai_name = "snd-soc-dummy-dai",
		.ops = &tegra_rt5671_fe_deep_ops,
		.dynamic = 1,
	},
	[DAI_LINK_FE_FAST] = {
		.name = "rt5671 Fast",
		.stream_name = "rt5671 Fast",
		.codec_name = "snd-soc-dummy",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra-pcm-fe1",
		.codec_dai_name = "snd-soc-dummy-dai",
		.ops = &tegra_rt5671_fe_fast_ops,
		.dynamic = 1,
	},
	/* AIF1 playback behind the DAM; cpu, codec and platform are set
	 * like the HIFI link's in probe */
	[DAI_LINK_HIFI_BE] = {
		.name = "rt5671 Mixer",
		.stream_name = "rt5671 Mixer",
		.codec_name = "rt5671.0-001c",
		.platform_name = "tegra30-i2s.0",
		.cpu_dai_name = "tegra30-i2s.0",
		.codec_dai_name = "rt5671-aif1",
		.init = tegra_rt5671_be_init,
		.ops = &tegra_rt5671_be_ops,
		.no_pcm = 1,
		.be_hw_params_fixup = tegra_rt5671_be_fixup,
		.ignore_pmdown_time = 1,
	},
};

static int tegra_rt5671_suspend_post(struct snd_soc_card *card)
{
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(card);
	int i, suspend_allowed = 1;

	/*In Voice Call we ignore suspend..so check for that*/
	for (i = 0; i < machine->pcard->num_links; i++) {
		if (machine->pcard->dai_link[i].ignore_suspend) {
			suspend_allowed = 0;
			break;
		}
	}

	if (suspend_allowed) {
		/*This may be required if dapm setbias level is not called in
		some cases, may be due to a wrong dapm map*/
		if (machine->clock_enabled) {
			machine->clock_enabled = 0;
			tegra_asoc_utils_clk_disable(&machine->util_data);
		}
		/*TODO: Disable Audio Regulators*/
	}

	return 0;
}

static int tegra_rt5671_resume_pre(struct snd_soc_card *card)
{
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(card);
	int i, suspend_allowed = 1;

	/*In Voice Call we ignore suspend..so check for that*/
	for (i = 0; i < machine->pcard->num_links; i++) {
		if (machine->pcard->dai_link[i].ignore_suspend) {
			suspend_allowed = 0;
			break;
		}
	}

	if (suspend_allowed) {
		/*This may be required if dapm setbias level is not called in
		some cases, may be due to a wrong dapm map*/
		if (!machine->clock_enabled && card->dapm.bias_level != SND_SOC_BIAS_STANDBY &&
				card->dapm.bias_level != SND_SOC_BIAS_OFF) {
			machine->clock_enabled = 1;
			tegra_asoc_utils_clk_enable(&machine->util_data);
		}
		/*TODO: Enable Audio Regulators*/
	}

	return 0;
}

static int tegra_rt5671_set_bias_level(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm, enum snd_soc_bias_level level)
{
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(card);

	if (dapm == &card->dapm) {
		if (level != SND_SOC_BIAS_STANDBY &&
			level != SND_SOC_BIAS_OFF && (!machine->clock_enabled)) {
			machine->clock_enabled = 1;
			tegra_asoc_utils_clk_enable(&machine->util_data);
		}
		dapm->bias_level = level;
	}

	return 0;
}

static int tegra_rt5671_set_bias_level_post(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm, enum snd_soc_bias_level level)
{
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(card);

	if (dapm == &card->dapm) {
		if ((level == SND_SOC_BIAS_STANDBY ||
			level == SND_SOC_BIAS_OFF) && machine->clock_enabled) {
			machine->clock_enabled = 0;
			tegra_asoc_utils_clk_disable(&machine->util_data);
		}
		dapm->bias_level = level;
	}

	return 0 ;
}

static struct snd_soc_codec_conf tegra_rt5671_conf[] = {
	{
		.dev_name = "tfa98xx.0-0034",
		.name_prefix = "Left Spk",
	},
	{
		.dev_name = "tfa98xx.0-0037",
		.name_prefix = "Right Spk",
	},
	{
		.dev_name = "spdif-dit.1",
		.name_prefix = "BT",
	},
	{
		.dev_name = "spdif-dit.3",
		.name_prefix = "FM",
	},
};

static struct snd_soc_card snd_soc_tegra_rt5671 = {
	.name = "tegra-rt5671",
	.owner = THIS_MODULE,
	.dai_link = tegra_rt5671_dai,
	.num_links = ARRAY_SIZE(tegra_rt5671_dai),
	.codec_conf = tegra_rt5671_conf,
	.num_configs = ARRAY_SIZE(tegra_rt5671_conf),
	.suspend_post = tegra_rt5671_suspend_post,
	.resume_pre = tegra_rt5671_resume_pre,
	.set_bias_level = tegra_rt5671_set_bias_level,
	.set_bias_level_post = tegra_rt5671_set_bias_level_post,
	.controls = ardbeg_controls,
	.num_controls = ARRAY_SIZE(ardbeg_controls),
	.dapm_widgets = ardbeg_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(ardbeg_dapm_widgets),
	.dapm_routes = ardbeg_audio_map,
	.num_dapm_routes = ARRAY_SIZE(ardbeg_audio_map),
	.fully_routed = true,
};

static void tegra_rt5671_free_dam(struct tegra_rt5671 *machine)
{
	tegra30_dam_free_channel(machine->dam_ifc, TEGRA30_DAM_CHIN0_SRC);
	tegra30_dam_free_channel(machine->dam_ifc, TEGRA30_DAM_CHIN1);
	tegra30_dam_free_controller(machine->dam_ifc);
}

static int tegra_rt5671_driver_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_soc_tegra_rt5671;
	struct device_node *np = pdev->dev.of_node;
	struct tegra_rt5671 *machine;
	struct tegra_asoc_platform_data *pdata = NULL;
	int ret;
	int codec_id;
	u32 val32[6];

	if (!pdev->dev.platform_data && !pdev->dev.of_node) {
		dev_err(&pdev->dev, "No platform data supplied\n");
		return -EINVAL;
	}
	if (pdev->dev.platform_data) {
		pdata = pdev->dev.platform_data;
	} else if (np) {
		pdata = kzalloc(sizeof(struct tegra_asoc_platform_data),
			GFP_KERNEL);
		if (!pdata) {
			dev_err(&pdev->dev, "Can't allocate tegra_asoc_platform_data struct\n");
			return -ENOMEM;
		}

		of_property_read_string(np, "nvidia,codec_name",
					&pdata->codec_name);

		of_property_read_string(np, "nvidia,codec_dai_name",
					&pdata->codec_dai_name);

		pdata->gpio_hp_mute = of_get_named_gpio(np,
						"nvidia,hp-mute-gpios", 0);
		if (pdata->gpio_hp_mute < 0)
			dev_warn(&pdev->dev, "Failed to get HP Mute GPIO\n");

		of_property_read_u32_array(np, "nvidia,i2s-param-hifi", val32,
							   ARRAY_SIZE(val32));
		pdata->i2s_param[HIFI_CODEC].audio_port_id = (int)val32[0];
		pdata->i2s_param[HIFI_CODEC].is_i2s_master = (int)val32[1];
		pdata->i2s_param[HIFI_CODEC].i2s_mode = (int)val32[2];
		pdata->i2s_param[HIFI_CODEC].sample_size = (int)val32[3];
		pdata->i2s_param[HIFI_CODEC].channels = (int)val32[4];
		pdata->i2s_param[HIFI_CODEC].rate = (int)val32[5];
	}

	if (!pdata) {
		dev_err(&pdev->dev, "No platform data supplied\n");
		return -EINVAL;
	}

	/* Seed the speaker link from the rate that configures AIF1, whether
	 * that came from a board file or from nvidia,i2s-param-hifi, so the
	 * two agree before the first stream opens. */
	tegra_rt5671_spk_params.rate_min = pdata->i2s_param[HIFI_CODEC].rate;
	tegra_rt5671_spk_params.rate_max = pdata->i2s_param[HIFI_CODEC].rate;

	if (pdata->codec_name) {
		tegra_rt5671_dai[DAI_LINK_HIFI].codec_name = pdata->codec_name;
		tegra_rt5671_dai[DAI_LINK_HIFI_BE].codec_name =
			pdata->codec_name;
	}

	if (pdata->codec_dai_name) {
		tegra_rt5671_dai[DAI_LINK_HIFI].codec_dai_name =
			pdata->codec_dai_name;
		tegra_rt5671_dai[DAI_LINK_HIFI_BE].codec_dai_name =
			pdata->codec_dai_name;
	}

	machine = kzalloc(sizeof(struct tegra_rt5671), GFP_KERNEL);
	if (!machine) {
		dev_err(&pdev->dev, "Can't allocate tegra_rt5671 struct\n");
		if (np)
			kfree(pdata);
		return -ENOMEM;
	}

	machine->pdata = pdata;
	machine->pcard = card;
	mutex_init(&machine->dam_lock);
	spin_lock_init(&machine->dam_trigger_lock);
	machine->fe_fifo_cif[FE_DEEP] = -1;
	machine->fe_fifo_cif[FE_FAST] = -1;

	machine->dam_ifc = tegra30_dam_allocate_controller();
	if (machine->dam_ifc < 0) {
		ret = machine->dam_ifc;
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "No DAM for the mixer: %d\n", ret);
		goto err_free_machine;
	}
	tegra30_dam_allocate_channel(machine->dam_ifc, TEGRA30_DAM_CHIN0_SRC);
	tegra30_dam_allocate_channel(machine->dam_ifc, TEGRA30_DAM_CHIN1);

	ret = tegra_asoc_utils_init(&machine->util_data, &pdev->dev, card);
	if (ret)
		goto err_free_dam;
	tegra_asoc_utils_clk_disable(&machine->util_data);

	/*
	 * No regulator here is the driver's to switch. The codec runs from
	 * AVDD_1V8_CDC (an LDO enabled from VDD_1V8_SMPS8) and AVDD_3V3_CDC
	 * (a load switch enabled from AVDD_1V8_CDC); the microphones are
	 * analogue, biased by the codec; the amplifiers' supplies are
	 * VDD_SYS and SMPS8. All of it comes up in hardware with the PMIC.
	 */

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, machine);

	codec_id = pdata->i2s_param[HIFI_CODEC].audio_port_id;
	tegra_rt5671_dai[DAI_LINK_HIFI].cpu_dai_name =
	tegra_rt5671_i2s_dai_name[codec_id];
	tegra_rt5671_dai[DAI_LINK_HIFI].platform_name =
	tegra_rt5671_i2s_dai_name[codec_id];
	tegra_rt5671_dai[DAI_LINK_HIFI_BE].cpu_dai_name =
	tegra_rt5671_i2s_dai_name[codec_id];
	tegra_rt5671_dai[DAI_LINK_HIFI_BE].platform_name =
	tegra_rt5671_i2s_dai_name[codec_id];

	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",
			ret);
		goto err_fini_utils;
	}

	if (!card->instantiated) {
		ret = -ENODEV;
		dev_err(&pdev->dev, "sound card not instantiated (%d)\n",
			ret);
		goto err_unregister_card;
	}

	ret = tegra_asoc_utils_set_parent(&machine->util_data,
				pdata->i2s_param[HIFI_CODEC].is_i2s_master);
	if (ret) {
		dev_err(&pdev->dev, "tegra_asoc_utils_set_parent failed (%d)\n",
			ret);
		goto err_unregister_card;
	}

	return 0;

err_unregister_card:
	snd_soc_unregister_card(card);
err_fini_utils:
	/* requested by the card's init, which may have run before a failure */
	if (machine->gpio_requested & GPIO_HP_MUTE)
		gpio_free(pdata->gpio_hp_mute);
	if (machine->be_i2s)
		machine->be_i2s->allocate_pb_fifo_cif = true;
	tegra_asoc_utils_fini(&machine->util_data);
err_free_dam:
	tegra_rt5671_free_dam(machine);
err_free_machine:
	if (np)
		kfree(machine->pdata);

	kfree(machine);

	return ret;
}

static int tegra_rt5671_driver_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	struct device_node *np = pdev->dev.of_node;

	snd_soc_unregister_card(card);

	if (machine->gpio_requested & GPIO_HP_MUTE)
		gpio_free(pdata->gpio_hp_mute);

	if (machine->be_i2s)
		machine->be_i2s->allocate_pb_fifo_cif = true;

	tegra_asoc_utils_fini(&machine->util_data);
	tegra_rt5671_free_dam(machine);

	if (np)
		kfree(machine->pdata);

	kfree(machine);

	return 0;
}

static const struct of_device_id tegra_rt5671_of_match[] = {
	{ .compatible = "nvidia,tegra-audio-rt5671", },
	{},
};

static struct platform_driver tegra_rt5671_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = tegra_rt5671_of_match,
	},
	.probe = tegra_rt5671_driver_probe,
	.remove = tegra_rt5671_driver_remove,
};

static int __init tegra_rt5671_modinit(void)
{
	return platform_driver_register(&tegra_rt5671_driver);
}
module_init(tegra_rt5671_modinit);

static void __exit tegra_rt5671_modexit(void)
{
	platform_driver_unregister(&tegra_rt5671_driver);
}
module_exit(tegra_rt5671_modexit);

MODULE_AUTHOR("Nikesh Oswal <noswal@nvidia.com>");
MODULE_DESCRIPTION("Tegra+rt5671 machine ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
