/*
 * arch/arm/mach-tegra/include/mach/pm.h
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *
 * Copyright (c) 2010-2014, NVIDIA CORPORATION.  All rights reserved.
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


#ifndef _MACH_TEGRA_PM_H_
#define _MACH_TEGRA_PM_H_

#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/clkdev.h>
#include <linux/tegra-pmc.h>
#include <linux/tegra-pm.h>
#include <linux/tegra_cluster_control.h>

#include "iomap.h"


#define PMC_SCRATCH0		0x50
#define PMC_SCRATCH1		0x54
#define PMC_SCRATCH4		0x60
#define PMC_SCRATCH203		0x84c

unsigned long tegra_cpu_power_good_time(void);
unsigned long tegra_cpu_power_off_time(void);
unsigned int tegra_cpu_suspend_freq(void);
unsigned long tegra_cpu_lp2_min_residency(void);
unsigned long tegra_mc_clk_stop_min_residency(void);
#ifdef CONFIG_ARCH_TEGRA_HAS_SYMMETRIC_CPU_PWR_GATE
unsigned long tegra_min_residency_vmin_fmin(void);
unsigned long tegra_min_residency_ncpu(void);
unsigned long tegra_min_residency_crail(void);
bool tegra_crail_can_start_early(void);
#else
static inline bool tegra_crail_can_start_early(void)
{ return false; }
#endif
void tegra_limit_cpu_power_timers(unsigned long us_on, unsigned long us_off);
void tegra_clear_cpu_in_pd(int cpu);
bool tegra_set_cpu_in_pd(int cpu);

void tegra_mc_clk_prepare(void);
void tegra_mc_clk_finish(void);
#ifdef CONFIG_TEGRA_LP1_LOW_COREVOLTAGE
int tegra_is_lp1_suspend_mode(void);
#endif
int tegra_is_lp0_suspend_mode(void);
void tegra_lp1bb_suspend_emc_rate(unsigned long emc_min, unsigned long emc_max);
void tegra_lp1bb_suspend_mv_set(int mv);
unsigned long tegra_lp1bb_emc_min_rate_get(void);

#define FLOW_CTRL_CPU_PWR_CSR \
	(IO_ADDRESS(TEGRA_FLOW_CTRL_BASE) + 0x38)
#define FLOW_CTRL_CPU_PWR_CSR_RAIL_ENABLE	1

#define FLOW_CTRL_MPID \
	(IO_ADDRESS(TEGRA_FLOW_CTRL_BASE) + 0x3c)

#define FLOW_CTRL_RAM_REPAIR \
	(IO_ADDRESS(TEGRA_FLOW_CTRL_BASE) + 0x40)
#define FLOW_CTRL_RAM_REPAIR_BYPASS_EN	(1<<2)
#define FLOW_CTRL_RAM_REPAIR_STS	(1<<1)
#define FLOW_CTRL_RAM_REPAIR_REQ	(1<<0)

#define FUSE_SKU_DIRECT_CONFIG \
	(IO_ADDRESS(TEGRA_FUSE_BASE) + 0x1F4)
#define FUSE_SKU_DISABLE_ALL_CPUS	(1<<5)
#define FUSE_SKU_NUM_DISABLED_CPUS(x)	(((x) >> 3) & 3)

u64 tegra_rtc_read_ms(void);

unsigned int tegra_idle_power_down_last(unsigned int us, unsigned int flags);

#if defined(CONFIG_PM_SLEEP) && !defined(CONFIG_ARCH_TEGRA_2x_SOC)
void tegra_lp0_suspend_mc(void);
void tegra_lp0_resume_mc(void);
void tegra_lp0_cpu_mode(bool enter);
#else
static inline void tegra_lp0_suspend_mc(void) {}
static inline void tegra_lp0_resume_mc(void) {}
static inline void tegra_lp0_cpu_mode(bool enter) {}
#endif

enum tegra_cluster_switch_time_id {
	tegra_cluster_switch_time_id_start = 0,
	tegra_cluster_switch_time_id_prolog,
	tegra_cluster_switch_time_id_switch,
	tegra_cluster_switch_time_id_epilog,
	tegra_cluster_switch_time_id_end,
	tegra_cluster_switch_time_id_max
};

#ifdef CONFIG_TEGRA_CLUSTER_CONTROL
#define INSTRUMENT_CLUSTER_SWITCH 1	/* Should be zero for shipping code */
#define DEBUG_CLUSTER_SWITCH 0		/* Should be zero for shipping code */
#define PARAMETERIZE_CLUSTER_SWITCH 1	/* Should be zero for shipping code */

#define CLUSTER_SWITCH_TIME_AVG_SHIFT	4
#define CLUSTER_SWITCH_AVG_SAMPLES	(0x1U << CLUSTER_SWITCH_TIME_AVG_SHIFT)

static inline bool is_g_cluster_present(void)
{
	u32 fuse_sku = readl(FUSE_SKU_DIRECT_CONFIG);
	if (fuse_sku & FUSE_SKU_DISABLE_ALL_CPUS)
		return false;
	return true;
}
int tegra_switch_to_g_cluster(void);
int tegra_switch_to_lp_cluster(void);
#else
#define INSTRUMENT_CLUSTER_SWITCH 0	/* Must be zero for ARCH_TEGRA_2x_SOC */
#define DEBUG_CLUSTER_SWITCH 0		/* Must be zero for ARCH_TEGRA_2x_SOC */
#define PARAMETERIZE_CLUSTER_SWITCH 0	/* Must be zero for ARCH_TEGRA_2x_SOC */

static inline bool is_g_cluster_present(void)   { return true; }
static inline int tegra_switch_to_g_cluster(void)
{
	return -EPERM;
}
static inline int tegra_switch_to_lp_cluster(void)
{
	return -EPERM;
}
#endif

#if INSTRUMENT_CLUSTER_SWITCH
void tegra_cluster_switch_time(unsigned int flags, int id);
#else
static inline void tegra_cluster_switch_time(unsigned int flags, int id) { }
#endif

#ifdef CONFIG_ARCH_TEGRA_2x_SOC
void tegra2_lp0_suspend_init(void);
void tegra2_lp2_set_trigger(unsigned long cycles);
unsigned long tegra2_lp2_timer_remain(void);
#else
void tegra3_lp2_set_trigger(unsigned long cycles);
unsigned long tegra3_lp2_timer_remain(void);
int tegra3_is_cpu_wake_timer_ready(unsigned int cpu);
void tegra3_lp2_timer_cancel_secondary(void);
#endif

static inline void tegra_lp0_suspend_init(void)
{
#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	tegra2_lp0_suspend_init();
#endif
}

static inline void tegra_pd_set_trigger(unsigned long cycles)
{
#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	tegra2_lp2_set_trigger(cycles);
#else
	tegra3_lp2_set_trigger(cycles);
#endif
}

static inline unsigned long tegra_pd_timer_remain(void)
{
#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	return tegra2_lp2_timer_remain();
#else
	return tegra3_lp2_timer_remain();
#endif
}

static inline int tegra_is_cpu_wake_timer_ready(unsigned int cpu)
{
#if defined(CONFIG_TEGRA_LP2_CPU_TIMER) || defined(CONFIG_ARCH_TEGRA_2x_SOC)
	return 1;
#else
	return tegra3_is_cpu_wake_timer_ready(cpu);
#endif
}

static inline void tegra_pd_timer_cancel_secondary(void)
{
#ifndef CONFIG_ARCH_TEGRA_2x_SOC
	tegra3_lp2_timer_cancel_secondary();
#endif
}

#if DEBUG_CLUSTER_SWITCH && 0 /* !!!FIXME!!! THIS IS BROKEN */
extern unsigned int tegra_cluster_debug;
#define DEBUG_CLUSTER(x) do { if (tegra_cluster_debug) printk x; } while (0)
#else
#define DEBUG_CLUSTER(x) do { } while (0)
#endif
#if PARAMETERIZE_CLUSTER_SWITCH
void tegra_cluster_switch_set_parameters(unsigned int us, unsigned int flags);
#else
static inline void tegra_cluster_switch_set_parameters(
	unsigned int us, unsigned int flags)
{ }
#endif

#ifdef CONFIG_SMP
extern bool tegra_all_cpus_booted __read_mostly;
#else
#define tegra_all_cpus_booted (true)
#endif

#if !defined(CONFIG_ARCH_TEGRA_2x_SOC) && !defined(CONFIG_ARCH_TEGRA_3x_SOC) \
	&& defined(CONFIG_SMP)
void tegra_smp_clear_power_mask(void);
#else
static inline void tegra_smp_clear_power_mask(void){}
#endif

u32 tegra_register_suspend_vectors(u32, u32);

#endif /* _MACH_TEGRA_PM_H_ */
