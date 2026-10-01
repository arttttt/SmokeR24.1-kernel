/*
 * tegra30_dam.c - Tegra 30 DAM driver
 *
 * Author: Nikesh Oswal <noswal@nvidia.com>
 * Copyright (C) 2011 - NVIDIA, Inc.
 * Copyright (C) 2012-2014, NVIDIA CORPORATION. All rights reserved.
 *
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

#include <linux/clk.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <sound/soc.h>
#include "tegra30_dam.h"
#include "tegra30_ahub.h"

#define DRV_NAME "tegra30-dam"

static struct tegra30_dam_context	*dams_cont_info[TEGRA30_NR_DAM_IFC];

/*
Static Tables used by DAM driver
*/

enum {
	dam_ch_in0 = 0x0,
	dam_ch_in1,
	dam_ch_out,
	dam_ch_maxnum
} tegra30_dam_chtype;

#ifdef CONFIG_ARCH_TEGRA_3x_SOC
static struct tegra30_dam_src_step_table  step_table[] = {
	{ 8000, 44100, 80 },
	{ 8000, 48000, 1 },
	{ 16000, 44100, 160 },
	{ 16000, 48000, 1 },
	{ 44100, 8000, 441 },
	{ 48000, 8000, 0 },
	{ 44100, 16000, 441 },
	{ 48000, 16000, 0 },
};
#else
#include "tegra30_dam_coef.h"

/* Rates in the order of the coefficient matrix, with their FSIN/FSOUT codes */
static const struct {
	int rate;
	u32 fs;
} tegra30_dam_rates[TEGRA30_DAM_NUM_RATES] = {
	{   8000,  0 },
	{  11025,  4 },
	{  16000,  1 },
	{  22050,  5 },
	{  24000,  6 },
	{  32000,  7 },
	{  44100,  2 },
	{  48000,  3 },
	{  88200,  8 },
	{  96000,  9 },
	{ 176400, 10 },
	{ 192000, 11 },
};
#endif

/*
Internally used helper (static) function prototypes
*/

static inline void tegra30_dam_writel(struct tegra30_dam_context *dam,
			u32 val, u32 reg);
static inline u32 tegra30_dam_readl(struct tegra30_dam_context *dam,
															u32 reg);
static int tegra30_dam_set_output_samplerate(
						struct tegra30_dam_context *dam, int fsout);
static int tegra30_dam_set_input_samplerate(
						struct tegra30_dam_context *dam, int fsin);
static int tegra30_dam_set_step_reset(struct tegra30_dam_context *dam,
										int insample, int outsample);
#ifdef CONFIG_ARCH_TEGRA_3x_SOC
static void tegra30_dam_ch0_set_step(struct tegra30_dam_context *dam,
															int step);
#endif


/*
Regmap and Runtime PM callback function
*/

static bool tegra30_dam_wr_rd_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TEGRA30_DAM_CTRL:
	case TEGRA30_DAM_CLIP:
	case TEGRA30_DAM_CLIP_THRESHOLD:
	case TEGRA30_DAM_AUDIOCIF_OUT_CTRL:
	case TEGRA30_DAM_CH0_CTRL:
	case TEGRA30_DAM_CH0_CONV:
	case TEGRA30_DAM_AUDIOCIF_CH0_CTRL:
	case TEGRA30_DAM_CH1_CTRL:
	case TEGRA30_DAM_CH1_CONV:
	case TEGRA30_DAM_AUDIOCIF_CH1_CTRL:
#ifndef CONFIG_ARCH_TEGRA_3x_SOC
	case TEGRA30_DAM_CH0_BIQUAD_FIXED_COEF_0:
	case TEGRA30_DAM_FARROW_PARAM_0:
	case TEGRA30_DAM_AUDIORAMCTL_DAM_CTRL_0:
	case TEGRA30_DAM_AUDIORAMCTL_DAM_DATA_0:
#endif
		return true;
	default:
		return false;
	};
}

/*
 * The coefficient RAM is reached through an address register that the
 * hardware auto-increments on every data access: a cached copy of either says
 * nothing about the RAM, and replaying them would write a stray word into it.
 */
static bool tegra30_dam_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
#ifndef CONFIG_ARCH_TEGRA_3x_SOC
	case TEGRA30_DAM_AUDIORAMCTL_DAM_CTRL_0:
	case TEGRA30_DAM_AUDIORAMCTL_DAM_DATA_0:
		return true;
#endif
	default:
		return false;
	}
}

static const struct regmap_config tegra30_dam_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
#ifndef CONFIG_ARCH_TEGRA_3x_SOC
	.max_register = TEGRA30_DAM_AUDIORAMCTL_DAM_DATA_0,
#else
	.max_register = TEGRA30_DAM_AUDIOCIF_CH1_CTRL,
#endif
	.writeable_reg = tegra30_dam_wr_rd_reg,
	.readable_reg = tegra30_dam_wr_rd_reg,
	.volatile_reg = tegra30_dam_volatile_reg,
	.cache_type = REGCACHE_RBTREE,
};

static int tegra30_dam_runtime_suspend(struct device *dev)
{
	struct tegra30_dam_context *dam = dev_get_drvdata(dev);

	tegra30_ahub_disable_clocks();
	regcache_cache_only(dam->regmap, true);
	clk_disable_unprepare(dam->dam_clk);

	return 0;
}

static int tegra30_dam_runtime_resume(struct device *dev)
{
	struct tegra30_dam_context *dam = dev_get_drvdata(dev);
	int ret;

	tegra30_ahub_enable_clocks();
	ret = clk_prepare_enable(dam->dam_clk);
	if (ret) {
		dev_err(dev, "clk_enable failed: %d\n", ret);
		return ret;
	}
	regcache_cache_only(dam->regmap, false);

	return 0;
}

/*
DebugFs callback functions
*/

#ifdef CONFIG_DEBUG_FS
static int tegra30_dam_show(struct seq_file *s, void *unused)
{
#define REG(r) { r, #r }
	static const struct {
		int offset;
		const char *name;
	} regs[] = {
		REG(TEGRA30_DAM_CTRL),
		REG(TEGRA30_DAM_CLIP),
		REG(TEGRA30_DAM_CLIP_THRESHOLD),
		REG(TEGRA30_DAM_AUDIOCIF_OUT_CTRL),
		REG(TEGRA30_DAM_CH0_CTRL),
		REG(TEGRA30_DAM_CH0_CONV),
		REG(TEGRA30_DAM_AUDIOCIF_CH0_CTRL),
		REG(TEGRA30_DAM_CH1_CTRL),
		REG(TEGRA30_DAM_CH1_CONV),
		REG(TEGRA30_DAM_AUDIOCIF_CH1_CTRL),
	};
#undef REG

	struct tegra30_dam_context *dam = s->private;
	int i;

	tegra30_ahub_enable_clocks();
	clk_enable(dam->dam_clk);

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		u32 val = tegra30_dam_readl(dam, regs[i].offset);
		seq_printf(s, "%s = %08x\n", regs[i].name, val);
	}

	clk_disable(dam->dam_clk);
	tegra30_ahub_disable_clocks();

	return 0;
}

static int tegra30_dam_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, tegra30_dam_show, inode->i_private);
}

static const struct file_operations tegra30_dam_debug_fops = {
	.open    = tegra30_dam_debug_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static void tegra30_dam_debug_add(struct tegra30_dam_context *dam, int id)
{
	char name[] = DRV_NAME ".0";

	snprintf(name, sizeof(name), DRV_NAME".%1d", id);
	dam->debug = debugfs_create_file(name, S_IRUGO, snd_soc_debugfs_root,
			dam, &tegra30_dam_debug_fops);
}

static void tegra30_dam_debug_remove(struct tegra30_dam_context *dam)
{
	if (dam->debug)
		debugfs_remove(dam->debug);
}
#else
static inline void tegra30_dam_debug_add(struct tegra30_dam_context *dam,
						int id)
{
}

static inline void tegra30_dam_debug_remove(struct tegra30_dam_context *dam)
{
}
#endif

/*
Internally used helper (static) functions
*/
static inline void tegra30_dam_writel(struct tegra30_dam_context *dam,
			u32 val, u32 reg)
{
	regmap_write(dam->regmap, reg, val);
}

static inline u32 tegra30_dam_readl(struct tegra30_dam_context *dam, u32 reg)
{
	u32 val;

	regmap_read(dam->regmap, reg, &val);
	return val;
}

/* FSIN/FSOUT code for a rate, or -EINVAL */
static int tegra30_dam_rate_fs(int rate)
{
#ifdef CONFIG_ARCH_TEGRA_3x_SOC
	switch (rate) {
	case TEGRA30_AUDIO_SAMPLERATE_8000:
		return TEGRA30_DAM_FS_8KHZ;
	case TEGRA30_AUDIO_SAMPLERATE_16000:
		return TEGRA30_DAM_FS_16KHZ;
	case TEGRA30_AUDIO_SAMPLERATE_44100:
		return TEGRA30_DAM_FS_44KHZ;
	case TEGRA30_AUDIO_SAMPLERATE_48000:
		return TEGRA30_DAM_FS_48KHZ;
	default:
		return -EINVAL;
	}
#else
	int i;

	for (i = 0; i < TEGRA30_DAM_NUM_RATES; i++)
		if (tegra30_dam_rates[i].rate == rate)
			return tegra30_dam_rates[i].fs;
	return -EINVAL;
#endif
}

static int tegra30_dam_set_output_samplerate(struct tegra30_dam_context *dam,
					int fsout)
{
	int fs = tegra30_dam_rate_fs(fsout);
	u32 val;

	if (fs < 0)
		return fs;

	val = tegra30_dam_readl(dam, TEGRA30_DAM_CTRL);
	val &= ~TEGRA30_DAM_CTRL_FSOUT_MASK;
	val |= fs << TEGRA30_DAM_CTRL_FSOUT_SHIFT;
	tegra30_dam_writel(dam, val, TEGRA30_DAM_CTRL);

	return 0;
}

static int tegra30_dam_set_input_samplerate(struct tegra30_dam_context *dam,
	int fsin)
{
	int fs = tegra30_dam_rate_fs(fsin);
	u32 val;

	if (fs < 0)
		return fs;

	val = tegra30_dam_readl(dam, TEGRA30_DAM_CH0_CTRL);
	val &= ~TEGRA30_DAM_CH0_CTRL_FSIN_MASK;
	val |= fs << TEGRA30_DAM_CH0_CTRL_FSIN_SHIFT;
	tegra30_dam_writel(dam, val, TEGRA30_DAM_CH0_CTRL);

	return 0;
}

#ifndef CONFIG_ARCH_TEGRA_3x_SOC
/* Index of a rate in tegra30_dam_rates[], or -EINVAL */
static int tegra30_dam_rate_index(int rate)
{
	int i;

	for (i = 0; i < TEGRA30_DAM_NUM_RATES; i++)
		if (tegra30_dam_rates[i].rate == rate)
			return i;
	return -EINVAL;
}

/*
 * FARROW_PARAM for a conversion. The farrow stage is what bridges the 48 kHz
 * and 44.1 kHz families, so only its direction matters. NVIDIA's three values
 * decode as bits 15:8 = L and 7:0 = M of the farrow ratio L/M, and bits 31:16
 * = 2^23 / L: PARAM_2 is 147/160 (towards 44.1 kHz), PARAM_3 is 160/147 (away
 * from it). Every chain in the matrix has a farrow stage exactly when the two
 * rates are of different families.
 */
static u32 tegra30_dam_farrow_param(int fsin, int fsout)
{
	bool in_44 = !(fsin % 11025);
	bool out_44 = !(fsout % 11025);

	if (in_44 == out_44)
		return TEGRA30_FARROW_PARAM_1;

	return out_44 ? TEGRA30_FARROW_PARAM_2 : TEGRA30_FARROW_PARAM_3;
}

/*
 * Number of stages in a coefficient program. Each stage starts with a header
 * whose bits 23:16 give the offset of the next stage, 0 ending the chain.
 */
static int tegra30_dam_coef_stages(const u32 *coef)
{
	int stages = 0;
	int i = 0;

	do {
		if (++stages > TEGRA30_DAM_CH0_CTRL_FILT_STAGES_MAX)
			return -EINVAL;
		i = (coef[i] >> 16) & 0xff;
		if (i >= TEGRA30_DAM_COEF_RAM_DEPTH)
			return -EINVAL;
	} while (i);

	return stages;
}

static int tegra30_dam_write_coeff_ram(struct tegra30_dam_context *dam,
				       const u32 *coef)
{
	unsigned int ctrl;
	int dcnt = 10;
	int i;

	/* Rewind the RAM address, then write it word by word from 0 */
	tegra30_dam_writel(dam, TEGRA30_DAM_RAMCTL_RESET_HW_ADR,
			   TEGRA30_DAM_AUDIORAMCTL_DAM_CTRL_0);
	do {
		ctrl = tegra30_dam_readl(dam, TEGRA30_DAM_AUDIORAMCTL_DAM_CTRL_0);
	} while ((ctrl & TEGRA30_DAM_RAMCTL_RESET_HW_ADR) && --dcnt);
	if (!dcnt)
		return -ETIMEDOUT;

	tegra30_dam_writel(dam, TEGRA30_DAM_RAMCTL_RW_WRITE |
			   TEGRA30_DAM_RAMCTL_HW_ADR_EN,
			   TEGRA30_DAM_AUDIORAMCTL_DAM_CTRL_0);
	for (i = 0; i < TEGRA30_DAM_COEF_RAM_DEPTH; i++)
		tegra30_dam_writel(dam, coef[i],
				   TEGRA30_DAM_AUDIORAMCTL_DAM_DATA_0);

	return 0;
}
#endif

static int tegra30_dam_set_step_reset(struct tegra30_dam_context *dam,
		int insample, int outsample)
{
#ifdef CONFIG_ARCH_TEGRA_3x_SOC
	int step_reset = 0;
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(step_table); i++) {
		if ((insample == step_table[i].insample) &&
			(outsample == step_table[i].outsample))
			step_reset = step_table[i].stepreset;
	}

	tegra30_dam_ch0_set_step(dam, step_reset);
#else
	int in = tegra30_dam_rate_index(insample);
	int out = tegra30_dam_rate_index(outsample);
	const u32 *coef;
	int stages, ret;
	u32 val;

	if (in < 0 || out < 0)
		return -EINVAL;

	coef = tegra30_dam_coef_table[in][out];
	if (IS_ERR(coef))
		return PTR_ERR(coef);

	val = tegra30_dam_readl(dam, TEGRA30_DAM_CH0_CTRL);
	val &= ~(TEGRA30_DAM_CH0_CTRL_COEFF_RAM_ENABLE |
		 TEGRA30_DAM_CH0_CTRL_FILT_STAGES_MASK);

	/* Equal rates: CH0 passes through, no program to run */
	if (coef == DAM_BYPASS_CONV) {
		tegra30_dam_writel(dam, TEGRA30_FARROW_PARAM_RESET,
				   TEGRA30_DAM_FARROW_PARAM_0);
		tegra30_dam_writel(dam, val, TEGRA30_DAM_CH0_CTRL);
		return 0;
	}

	stages = tegra30_dam_coef_stages(coef);
	if (stages < 0)
		return stages;

	ret = tegra30_dam_write_coeff_ram(dam, coef);
	if (ret)
		return ret;

	tegra30_dam_writel(dam, tegra30_dam_farrow_param(insample, outsample),
			   TEGRA30_DAM_FARROW_PARAM_0);
	tegra30_dam_writel(dam, TEGRA30_DAM_CH0_BIQUAD_FIXED_COEF_0_VAL,
			   TEGRA30_DAM_CH0_BIQUAD_FIXED_COEF_0);

	/* FILT_STAGES counts the stages of the program, less one */
	val |= TEGRA30_DAM_CH0_CTRL_COEFF_RAM_ENABLE;
	val |= (stages - 1) << TEGRA30_DAM_CH0_CTRL_FILT_STAGES_SHIFT;
	tegra30_dam_writel(dam, val, TEGRA30_DAM_CH0_CTRL);
#endif

	return 0;
}

#ifdef CONFIG_ARCH_TEGRA_3x_SOC
static void tegra30_dam_ch0_set_step(struct tegra30_dam_context *dam, int step)
{
	u32 val;

	val = tegra30_dam_readl(dam, TEGRA30_DAM_CH0_CTRL);
	val &= ~TEGRA30_DAM_CH0_CTRL_STEP_MASK;
	val |= step << TEGRA30_DAM_CH0_CTRL_STEP_SHIFT;
	tegra30_dam_writel(dam, val, TEGRA30_DAM_CH0_CTRL);
}
#endif

/*
DAM Driver API's
*/

int tegra30_dam_allocate_controller(void)
{
	int i = 0;
	bool probed = false;
	struct tegra30_dam_context *dam = NULL;

	for (i = 0; i < TEGRA30_NR_DAM_IFC; i++) {

		dam =  dams_cont_info[i];

		/* Not probed (yet) */
		if (!dam)
			continue;
		probed = true;

		if (!dam->in_use) {
			dam->in_use = true;
			return i;
		}
	}

	return probed ? -EBUSY : -EPROBE_DEFER;
}
EXPORT_SYMBOL(tegra30_dam_allocate_controller);

void tegra30_dam_disable_clock(int ifc)
{
	struct tegra30_dam_context *dam;

	if (ifc >= TEGRA30_NR_DAM_IFC)
		return;

	dam =  dams_cont_info[ifc];
	pm_runtime_put(dam->dev);
}
EXPORT_SYMBOL(tegra30_dam_disable_clock);

int tegra30_dam_enable_clock(int ifc)
{
	struct tegra30_dam_context *dam;

	if (ifc >= TEGRA30_NR_DAM_IFC)
		return -EINVAL;

	dam =  dams_cont_info[ifc];
	pm_runtime_get_sync(dam->dev);

	return 0;
}
EXPORT_SYMBOL(tegra30_dam_enable_clock);

int tegra30_dam_allocate_channel(int ifc, int chid)
{
	struct tegra30_dam_context *dam = NULL;

	if (ifc >= TEGRA30_NR_DAM_IFC)
		return -EINVAL;

	dam =  dams_cont_info[ifc];

	if (!dam->ch_alloc[chid]) {
		dam->ch_alloc[chid] = true;
		return 0;
	}

	return -ENOENT;
}
EXPORT_SYMBOL(tegra30_dam_allocate_channel);

int tegra30_dam_free_channel(int ifc, int chid)
{
	struct tegra30_dam_context *dam = NULL;

	if (ifc >= TEGRA30_NR_DAM_IFC)
		return -EINVAL;

	dam =  dams_cont_info[ifc];

	if (dam->ch_alloc[chid]) {
		dam->ch_alloc[chid] = false;
		return 0;
	}

	return -EINVAL;
}
EXPORT_SYMBOL(tegra30_dam_free_channel);

int tegra30_dam_free_controller(int ifc)
{
	struct tegra30_dam_context *dam = NULL;

	if (ifc >= TEGRA30_NR_DAM_IFC)
		return -EINVAL;

	dam =  dams_cont_info[ifc];

	if (!dam->ch_alloc[dam_ch_in0] &&
		!dam->ch_alloc[dam_ch_in1]) {
		dam->in_use = false;
		return 0;
	}

	return -EINVAL;
}
EXPORT_SYMBOL(tegra30_dam_free_controller);

/*
 * Set the rate of a channel. CH0 goes through the sample rate converter, so
 * set CHOUT first: the converter is programmed for CH0's rate against it.
 * CH1 has no converter and must run at the output rate.
 */
int tegra30_dam_set_samplerate(int ifc, int chid, int samplerate)
{
	struct tegra30_dam_context *dam;
	int ret;

	if ((ifc < 0) || (ifc >= TEGRA30_NR_DAM_IFC))
		return -EINVAL;

	dam = dams_cont_info[ifc];

	switch (chid) {
	case dam_ch_in0:
		ret = tegra30_dam_set_input_samplerate(dam, samplerate);
		if (ret)
			return ret;
		ret = tegra30_dam_set_step_reset(dam, samplerate,
						 dam->outsamplerate);
		if (ret)
			return ret;
		dam->ch_insamplerate[dam_ch_in0] = samplerate;
		break;
	case dam_ch_in1:
		if (samplerate != dam->outsamplerate)
			return -EINVAL;
		dam->ch_insamplerate[dam_ch_in1] = samplerate;
		break;
	case dam_ch_out:
		ret = tegra30_dam_set_output_samplerate(dam, samplerate);
		if (ret)
			return ret;
		dam->outsamplerate = samplerate;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(tegra30_dam_set_samplerate);

int tegra30_dam_set_gain(int ifc, int chid, int gain)
{
	if (ifc >= TEGRA30_NR_DAM_IFC)
		return -EINVAL;

	switch (chid) {
	case dam_ch_in0:
		tegra30_dam_writel(dams_cont_info[ifc], gain,
			TEGRA30_DAM_CH0_CONV);
		break;
	case dam_ch_in1:
		tegra30_dam_writel(dams_cont_info[ifc], gain,
			TEGRA30_DAM_CH1_CONV);
		break;
	default:
		break;
	}

	return 0;
}
EXPORT_SYMBOL(tegra30_dam_set_gain);

int tegra30_dam_set_acif(int ifc, int chid, unsigned int audio_channels,
	unsigned int audio_bits, unsigned int client_channels,
	unsigned int client_bits)
{
	unsigned int reg;
	unsigned int value = 0;

	if (ifc >= TEGRA30_NR_DAM_IFC)
		return -EINVAL;

#ifndef CONFIG_ARCH_TEGRA_3x_SOC
	/*
	 * CH0 is not mono-only here: stereo mixing (bypass) and
	 * STEREO_SRC_EN both take a stereo CH0.
	 */
	/*as per dam spec file chout is fixed to 32 bits*/
	/*so accept ch0, ch1 and chout as 32bit always*/
	if (client_bits != 32)
		return -EINVAL;
#else
	/*ch0 takes input as mono/16bit always*/
	if ((chid == dam_ch_in0) &&
		((client_channels != 1) || (client_bits != 16)))
		return -EINVAL;
#endif

	value |= TEGRA30_AUDIOCIF_CTRL_MONO_CONV_COPY;
	value |= TEGRA30_AUDIOCIF_CTRL_STEREO_CONV_AVG;
	value |= (audio_channels-1)  <<
				TEGRA30_AUDIOCIF_CTRL_AUDIO_CHANNELS_SHIFT;
	value |= (((audio_bits>>2)-1) <<
				TEGRA30_AUDIOCIF_CTRL_AUDIO_BITS_SHIFT);
	value |= (client_channels-1) <<
				TEGRA30_AUDIOCIF_CTRL_CLIENT_CHANNELS_SHIFT;
	value |= (((client_bits>>2)-1) <<
				TEGRA30_AUDIOCIF_CTRL_CLIENT_BITS_SHIFT);

	switch (chid) {
	case dam_ch_out:
		value |= TEGRA30_CIF_DIRECTION_TX;
		reg = TEGRA30_DAM_AUDIOCIF_OUT_CTRL;
		break;
	case dam_ch_in0:
		value |= TEGRA30_CIF_DIRECTION_RX;
		reg = TEGRA30_DAM_AUDIOCIF_CH0_CTRL;
		break;
	case dam_ch_in1:
		value |= TEGRA30_CIF_DIRECTION_RX;
		reg = TEGRA30_DAM_AUDIOCIF_CH1_CTRL;
		break;
	default:
		return -EINVAL;
	}

	tegra30_dam_writel(dams_cont_info[ifc], value, reg);

	return 0;
}
EXPORT_SYMBOL(tegra30_dam_set_acif);

void tegra30_dam_enable(int ifc, int on, int chid)
{
	u32 old_val, val, enreg;
	u32 old_val_dam, val_dam;
	int dcnt = 10;
	struct tegra30_dam_context *dam;

	if ((ifc < 0) || (ifc >= TEGRA30_NR_DAM_IFC) ||
	    (chid < dam_ch_in0) || (chid > dam_ch_in1))
		return;

	dam = dams_cont_info[ifc];

	if (chid == dam_ch_in0)
		enreg = TEGRA30_DAM_CH0_CTRL;
	else
		enreg = TEGRA30_DAM_CH1_CTRL;

	old_val = val = tegra30_dam_readl(dam, enreg);

	if (on) {
		if (!dam->ch_enable_refcnt[chid]++)
			val |= TEGRA30_DAM_CH0_CTRL_EN;
	} else if (dam->ch_enable_refcnt[chid]) {
		dam->ch_enable_refcnt[chid]--;
		if (!dam->ch_enable_refcnt[chid])
			val &= ~TEGRA30_DAM_CH0_CTRL_EN;
	}

	old_val_dam = val_dam = tegra30_dam_readl(dam, TEGRA30_DAM_CTRL);

	if (dam->ch_enable_refcnt[dam_ch_in0] ||
		dam->ch_enable_refcnt[dam_ch_in1])
		val_dam |= TEGRA30_DAM_CTRL_DAM_EN;
	else
		val_dam &= ~TEGRA30_DAM_CTRL_DAM_EN;

	if (val != old_val) {
		tegra30_dam_writel(dam, val, enreg);

		if (!on) {
			if (chid == dam_ch_in0) {
				while (!tegra30_ahub_dam_ch0_is_empty(ifc)
					&& dcnt--)
					udelay(100);

				dcnt = 10;
			}
			else {
				while (!tegra30_ahub_dam_ch1_is_empty(ifc)
					&& dcnt--)
					udelay(100);

				dcnt = 10;
			}
		}
	}

	if (old_val_dam != val_dam) {
		tegra30_dam_writel(dam, val_dam, TEGRA30_DAM_CTRL);
		if (!on) {
			while (!tegra30_ahub_dam_tx_is_empty(ifc) && dcnt--)
				udelay(100);

			dcnt = 10;
		}
	}
}
EXPORT_SYMBOL(tegra30_dam_enable);

void tegra30_dam_ch0_set_datasync(int ifc, int datasync)
{
	u32 val;
	struct tegra30_dam_context *dam = NULL;

	dam =  dams_cont_info[ifc];
	val = tegra30_dam_readl(dam, TEGRA30_DAM_CH0_CTRL);
	val &= ~TEGRA30_DAM_CH0_CTRL_DATA_SYNC_MASK;
	val |= datasync << TEGRA30_DAM_DATA_SYNC_SHIFT;
	tegra30_dam_writel(dam, val, TEGRA30_DAM_CH0_CTRL);
}
EXPORT_SYMBOL(tegra30_dam_ch0_set_datasync);

void tegra30_dam_ch1_set_datasync(int ifc, int datasync)
{
	u32 val;
	struct tegra30_dam_context *dam = NULL;

	dam =  dams_cont_info[ifc];
	val = tegra30_dam_readl(dam, TEGRA30_DAM_CH1_CTRL);
	val &= ~TEGRA30_DAM_CH1_CTRL_DATA_SYNC_MASK;
	val |= datasync << TEGRA30_DAM_DATA_SYNC_SHIFT;
	tegra30_dam_writel(dam, val, TEGRA30_DAM_CH1_CTRL);
}
EXPORT_SYMBOL(tegra30_dam_ch1_set_datasync);

void tegra30_dam_enable_clip_counter(struct tegra30_dam_context *dam, int on)
{
	u32 val;

	val = tegra30_dam_readl(dam, TEGRA30_DAM_CLIP);
	val &= ~TEGRA30_DAM_CLIP_COUNTER_ENABLE;
	val |= on ?  TEGRA30_DAM_CLIP_COUNTER_ENABLE : 0;
	tegra30_dam_writel(dam, val, TEGRA30_DAM_CLIP);
}

int tegra30_dam_set_acif_stereo_conv(int ifc, int chtype, int conv)
{
	unsigned int reg;
	unsigned int val = 0;

	if (ifc >= TEGRA30_NR_DAM_IFC)
		return -EINVAL;

	if ((conv != TEGRA30_CIF_STEREOCONV_CH0) &&
		(conv != TEGRA30_CIF_STEREOCONV_CH1) &&
		(conv != TEGRA30_CIF_STEREOCONV_AVG))
			return -EINVAL;

	switch (chtype) {
	case dam_ch_out:
		reg = TEGRA30_DAM_AUDIOCIF_OUT_CTRL;
		break;
	case dam_ch_in0:
		reg = TEGRA30_DAM_AUDIOCIF_CH0_CTRL;
		break;
	case dam_ch_in1:
		reg = TEGRA30_DAM_AUDIOCIF_CH1_CTRL;
		break;
	default:
		return -EINVAL;
	}

	val = tegra30_dam_readl(dams_cont_info[ifc], reg);
	val &= ~TEGRA30_CIF_STEREOCONV_MASK;
	val |= conv;

	tegra30_dam_writel(dams_cont_info[ifc], val, reg);

	return 0;
}

#ifndef CONFIG_ARCH_TEGRA_3x_SOC
/*
 * Stereo mixing works only in bypass, so CH0 must already run at the
 * output rate. Call with the DAM clock enabled.
 */
int tegra30_dam_enable_stereo_mixing(int ifc, int on)
{
	struct tegra30_dam_context *dam;
	u32 val;

	if ((ifc < 0) || (ifc >= TEGRA30_NR_DAM_IFC))
		return -EINVAL;

	dam = dams_cont_info[ifc];

	if (on && (dam->ch_insamplerate[dam_ch_in0] != dam->outsamplerate))
		return -EINVAL;

	val = tegra30_dam_readl(dam, TEGRA30_DAM_CTRL);
	if (on)
		val |= TEGRA30_DAM_CTRL_STEREO_MIXING_ENABLE;
	else
		val &= ~TEGRA30_DAM_CTRL_STEREO_MIXING_ENABLE;
	tegra30_dam_writel(dam, val, TEGRA30_DAM_CTRL);

	return 0;
}
EXPORT_SYMBOL(tegra30_dam_enable_stereo_mixing);

/* Convert CH0 as stereo rather than mono (TRM 20.10.4.1 STEREO_SRC_EN) */
int tegra30_dam_enable_stereo_src(int ifc, int on)
{
	struct tegra30_dam_context *dam;
	u32 val;

	if ((ifc < 0) || (ifc >= TEGRA30_NR_DAM_IFC))
		return -EINVAL;

	dam = dams_cont_info[ifc];

	val = tegra30_dam_readl(dam, TEGRA30_DAM_CTRL);
	if (on)
		val |= TEGRA30_DAM_CTRL_STEREO_SRC_ENABLE;
	else
		val &= ~TEGRA30_DAM_CTRL_STEREO_SRC_ENABLE;
	tegra30_dam_writel(dam, val, TEGRA30_DAM_CTRL);

	return 0;
}
EXPORT_SYMBOL(tegra30_dam_enable_stereo_src);
#endif

/*
 * Resets the DAM logic and its CIFs. The hardware keeps the configuration
 * and clears only the DAM and channel enables, so call it while no channel
 * is enabled. Call with the DAM clock enabled.
 */
int tegra30_dam_soft_reset(int ifc)
{
	struct tegra30_dam_context *dam;
	unsigned int val, cur;
	int dcnt = 100;

	if ((ifc < 0) || (ifc >= TEGRA30_NR_DAM_IFC))
		return -EINVAL;

	dam = dams_cont_info[ifc];

	if (dam->ch_enable_refcnt[dam_ch_in0] ||
	    dam->ch_enable_refcnt[dam_ch_in1])
		return -EBUSY;

	val = tegra30_dam_readl(dam, TEGRA30_DAM_CTRL);

	/* The bit clears itself, so neither the write nor the poll may
	 * go through the register cache. */
	regcache_cache_bypass(dam->regmap, true);
	regmap_write(dam->regmap, TEGRA30_DAM_CTRL,
		     val | TEGRA30_DAM_CTRL_SOFT_RESET_ENABLE);
	do {
		udelay(10);
		regmap_read(dam->regmap, TEGRA30_DAM_CTRL, &cur);
	} while ((cur & TEGRA30_DAM_CTRL_SOFT_RESET_ENABLE) && --dcnt);
	regcache_cache_bypass(dam->regmap, false);

	/* Bring the cache in line with what the reset left behind */
	tegra30_dam_writel(dam, val & ~TEGRA30_DAM_CTRL_DAM_EN,
			   TEGRA30_DAM_CTRL);
	val = tegra30_dam_readl(dam, TEGRA30_DAM_CH0_CTRL);
	tegra30_dam_writel(dam, val & ~TEGRA30_DAM_CH0_CTRL_EN,
			   TEGRA30_DAM_CH0_CTRL);
	val = tegra30_dam_readl(dam, TEGRA30_DAM_CH1_CTRL);
	tegra30_dam_writel(dam, val & ~TEGRA30_DAM_CH1_CTRL_EN,
			   TEGRA30_DAM_CH1_CTRL);

	if (!dcnt) {
		dev_err(dam->dev, "soft reset timed out\n");
		return -ETIMEDOUT;
	}

	return 0;
}
EXPORT_SYMBOL(tegra30_dam_soft_reset);


/*
DAM Driver probe and remove functions
*/

static int tegra30_dam_probe(struct platform_device *pdev)
{
	struct resource *res,  *region;
	struct tegra30_dam_context *dam;
	int ret = 0;
	u32 val32;

	if (pdev->dev.of_node) {
		of_property_read_u32(pdev->dev.of_node, "nvidia,ahub-dam-id",
			&val32);
		pdev->id = (int)val32;
	}

	if ((pdev->id < 0) ||
		(pdev->id >= TEGRA30_NR_DAM_IFC)) {
		dev_err(&pdev->dev, "ID %d out of range\n", pdev->id);
		return -EINVAL;
	}

	dams_cont_info[pdev->id] = devm_kzalloc(&pdev->dev,
					sizeof(struct tegra30_dam_context),
					GFP_KERNEL);
	if (!dams_cont_info[pdev->id]) {
		dev_err(&pdev->dev, "Can't allocate dam context\n");
		ret = -ENOMEM;
		goto exit;
	}
	dams_cont_info[pdev->id]->dev = &pdev->dev;

	dam = dams_cont_info[pdev->id];
	dev_set_drvdata(&pdev->dev, dam);

	dam->dam_clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(dam->dam_clk)) {
		dev_err(&pdev->dev, "Can't retrieve dam clock\n");
		ret = PTR_ERR(dam->dam_clk);
		goto err_free;
	}
	/*
	 * The clock is left as the board configured it. The converter's
	 * heaviest programs (a farrow stage at 176.4 kHz, as in 44.1 -> 48 kHz)
	 * need more than the ~12 MHz this used to force on it.
	 */
	dev_dbg(&pdev->dev, "clock %lu Hz\n", clk_get_rate(dam->dam_clk));

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "No memory 0 resource\n");
		ret = -ENODEV;
		goto err_clk_put_dam;
	}

	region = devm_request_mem_region(&pdev->dev, res->start,
			resource_size(res), pdev->name);
	if (!region) {
		dev_err(&pdev->dev, "Memory region 0 already claimed\n");
		ret = -EBUSY;
		goto err_clk_put_dam;
	}

	dam->damregs = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!dam->damregs) {
		dev_err(&pdev->dev, "ioremap 0 failed\n");
		ret = -ENOMEM;
		goto err_clk_put_dam;
	}

	dam->regmap = devm_regmap_init_mmio(&pdev->dev, dam->damregs,
				    &tegra30_dam_regmap_config);
	if (IS_ERR(dam->regmap)) {
		dev_err(&pdev->dev, "regmap init failed\n");
		ret = PTR_ERR(dam->regmap);
		goto err_clk_put_dam;
	}
	regcache_cache_only(dam->regmap, true);

	pm_runtime_enable(&pdev->dev);
	if (!pm_runtime_enabled(&pdev->dev)) {
		ret = tegra30_dam_runtime_resume(&pdev->dev);
		if (ret)
			goto err_pm_disable;
	}

	tegra30_dam_debug_add(dam, pdev->id);

	return 0;

err_pm_disable:
	pm_runtime_disable(&pdev->dev);
err_clk_put_dam:
	clk_put(dam->dam_clk);
err_free:
	dams_cont_info[pdev->id] = NULL;
exit:
	return ret;
}

static int tegra30_dam_remove(struct platform_device *pdev)
{
	struct tegra30_dam_context *dam;

	pm_runtime_disable(&pdev->dev);
	if (!pm_runtime_status_suspended(&pdev->dev))
		tegra30_dam_runtime_suspend(&pdev->dev);

	dam = platform_get_drvdata(pdev);
	clk_put(dam->dam_clk);
	tegra30_dam_debug_remove(dam);
	dams_cont_info[pdev->id] = NULL;

	return 0;
}

static const struct of_device_id tegra30_dam_of_match[] = {
	{ .compatible = "nvidia,tegra30-dam",},
	{},
};

static const struct dev_pm_ops tegra30_dam_pm_ops = {
	SET_RUNTIME_PM_OPS(tegra30_dam_runtime_suspend,
			   tegra30_dam_runtime_resume, NULL)
};

static struct platform_driver tegra30_dam_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = tegra30_dam_of_match,
		.pm = &tegra30_dam_pm_ops,
	},
	.probe = tegra30_dam_probe,
	.remove = tegra30_dam_remove,
};
module_platform_driver(tegra30_dam_driver);

MODULE_AUTHOR("Nikesh Oswal <noswal@nvidia.com>");
MODULE_DESCRIPTION("Tegra 30 DAM driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
