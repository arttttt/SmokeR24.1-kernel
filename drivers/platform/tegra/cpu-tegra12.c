/*
 * drivers/platform/tegra/cpu-tegra12.c
 *
 * Copyright (C) 2015 NVIDIA Corporation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/cpufreq.h>
#include <linux/tegra-fuse.h>

#include <linux/platform/tegra/clock.h>
#include <linux/platform/tegra/dvfs.h>
#include <linux/platform/tegra/cpu-tegra.h>

/*
 * Frequency table index must be sequential starting at 0 and frequencies
 * must be ascending.
 */
#define CPU_FREQ_STEP 102000 /* 102MHz cpu_g table step */
#define CPU_FREQ_TABLE_MAX_SIZE (2 * MAX_DVFS_FREQS + 1)

static struct cpufreq_frequency_table freq_table[CPU_FREQ_TABLE_MAX_SIZE];
static struct tegra_cpufreq_table_data freq_table_data;

static DEFINE_MUTEX(scaling_data_lock);

#ifndef CONFIG_ARCH_TEGRA_13x_SOC
struct tegra_cpufreq_table_data *tegra_cpufreq_table_get(void)
{
	int i, j;
	bool g_vmin_done = false;
	unsigned int freq, lp_backup_freq, g_vmin_freq, g_start_freq, max_freq;
	struct clk *cpu_clk_g = tegra_get_clock_by_name("cpu_g");
	struct clk *cpu_clk_lp = tegra_get_clock_by_name("cpu_lp");

	/* Initialize once */
	if (freq_table_data.freq_table)
		return &freq_table_data;

	if (tegra_is_soc_automotive_speedo())
		freq_table_data.preserve_across_suspend = true;

	/* Clean table */
	for (i = 0; i < CPU_FREQ_TABLE_MAX_SIZE; i++) {
		freq_table[i].index = i;
		freq_table[i].frequency = CPUFREQ_TABLE_END;
	}

	lp_backup_freq = cpu_clk_lp->u.cpu.backup_rate / 1000;
	if (!lp_backup_freq) {
		WARN(1, "%s: cannot make cpufreq table: no LP CPU backup rate\n",
		     __func__);
		return NULL;
	}
	if (!cpu_clk_lp->dvfs) {
		WARN(1, "%s: cannot make cpufreq table: no LP CPU dvfs\n",
		     __func__);
		return NULL;
	}
	if (!cpu_clk_g->dvfs) {
		WARN(1, "%s: cannot make cpufreq table: no G CPU dvfs\n",
		     __func__);
		return NULL;
	}
	g_vmin_freq = cpu_clk_g->dvfs->freqs[0] / 1000;
	if (g_vmin_freq < lp_backup_freq) {
		WARN(1, "%s: cannot make cpufreq table: LP CPU backup rate"
			" exceeds G CPU rate at Vmin\n", __func__);
		return NULL;
	}
	/* Avoid duplicate frequency if g_vim_freq is already part of table */
	if (g_vmin_freq == lp_backup_freq)
		g_vmin_done = true;

	/* Start with backup frequencies */
	i = 0;
	freq = lp_backup_freq;
	freq_table[i++].frequency = freq/4;
	freq_table[i++].frequency = freq/2;
	freq_table[i++].frequency = freq;

	/* Throttle low index at backup level*/
	freq_table_data.throttle_lowest_index = i - 1;

	/*
	 * Next, set table steps along LP CPU dvfs ladder, but make sure G CPU
	 * dvfs rate at minimum voltage is not missed (if it happens to be below
	 * LP maximum rate)
	 */
	max_freq = cpu_clk_lp->max_rate / 1000;
	for (j = 0; j < cpu_clk_lp->dvfs->num_freqs; j++) {
		freq = cpu_clk_lp->dvfs->freqs[j] / 1000;
		if (freq <= lp_backup_freq)
			continue;

		if (!g_vmin_done && (freq >= g_vmin_freq)) {
			g_vmin_done = true;
			if (freq > g_vmin_freq)
				freq_table[i++].frequency = g_vmin_freq;
		}

		/* Skip duplicated frequency (may happen on LP CPU only) */
		if (freq_table[i-1].frequency != freq)
			freq_table[i++].frequency = freq;

		if (freq == max_freq)
			break;
	}

	/* Set G CPU min rate at least one table step below LP maximum */
	cpu_clk_g->min_rate = min(freq_table[i-2].frequency, g_vmin_freq)*1000;

	/* Suspend index at max LP CPU */
	freq_table_data.suspend_index = i - 1;

	/* Fill in "hole" (if any) between LP CPU maximum rate and G CPU dvfs
	   ladder rate at minimum voltage */
	if (freq < g_vmin_freq) {
		int n = (g_vmin_freq - freq) / CPU_FREQ_STEP;
		for (j = 0; j <= n; j++) {
			freq = g_vmin_freq - CPU_FREQ_STEP * (n - j);
			freq_table[i++].frequency = freq;
		}
	}

	/* Now, step along the rest of G CPU dvfs ladder */
	g_start_freq = freq;
	max_freq = cpu_clk_g->max_rate / 1000;
	for (j = 0; j < cpu_clk_g->dvfs->num_freqs; j++) {
		freq = cpu_clk_g->dvfs->freqs[j] / 1000;
		if (freq > g_start_freq)
			freq_table[i++].frequency = freq;
		if (freq == max_freq)
			break;
	}

	/* Throttle high index one step below maximum */
	BUG_ON(i >= CPU_FREQ_TABLE_MAX_SIZE);
	freq_table_data.throttle_highest_index = i - 2;
	freq_table_data.freq_table = freq_table;
	return &freq_table_data;
}

#else

#define GRANULARITY_KHZ   25500
#define GRANULARITY_END 1020000
#define CPU_THROTTLE_FREQ 408000
#define CPU_SUSPEND_FREQ  408000

struct tegra_cpufreq_table_data *tegra_cpufreq_table_get(void)
{
	int i, j;
	unsigned int freq, max_freq, cpu_min_freq;
	struct clk *cpu_clk_g = tegra_get_clock_by_name("cpu_g");

	/* Initialize once */
	if (freq_table_data.freq_table)
		return &freq_table_data;

	/* Clean table */
	for (i = 0; i < CPU_FREQ_TABLE_MAX_SIZE; i++) {
		freq_table[i].index = i;
		freq_table[i].frequency = CPUFREQ_TABLE_END;
	}

	if (!cpu_clk_g->dvfs) {
		WARN(1, "%s: cannot make cpufreq table: no CPU dvfs\n",
		     __func__);
		return NULL;
	}

	cpu_min_freq = 204000;

	cpu_clk_g->min_rate = cpu_min_freq*1000;

	i = 0;
	freq_table[i++].frequency = cpu_min_freq;
	for (j = 1; j <= (GRANULARITY_END - cpu_min_freq)/GRANULARITY_KHZ; j++)
		freq_table[i++].frequency = cpu_min_freq + j*GRANULARITY_KHZ;

	/* Now, step along the rest of G CPU dvfs ladder */
	max_freq = cpu_clk_g->max_rate / 1000;
	for (j = 0; j < cpu_clk_g->dvfs->num_freqs; j++) {
		freq = cpu_clk_g->dvfs->freqs[j] / 1000;
		if (freq > GRANULARITY_END)
			freq_table[i++].frequency = freq;
		if (freq == max_freq)
			break;
	}

	freq_table_data.throttle_lowest_index = 0;
	freq_table_data.suspend_index = 0;

	for (j = 1; j < i; j++) {
		if ((freq_table[j].frequency > CPU_THROTTLE_FREQ) &&
			(freq_table[j-1].frequency <= CPU_THROTTLE_FREQ))
			freq_table_data.throttle_lowest_index = j - 1;
		if ((freq_table[j].frequency > CPU_SUSPEND_FREQ) &&
			(freq_table[j-1].frequency <= CPU_SUSPEND_FREQ))
			freq_table_data.suspend_index = j - 1;
	}

	/* Throttle high index one step below maximum */
	BUG_ON(i >= CPU_FREQ_TABLE_MAX_SIZE);
	freq_table_data.throttle_highest_index = i - 2;
	freq_table_data.freq_table = freq_table;
	return &freq_table_data;
}

#endif

/* EMC/CPU frequency ratio for power/performance optimization */
unsigned long tegra_emc_to_cpu_ratio(unsigned long cpu_rate)
{
	static unsigned long emc_max_rate;

	if (emc_max_rate == 0)
		emc_max_rate = clk_round_rate(
			tegra_get_clock_by_name("emc"), ULONG_MAX);

	/* Vote on memory bus frequency based on cpu frequency;
	   cpu rate is in kHz, emc rate is in Hz */
	if (cpu_rate >= 1300000)
		return emc_max_rate;	/* cpu >= 1.3GHz, emc max */
	else if (cpu_rate >= 975000)
		return 550000000;	/* cpu >= 975 MHz, emc 550 MHz */
	else if (cpu_rate >= 725000)
		return  350000000;	/* cpu >= 725 MHz, emc 350 MHz */
	else if (cpu_rate >= 500000)
		return  150000000;	/* cpu >= 500 MHz, emc 150 MHz */
	else if (cpu_rate >= 275000)
		return  50000000;	/* cpu >= 275 MHz, emc 50 MHz */
	else
		return 0;		/* emc min */
}

static struct device_node *of_get_scaling_node(const char *name)
{
	struct device_node *scaling_np = NULL;
	struct device_node *np =
		of_find_compatible_node(NULL, NULL, "nvidia,tegra124-cpufreq");

	if (!np || !of_device_is_available(np)) {
		pr_debug("%s: Tegra124 cpufreq node is not found\n", __func__);
		of_node_put(np);
		return NULL;
	}

	scaling_np = of_get_child_by_name(np, name);
	of_node_put(np);
	if (!scaling_np || !of_device_is_available(scaling_np)) {
		pr_debug("%s: %s for cpufreq is not found\n", __func__, name);
		of_node_put(scaling_np);
		return NULL;
	}
	return scaling_np;
}

/*
 * Vote on memory bus frequency based on cpu frequency.
 * input cpu rate is in kHz
 * output emc rate is in Hz
 */
static unsigned long emc_max_rate;
static u32 *emc_cpu_table;
static int cpu_emc_table_src = CPU_EMC_TABLE_SRC_DT;
static int emc_cpu_table_size;

static u32 *cpufreq_emc_table_get(int *table_size)
{
	int freqs_num;
	u32 *freqs = NULL;
	struct device_node *np = NULL;
	const char *propname = "emc-cpu-limit-table";

	/* Find cpufreq node */
	np = of_get_scaling_node("emc-scaling-data");
	if (!np){
		return ERR_PTR(-ENODATA);
	}

	/* Read frequency table */
	if (!of_find_property(np, propname, &freqs_num)) {
		pr_err("%s: %s is not found\n", __func__, propname);
		goto out;
	}

	/* must have even entries */
	if (!freqs_num || (freqs_num % (sizeof(*freqs) * 2))) {
		pr_err("%s: invalid %s size %d\n", __func__, propname,
				freqs_num);
		goto out;
	}

	freqs = kzalloc(freqs_num, GFP_KERNEL);
	if (!freqs) {
		pr_err("%s: failed to allocate limit table\n", __func__);
		goto out;
	}

	freqs_num /= sizeof(*freqs);
	if (of_property_read_u32_array(np, propname, freqs, freqs_num)) {
		pr_err("%s: failed to read %s\n", __func__, propname);
		goto out;
	}

	of_node_put(np);
	*table_size = freqs_num;
	return freqs;

out:
	kfree(freqs);
	of_node_put(np);
	return ERR_PTR(-ENODATA);
}

static unsigned long dt_emc_cpu_limit(unsigned long cpu_rate,
		unsigned long emc_max_rate)
{
	int i;

	for (i = 0; i < emc_cpu_table_size; i += 2) {
		if (cpu_rate < emc_cpu_table[i])
			break;
	}

	if (i)
		return min(emc_max_rate, emc_cpu_table[i-1] * 1000UL);
	return 0;
}

static unsigned long default_emc_cpu_limit(unsigned long cpu_rate,
		unsigned long emc_max_rate)
{
	/* Vote on memory bus frequency based on cpu frequency;
	   cpu rate is in kHz, emc rate is in Hz */
	if (cpu_rate >= 1734000)
		return emc_max_rate;    /* cpu >= 1.734GHz, emc max */
	else if (cpu_rate >= 1326000)
		return 792000000;   /* cpu >= 1.362GHz, emc 792 MHz */
	else if (cpu_rate >= 1224000)
		return 600000000;   /* cpu >= 1.224GHz, emc 600 MHz */
	else if (cpu_rate >= 1044000)
		return 528000000;   /* cpu >= 1.044GHz, emc 528 MHz */
	else if (cpu_rate >= 960000)
		return 396000000;   /* cpu >= 960 MHz, emc 396 MHz */
	else if (cpu_rate >= 828000)
		return 300000000;  /* cpu >= 828 MHz, emc 300 MHz */
	else if (cpu_rate >= 696000)
		return 204000000;  /* cpu >= 696 MHz, emc 204 MHz */
	else if (cpu_rate >= 564000)
		return 102000000;   /* cpu >= 564 MHz, emc 102 MHz */
	else if (cpu_rate >= 312000)
		return 50000000;   /* cpu >= 312 MHz, emc 50 MHz */
	else
		return 0;       /* emc min */
}

#ifdef CONFIG_ARCH_TEGRA_13x_SOC
/* EMC/CPU frequency operational requirement limit */
unsigned long tegra_emc_cpu_limit(unsigned long cpu_rate)
{
	static unsigned long last_emc_rate;
	unsigned long emc_rate;

	/* Vote on memory bus frequency based on cpu frequency;
	   cpu rate is in kHz, emc rate is in Hz */

	if ((tegra_revision != TEGRA_REVISION_A01) &&
	    (tegra_revision != TEGRA_REVISION_A02))
		return 0; /* no frequency dependency for A03+ revisions */

	if (cpu_rate > 1020000)
		emc_rate = 600000000;	/* cpu > 1.02GHz, emc 600MHz */
	else
		emc_rate = 300000000;	/* 300MHz floor always */

	/* When going down, allow some time for CPU DFLL to settle */
	if (emc_rate < last_emc_rate)
		udelay(200);		/* FIXME: to be characterized */

	last_emc_rate = emc_rate;
	return emc_rate;
}
#else
unsigned long tegra_emc_cpu_limit(unsigned long cpu_rate)
{
	static unsigned long emc_rate;

	if (emc_max_rate == 0) {
		struct clk *emc = tegra_get_clock_by_name("emc");
		if (!emc)
			return -ENODEV;
		emc_max_rate = clk_round_rate(emc, ULONG_MAX);
	}

	mutex_lock(&scaling_data_lock);
	if (!emc_cpu_table)
		emc_cpu_table =
			cpufreq_emc_table_get(&emc_cpu_table_size);

	if ((cpu_emc_table_src == CPU_EMC_TABLE_SRC_DEFAULT) ||
			IS_ERR(emc_cpu_table))
		emc_rate =
			default_emc_cpu_limit(cpu_rate, emc_max_rate);
	else
		emc_rate = dt_emc_cpu_limit(cpu_rate, emc_max_rate);

	mutex_unlock(&scaling_data_lock);

	return emc_rate;
}
#endif

int tegra_update_mselect_rate(unsigned long cpu_rate)
{
	static struct clk *mselect; /* statics init to 0 */

	unsigned long mselect_rate;

	if (!mselect) {
		mselect = tegra_get_clock_by_name("cpu.mselect");
		if (!mselect)
			return -ENODEV;
	}

	/* Vote on mselect frequency based on cpu frequency:
	   keep mselect at half of cpu rate up to 102 MHz;
	   cpu rate is in kHz, mselect rate is in Hz */
	mselect_rate = DIV_ROUND_UP(cpu_rate, 2) * 1000;
	mselect_rate = min(mselect_rate, 102000000UL);
	return clk_set_rate(mselect, mselect_rate);
}

int set_cpu_emc_limit_table_source(int table_src)
{
	if (table_src != CPU_EMC_TABLE_SRC_DT &&
			table_src != CPU_EMC_TABLE_SRC_DEFAULT)
		return -1;
	cpu_emc_table_src = table_src;

	return 0;
}

int get_cpu_emc_limit_table_source(void)
{
	return cpu_emc_table_src;
}
