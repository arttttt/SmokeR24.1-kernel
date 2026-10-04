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
#define DAI_LINK_FE_HIFI	7
#define DAI_LINK_HIFI_BE	8
#define NUM_DAI_LINKS		9

/* Playback front ends */
#define FE_DEEP			0
#define FE_FAST			1
#define FE_HIFI			2
#define NUM_FE			3

/*
 * The three DAMs, by role. Every DAM runs in a mode TRM 20.6 allows: stereo
 * mixing only in bypass, stereo SRC only with channel 1 idle.
 *
 *   deep ─► MIX CH0 ┐ 48 kHz, bypass,
 *   fast ─► MIX CH1 ┘ stereo mixing  ─► SRC CH0: 48 kHz -> back-end rate ─┐
 *                                                                        ├► OUT ─► I2S0
 *   hifi ──────────────────────────────────────────────────► OUT CH0 ────┘   (OUT CH1 <- SRC)
 *
 * OUT mixes in bypass at the back-end rate. The hifi stream is on its CH0
 * and reaches the I2S untouched while nothing else plays.
 */
#define DAM_MIX			0
#define DAM_SRC			1
#define DAM_OUT			2
#define NUM_DAM			3

/* The rate deep and fast are mixed at, before SRC to the back-end rate */
#define DAM_CHAIN_RATE		48000

/* NVIDIA's machine drivers all program 0x1000 as unity; TRM omits CONV */
#define DAM_GAIN_UNITY		0x1000

/* DAM_CHx_CTRL DATA_SYNC: one bit per channel to wait for */
#define DAM_SYNC_NONE		0
#define DAM_SYNC_WAIT_CH0	BIT(0)

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
	int aif1_rate;			/* the rate they hold */
	struct snd_soc_card *pcard;

	/* Playback mixer: the front ends go through the DAMs into I2S */
	int dam[NUM_DAM];		/* DAM controller per role */
	struct mutex dam_lock;		/* DAM setup against BE users */
	int dam_users;
	int be_rate;			/* under dam_lock */
	spinlock_t dam_trigger_lock;	/* DAM enables, from three streams */
	bool fe_running[NUM_FE];	/* under dam_trigger_lock */
	int chain_users;		/* deep/fast running; dam_trigger_lock */
	struct tegra30_i2s *be_i2s;
	enum tegra30_ahub_txcif fe_fifo_cif[NUM_FE];
	struct tegra_pcm_dma_params fe_dma_data[NUM_FE];
};

/* Which DAM input each front end feeds */
static const struct {
	int dam;
	int ch;
} tegra_rt5671_fe_input[NUM_FE] = {
	[FE_DEEP] = { DAM_MIX, TEGRA30_DAM_CHIN0_SRC },
	[FE_FAST] = { DAM_MIX, TEGRA30_DAM_CHIN1 },
	[FE_HIFI] = { DAM_OUT, TEGRA30_DAM_CHIN0_SRC },
};

static inline enum tegra30_ahub_rxcif tegra_rt5671_dam_rx(int ifc, int ch)
{
	return TEGRA30_AHUB_RXCIF_DAM0_RX0 + ifc * 2 + ch;
}

static inline enum tegra30_ahub_txcif tegra_rt5671_dam_tx(int ifc)
{
	return TEGRA30_AHUB_TXCIF_DAM0_TX0 + ifc;
}

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

	/*
	 * I2S0 and AIF1 each keep one word length for both directions (the
	 * Tegra I2S BIT_SIZE, the codec's I2S1 data length), so capture must
	 * run in the playback back end's S24, or opening one would re-clock
	 * the other.
	 */
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE &&
	    !rtd->dai_link->no_pcm) {
		int ret = snd_pcm_hw_constraint_mask64(substream->runtime,
				SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FMTBIT_S24_LE);

		if (ret < 0)
			return ret;
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

/*
 * AIF1's format, the I2S role included, as the board describes it
 * (nvidia,i2s-param-hifi). The role is the wiring's -- who drives BCLK and
 * LRCK on the bus -- not a stream's, so it goes into the AIF1 links'
 * dai_fmt, which ASoC sets on the codec and the I2S controller once, when
 * the card comes up. Paths inside the codec need it with no stream at all:
 * FM on the speakers plays only while the codec's I2S1 is master.
 *
 * The codec's BCLK used to be inverted in DSP A mode with the codec master,
 * to meet the Tegra I2S on the other edge; one dai_fmt for both ends can't
 * say that, and no board this driver runs on uses DSP A.
 */
static int tegra_rt5671_aif1_fmt(struct tegra_asoc_platform_data *pdata,
				 unsigned int *fmt)
{
	const bool codec_master = !pdata->i2s_param[HIFI_CODEC].is_i2s_master;

	*fmt = SND_SOC_DAIFMT_NB_NF;
	*fmt |= codec_master ? SND_SOC_DAIFMT_CBM_CFM : SND_SOC_DAIFMT_CBS_CFS;

	switch (pdata->i2s_param[HIFI_CODEC].i2s_mode) {
	case TEGRA_DAIFMT_I2S:
		*fmt |= SND_SOC_DAIFMT_I2S;
		break;
	case TEGRA_DAIFMT_DSP_A:
		if (codec_master)
			return -EINVAL;
		*fmt |= SND_SOC_DAIFMT_DSP_A;
		break;
	case TEGRA_DAIFMT_DSP_B:
		*fmt |= SND_SOC_DAIFMT_DSP_B;
		break;
	case TEGRA_DAIFMT_LEFT_J:
		*fmt |= SND_SOC_DAIFMT_LEFT_J;
		break;
	case TEGRA_DAIFMT_RIGHT_J:
		*fmt |= SND_SOC_DAIFMT_RIGHT_J;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/* Defined with the other link params below. The speaker link is codec to
 * codec, so it carries its rate in .params instead of taking it from a
 * stream; probe seeds it from platform data and hw_params() below sets it
 * from the AIF1 rate. */
static struct snd_soc_pcm_stream tegra_rt5671_spk_params;

static int tegra_rt5671_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(card);
	int srate;
	int err, sample_size;
	unsigned int spk_rate, da_clk, ad_clk;

	srate = params_rate(params);

	/* hw_params may come again without hw_free between; drop this
	 * stream's hold on the rate so it may change it, and take it back
	 * below once the stream is configured */
	if (machine->rate_locked & BIT(substream->stream)) {
		tegra_asoc_utils_lock_clk_rate(&machine->util_data, 0);
		machine->rate_locked &= ~BIT(substream->stream);
	}

	/*
	 * I2S0 and AIF1 run both directions from one clock and one codec
	 * sysclk (PLL1 = 512 * srate, set below). While the other direction
	 * holds them, this one has to take their rate: a different one could
	 * still pass the audio PLL check when its MCLK divides the held one
	 * (48 kHz capture under 96 kHz playback), and setting PLL1 for it
	 * would re-clock the stream already running.
	 */
	if ((machine->rate_locked & BIT(!substream->stream)) &&
	    srate != machine->aif1_rate) {
		dev_err(card->dev, "%s: AIF1 runs at %d Hz, can't take %d Hz\n",
			rtd->dai_link->name, machine->aif1_rate, srate);
		return -EBUSY;
	}

	/*
	 * The speaker link follows this rate up to 48 kHz, the most the
	 * TFA9890s take. Above it AIF2 runs at 44.1 or 48 kHz from the same
	 * sysclk (PLL1 = 512 * srate, set below): the codec's get_clk_info()
	 * takes sysclk / (256 * rate) as a pre-divider, and 4 is one of
	 * them. The mono DAC filters then track I2S1 and the mono ADC
	 * filters I2S2 through the codec's ASRC, which carries the speaker
	 * mix from the AIF1 rate to the AIF2 one.
	 */
	if (srate > 48000) {
		spk_rate = srate % 11025 ? 48000 : 44100;
		da_clk = RT5671_CLK_SEL_I2S1_ASRC;
		ad_clk = RT5671_CLK_SEL_I2S2_ASRC;
	} else {
		spk_rate = srate;
		da_clk = RT5671_CLK_SEL_SYS;
		ad_clk = RT5671_CLK_SEL_SYS;
	}
	tegra_rt5671_spk_params.rate_min = spk_rate;
	tegra_rt5671_spk_params.rate_max = spk_rate;
	rt5671_sel_asrc_clk_src(codec,
			RT5671_DA_MONO_L_FILTER | RT5671_DA_MONO_R_FILTER,
			da_clk);
	rt5671_sel_asrc_clk_src(codec,
			RT5671_AD_MONO_L_FILTER | RT5671_AD_MONO_R_FILTER,
			ad_clk);

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

	err = tegra_rt5671_set_clock(rtd, sample_size, params_channels(params), srate);
	if (err < 0) {
		dev_err(card->dev, "Can't configure clocks\n");
		return err;
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);
	machine->rate_locked |= BIT(substream->stream);
	machine->aif1_rate = srate;

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
 * Playback mixer: three front ends and three DAMs, see the diagram at the
 * top. The back end (I2S0 + AIF1) runs at 48 kHz, or at the hifi stream's
 * rate when that stream is the first to open it.
 */

static int tegra_rt5671_dam_init_one(int ifc, int ch0_bits, int ch1_bits,
				     bool mixing)
{
	int ret;

	tegra30_dam_enable_clock(ifc);

	ret = tegra30_dam_soft_reset(ifc);
	if (!ret)
		ret = tegra30_dam_set_samplerate(ifc, TEGRA30_DAM_CHOUT,
						 DAM_CHAIN_RATE);
	if (!ret)
		ret = tegra30_dam_set_samplerate(ifc, TEGRA30_DAM_CHIN0_SRC,
						 DAM_CHAIN_RATE);
	if (!ret)
		ret = tegra30_dam_set_samplerate(ifc, TEGRA30_DAM_CHIN1,
						 DAM_CHAIN_RATE);
	if (ret)
		goto err;

	tegra30_dam_set_gain(ifc, TEGRA30_DAM_CHIN0_SRC, DAM_GAIN_UNITY);
	tegra30_dam_set_gain(ifc, TEGRA30_DAM_CHIN1, DAM_GAIN_UNITY);

	/* Every DAM-to-DAM and DAM-to-I2S hop carries 32-bit words, the
	 * samples at the top; front-end inputs are set by their hw_params */
	ret = tegra30_dam_set_acif(ifc, TEGRA30_DAM_CHIN0_SRC,
				   2, ch0_bits, 2, 32);
	if (!ret)
		ret = tegra30_dam_set_acif(ifc, TEGRA30_DAM_CHIN1,
					   2, ch1_bits, 2, 32);
	if (!ret)
		ret = tegra30_dam_set_acif(ifc, TEGRA30_DAM_CHOUT,
					   2, 32, 2, 32);
	if (ret)
		goto err;

	tegra30_dam_enable_stereo_src(ifc, 0);
	ret = tegra30_dam_enable_stereo_mixing(ifc, mixing);
	if (ret)
		goto err;

	tegra30_dam_ch0_set_datasync(ifc, DAM_SYNC_NONE);
	tegra30_dam_ch1_set_datasync(ifc, DAM_SYNC_NONE);

	return 0;

err:
	tegra30_dam_disable_clock(ifc);
	return ret;
}

static void tegra_rt5671_dam_fini_one(int ifc)
{
	tegra30_dam_enable_stereo_mixing(ifc, 0);
	tegra30_dam_enable_stereo_src(ifc, 0);
	tegra30_dam_disable_clock(ifc);
}

static int tegra_rt5671_dam_setup(struct tegra_rt5671 *machine,
				  struct tegra30_i2s *i2s)
{
	int *dam = machine->dam;
	int ret;

	ret = tegra_rt5671_dam_init_one(dam[DAM_MIX], 16, 16, true);
	if (ret)
		return ret;
	ret = tegra_rt5671_dam_init_one(dam[DAM_SRC], 32, 32, true);
	if (ret)
		goto err_mix;
	ret = tegra_rt5671_dam_init_one(dam[DAM_OUT], 16, 32, true);
	if (ret)
		goto err_src;

	tegra30_ahub_set_rx_cif_source(
			tegra_rt5671_dam_rx(dam[DAM_SRC], TEGRA30_DAM_CHIN0_SRC),
			tegra_rt5671_dam_tx(dam[DAM_MIX]));
	tegra30_ahub_set_rx_cif_source(
			tegra_rt5671_dam_rx(dam[DAM_OUT], TEGRA30_DAM_CHIN1),
			tegra_rt5671_dam_tx(dam[DAM_SRC]));
	tegra30_ahub_set_rx_cif_source(i2s->playback_i2s_cif,
			tegra_rt5671_dam_tx(dam[DAM_OUT]));

	machine->be_rate = DAM_CHAIN_RATE;
	return 0;

err_src:
	tegra_rt5671_dam_fini_one(dam[DAM_SRC]);
err_mix:
	tegra_rt5671_dam_fini_one(dam[DAM_MIX]);
	return ret;
}

static void tegra_rt5671_dam_teardown(struct tegra_rt5671 *machine,
				      struct tegra30_i2s *i2s)
{
	int *dam = machine->dam;

	tegra30_ahub_unset_rx_cif_source(i2s->playback_i2s_cif);
	tegra30_ahub_unset_rx_cif_source(
			tegra_rt5671_dam_rx(dam[DAM_OUT], TEGRA30_DAM_CHIN1));
	tegra30_ahub_unset_rx_cif_source(
			tegra_rt5671_dam_rx(dam[DAM_SRC], TEGRA30_DAM_CHIN0_SRC));

	tegra_rt5671_dam_fini_one(dam[DAM_OUT]);
	tegra_rt5671_dam_fini_one(dam[DAM_SRC]);
	tegra_rt5671_dam_fini_one(dam[DAM_MIX]);
}

/*
 * Put the chain's tail at the back-end rate. SRC converts 48 kHz to it, as
 * stereo with its CH1 idle, or passes 48 kHz through; OUT mixes in bypass at
 * it. Only called with no front end running (the back end's hw_params).
 *
 * A DAM's CH0 is stereo only with STEREO_SRC_EN (converting) or
 * STEREO_MIXING_EN (bypass) set; with neither it takes each stereo frame as
 * two mono samples and runs at half speed. So SRC, idle CH1 or not, keeps
 * stereo mixing on whenever it passes 48 kHz through.
 */
static int tegra_rt5671_dam_rate(struct tegra_rt5671 *machine, int rate)
{
	int src = machine->dam[DAM_SRC], out = machine->dam[DAM_OUT];
	bool convert = rate != DAM_CHAIN_RATE;
	int ret;

	tegra30_dam_enable_stereo_mixing(out, 0);
	tegra30_dam_enable_stereo_mixing(src, 0);

	ret = tegra30_dam_set_samplerate(src, TEGRA30_DAM_CHOUT, rate);
	if (!ret)
		ret = tegra30_dam_set_samplerate(src, TEGRA30_DAM_CHIN0_SRC,
						 DAM_CHAIN_RATE);
	if (ret)
		return ret;
	tegra30_dam_enable_stereo_src(src, convert);
	if (!convert) {
		ret = tegra30_dam_enable_stereo_mixing(src, 1);
		if (ret)
			return ret;
	}

	ret = tegra30_dam_set_samplerate(out, TEGRA30_DAM_CHOUT, rate);
	if (!ret)
		ret = tegra30_dam_set_samplerate(out, TEGRA30_DAM_CHIN0_SRC,
						 rate);
	if (!ret)
		ret = tegra30_dam_set_samplerate(out, TEGRA30_DAM_CHIN1, rate);
	if (!ret)
		ret = tegra30_dam_enable_stereo_mixing(out, 1);

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
		ret = tegra_rt5671_dam_setup(machine, i2s);
		if (ret) {
			dev_err(rtd->card->dev, "DAM setup failed: %d\n", ret);
			goto out;
		}
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
	if (!--machine->dam_users)
		tegra_rt5671_dam_teardown(machine, i2s);
	mutex_unlock(&machine->dam_lock);
}

/* The back end runs at the rate of the front end that opened it first */
static int tegra_rt5671_be_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(rtd->card);
	int rate = params_rate(params);
	int ret;

	mutex_lock(&machine->dam_lock);
	ret = tegra_rt5671_dam_rate(machine, rate);
	if (!ret)
		machine->be_rate = rate;
	mutex_unlock(&machine->dam_lock);
	if (ret) {
		dev_err(rtd->card->dev, "DAMs can't run at %d Hz: %d\n",
			rate, ret);
		return ret;
	}

	return tegra_rt5671_hw_params(substream, params);
}

/*
 * The back end keeps the rate of the front end that opens it (48 kHz for
 * deep and fast, the track's own rate for hifi) and runs stereo S24, which
 * the I2S carries in 32-bit slots.
 */
static int tegra_rt5671_be_fixup(struct snd_soc_pcm_runtime *rtd,
				 struct snd_pcm_hw_params *params)
{
	struct snd_interval *channels = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *format = hw_param_mask(params,
						SNDRV_PCM_HW_PARAM_FORMAT);

	channels->min = channels->max = 2;
	snd_mask_none(format);
	snd_mask_set(format, SNDRV_PCM_FORMAT_S24_LE);

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

static enum tegra30_ahub_rxcif tegra_rt5671_fe_rx(struct tegra_rt5671 *m,
						  int fe)
{
	return tegra_rt5671_dam_rx(m->dam[tegra_rt5671_fe_input[fe].dam],
				   tegra_rt5671_fe_input[fe].ch);
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

	tegra30_ahub_set_rx_cif_source(tegra_rt5671_fe_rx(machine, fe),
				       machine->fe_fifo_cif[fe]);

	return 0;
}

static void tegra_rt5671_fe_shutdown(struct snd_pcm_substream *substream,
				     int fe)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(rtd->card);

	tegra30_ahub_unset_rx_cif_source(tegra_rt5671_fe_rx(machine, fe));
	tegra30_ahub_free_tx_fifo(machine->fe_fifo_cif[fe]);
	machine->fe_fifo_cif[fe] = -1;
	rtd->cpu_dai->playback_dma_data = NULL;

	tegra30_ahub_disable_clocks();
}

static int tegra_rt5671_fe_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params, int fe)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(rtd->card);
	enum tegra30_ahub_txcif cif = machine->fe_fifo_cif[fe];
	int bits, cif_bits, pack;
	int ret;

	/* The front-end DAIs offer S16 and S24 (in 32-bit words) */
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		bits = 16;
		cif_bits = TEGRA30_AUDIOCIF_BITS_16;
		pack = TEGRA30_AHUB_CHANNEL_CTRL_TX_PACK_16;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		bits = 24;
		cif_bits = TEGRA30_AUDIOCIF_BITS_24;
		pack = 0;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&machine->dam_lock);
	/* hifi plays at the back-end rate, untouched; a back end another
	 * front end opened at a different rate cannot take it */
	if (fe == FE_HIFI && params_rate(params) != machine->be_rate) {
		dev_dbg(rtd->card->dev, "back end busy at %d Hz\n",
			machine->be_rate);
		mutex_unlock(&machine->dam_lock);
		return -EBUSY;
	}

	/* The DAM input takes the stream as it comes; inside it is 32-bit */
	ret = tegra30_dam_set_acif(machine->dam[tegra_rt5671_fe_input[fe].dam],
				   tegra_rt5671_fe_input[fe].ch,
				   params_channels(params), bits, 2, 32);
	mutex_unlock(&machine->dam_lock);
	if (ret)
		return ret;

	tegra30_ahub_set_tx_cif_channels(cif, params_channels(params),
					 params_channels(params));
	tegra30_ahub_set_tx_cif_bits(cif, cif_bits, cif_bits);
	tegra30_ahub_set_tx_fifo_pack_mode(cif, pack);

	return 0;
}

/*
 * DATA_SYNC as TRM 20.10.4.3/5 recommend: CH0 waits for nothing, CH1 waits
 * for CH0. CH1 may wait only while CH0 runs, or it would stall on a channel
 * that sends nothing; so the wait is set once CH0's stream has started and
 * cleared before it stops. In MIX, CH0 is deep and CH1 fast; in OUT, CH0 is
 * hifi and CH1 the chain from SRC. Called under dam_trigger_lock.
 */
static void tegra_rt5671_dam_sync(struct tegra_rt5671 *machine)
{
	tegra30_dam_ch1_set_datasync(machine->dam[DAM_MIX],
			machine->fe_running[FE_DEEP] ?
				DAM_SYNC_WAIT_CH0 : DAM_SYNC_NONE);
	tegra30_dam_ch1_set_datasync(machine->dam[DAM_OUT],
			machine->fe_running[FE_HIFI] ?
				DAM_SYNC_WAIT_CH0 : DAM_SYNC_NONE);
}

/*
 * Called by the platform before the DMA starts and after it stops.
 *
 * Making neither channel wait does not work: CH0 stalls as soon as CH1
 * starts. Having CH0 wait for CH1, as flounder does, makes the steady stream
 * on CH0 click whenever a short sound starts or stops on CH1.
 *
 * deep and fast reach OUT through SRC; that path is switched on with the
 * first of them and off after the last, downstream first going up.
 */
static int tegra_rt5671_fe_trigger(struct snd_pcm_substream *substream,
				   int cmd, int fe)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_rt5671 *machine = snd_soc_card_get_drvdata(rtd->card);
	int ifc = machine->dam[tegra_rt5671_fe_input[fe].dam];
	int ch = tegra_rt5671_fe_input[fe].ch;
	bool chain = fe != FE_HIFI;
	unsigned long flags;

	spin_lock_irqsave(&machine->dam_trigger_lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		machine->fe_running[fe] = true;
		if (chain && !machine->chain_users++) {
			tegra_rt5671_dam_sync(machine);
			tegra30_dam_enable(machine->dam[DAM_OUT],
					   TEGRA30_DAM_ENABLE, TEGRA30_DAM_CHIN1);
			tegra30_dam_enable(machine->dam[DAM_SRC],
					   TEGRA30_DAM_ENABLE,
					   TEGRA30_DAM_CHIN0_SRC);
		}
		if (fe == FE_FAST)
			tegra_rt5671_dam_sync(machine);
		tegra30_dam_enable(ifc, TEGRA30_DAM_ENABLE, ch);
		tegra30_ahub_enable_tx_fifo(machine->fe_fifo_cif[fe]);
		if (fe == FE_DEEP || fe == FE_HIFI)
			tegra_rt5671_dam_sync(machine);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		machine->fe_running[fe] = false;
		if (fe == FE_DEEP || fe == FE_HIFI)
			tegra_rt5671_dam_sync(machine);
		tegra30_ahub_disable_tx_fifo(machine->fe_fifo_cif[fe]);
		tegra30_dam_enable(ifc, TEGRA30_DAM_DISABLE, ch);
		if (chain && !--machine->chain_users) {
			tegra30_dam_enable(machine->dam[DAM_SRC],
					   TEGRA30_DAM_DISABLE,
					   TEGRA30_DAM_CHIN0_SRC);
			tegra30_dam_enable(machine->dam[DAM_OUT],
					   TEGRA30_DAM_DISABLE, TEGRA30_DAM_CHIN1);
		}
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
TEGRA_RT5671_FE_OPS(fe_hifi, FE_HIFI);

static struct snd_soc_ops tegra_rt5671_be_ops = {
	.hw_params = tegra_rt5671_be_hw_params,
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
	{"DAM Mixer", NULL, "FE2 Playback"},
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
	/* Music on its own rate: the back end follows it when it opens
	 * first, and it reaches the codec untouched while nothing else
	 * plays */
	[DAI_LINK_FE_HIFI] = {
		.name = "rt5671 HiFi",
		.stream_name = "rt5671 HiFi",
		.codec_name = "snd-soc-dummy",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra-pcm-fe2",
		.codec_dai_name = "snd-soc-dummy-dai",
		.ops = &tegra_rt5671_fe_hifi_ops,
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
	int i;

	for (i = 0; i < NUM_DAM; i++) {
		if (machine->dam[i] < 0)
			continue;
		tegra30_dam_free_channel(machine->dam[i],
					 TEGRA30_DAM_CHIN0_SRC);
		tegra30_dam_free_channel(machine->dam[i], TEGRA30_DAM_CHIN1);
		tegra30_dam_free_controller(machine->dam[i]);
		machine->dam[i] = -1;
	}
}

static int tegra_rt5671_alloc_dam(struct tegra_rt5671 *machine,
				  struct device *dev)
{
	int i, ret;

	for (i = 0; i < NUM_DAM; i++)
		machine->dam[i] = -1;

	for (i = 0; i < NUM_DAM; i++) {
		ret = tegra30_dam_allocate_controller();
		if (ret < 0) {
			if (ret != -EPROBE_DEFER)
				dev_err(dev, "No DAM %d for the mixer: %d\n",
					i, ret);
			tegra_rt5671_free_dam(machine);
			return ret;
		}
		machine->dam[i] = ret;
		tegra30_dam_allocate_channel(ret, TEGRA30_DAM_CHIN0_SRC);
		tegra30_dam_allocate_channel(ret, TEGRA30_DAM_CHIN1);
	}

	return 0;
}

static int tegra_rt5671_driver_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_soc_tegra_rt5671;
	struct device_node *np = pdev->dev.of_node;
	struct tegra_rt5671 *machine;
	struct tegra_asoc_platform_data *pdata = NULL;
	unsigned int aif1_fmt;
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

	ret = tegra_rt5671_aif1_fmt(pdata, &aif1_fmt);
	if (ret < 0) {
		dev_err(&pdev->dev, "AIF1: unsupported i2s mode %d, %s master\n",
			pdata->i2s_param[HIFI_CODEC].i2s_mode,
			pdata->i2s_param[HIFI_CODEC].is_i2s_master ?
				"tegra" : "codec");
		if (np)
			kfree(pdata);
		return ret;
	}
	tegra_rt5671_dai[DAI_LINK_HIFI].dai_fmt = aif1_fmt;
	tegra_rt5671_dai[DAI_LINK_HIFI_BE].dai_fmt = aif1_fmt;

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
	machine->fe_fifo_cif[FE_HIFI] = -1;

	ret = tegra_rt5671_alloc_dam(machine, &pdev->dev);
	if (ret)
		goto err_free_machine;

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
