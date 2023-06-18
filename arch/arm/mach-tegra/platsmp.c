/*
 *  linux/arch/arm/mach-tegra/platsmp.c
 *
 *  Copyright (C) 2002 ARM Ltd.
 *  All Rights Reserved
 *
 *  Copyright (C) 2009 Palm
 *  All Rights Reserved
 *
 *  Copyright (C) 2010-2015, NVIDIA Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/clk/tegra.h>
#include <linux/cpumask.h>
#include <linux/tegra-powergate.h>
#include <linux/tegra-timer.h>
#include <linux/tegra-fuse.h>
#include <linux/fiq_glue.h>

#include <asm/cputype.h>
#include <asm/smp_plat.h>
#include <asm/smp_scu.h>
#include <asm/psci.h>

#include <linux/platform/tegra/flowctrl.h>
#include <linux/platform/tegra/reset.h>
#include "pm.h"
#include <linux/platform/tegra/clock.h>
#include "sleep.h"
#include <linux/platform/tegra/cpu-tegra.h>

#include <linux/platform/tegra/common.h>
#include "iomap.h"

bool tegra_all_cpus_booted;

static DECLARE_BITMAP(tegra_cpu_init_bits, CONFIG_NR_CPUS) __read_mostly;
const struct cpumask *const tegra_cpu_init_mask = to_cpumask(tegra_cpu_init_bits);
#define tegra_cpu_init_map	(*(cpumask_t *)tegra_cpu_init_mask)

static DECLARE_BITMAP(tegra_cpu_power_up_by_fc, CONFIG_NR_CPUS) __read_mostly;
static struct cpumask *tegra_cpu_power_mask =
				to_cpumask(tegra_cpu_power_up_by_fc);
#define tegra_cpu_power_map	(*(cpumask_t *)tegra_cpu_power_mask)

#ifndef CONFIG_ARCH_TEGRA_2x_SOC
#define CAR_BOND_OUT_V \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x390)
#define CAR_BOND_OUT_V_CPU_G	(1<<0)
#endif

#define CLAMP_STATUS	0x2c
#define PWRGATE_TOGGLE	0x30

#define PMC_TOGGLE_START	0x100

#ifdef CONFIG_HAVE_ARM_SCU
static void __iomem *scu_base = IO_ADDRESS(TEGRA_ARM_PERIF_BASE);
#endif

static unsigned int number_of_cores;

static void __iomem *pmc = IO_ADDRESS(TEGRA_PMC_BASE);
#define pmc_writel(value, reg)	writel(value, pmc + (reg))
#define pmc_readl(reg)		readl(pmc + (reg))

static void __init setup_core_count(void)
{
#ifndef CONFIG_ARCH_TEGRA_2x_SOC
	u32 l2ctlr;

	unsigned int cpuid;

	/* Dual core SKU */
	if (tegra_get_chipid() == TEGRA_CHIPID_TEGRA11) {
		if (tegra_get_sku_id() == 0x20) {
			number_of_cores = 2;
			return;
		}
	}
	cpuid = (read_cpuid_id() >> 4) & 0xFFF;

#ifdef CONFIG_ARM64
	/* Denver ASIM? */
	if (cpuid == 0xE0F) {
		/* 30th bit of MPIDR_EL1: 0 => SMP, 1 => UP */
		__asm__("mrs %0, mpidr_el1\n" : "=r" (l2ctlr));
		number_of_cores = (l2ctlr & (1 << 30)) ? 2 : 1;
	}
#else
	/* Cortex-A15? */
	if (cpuid == 0xC0F) {
		__asm__("mrc p15, 1, %0, c9, c0, 2\n" : "=r" (l2ctlr));
		number_of_cores = ((l2ctlr >> 24) & 3) + 1;
	}
#endif
	else {
#endif
#ifdef CONFIG_HAVE_ARM_SCU
		number_of_cores = scu_get_core_count(scu_base);
#else
		number_of_cores = 1;
#endif
#ifndef CONFIG_ARCH_TEGRA_2x_SOC
	}
	if (number_of_cores > 1) {
		u32 fuse_sku = readl(FUSE_SKU_DIRECT_CONFIG);
		number_of_cores -= FUSE_SKU_NUM_DISABLED_CPUS(fuse_sku);
		BUG_ON((int)number_of_cores <= 0);
	}
#endif
}

static unsigned int available_cpus(void)
{

	BUG_ON((int)number_of_cores <= 0);

	return number_of_cores;
}

static int is_g_cluster_available(unsigned int cpu)
{
#ifdef CONFIG_TEGRA_CLUSTER_CONTROL
	u32 fuse_sku = readl(FUSE_SKU_DIRECT_CONFIG);
	u32 bond_out = readl(CAR_BOND_OUT_V);

	/* Does the G CPU complex exist at all? */
	if ((fuse_sku & FUSE_SKU_DISABLE_ALL_CPUS) ||
	    (bond_out & CAR_BOND_OUT_V_CPU_G))
		return -EPERM;

	if (cpu >= available_cpus())
		return -EPERM;

	/* FIXME: The G CPU can be unavailable for a number of reasons
	 *	  (e.g., low battery, over temperature, etc.). Add checks for
	 *	  these conditions. */
	return 0;
#else
	return -EPERM;
#endif
}

static bool is_cpu_powered(unsigned int cpu)
{
	if (is_lp_cluster())
		return true;
	else
		return tegra_powergate_is_powered(TEGRA_CPU_POWERGATE_ID(cpu));
}

static void __cpuinit tegra_secondary_init(unsigned int cpu)
{
	cpumask_set_cpu(cpu, to_cpumask(tegra_cpu_init_bits));
	cpumask_set_cpu(cpu, tegra_cpu_power_mask);
#ifdef CONFIG_TEGRA_FIQ_DEBUGGER
	fiq_glue_resume();
#endif
	if (!tegra_all_cpus_booted)
		if (cpumask_equal(tegra_cpu_init_mask, cpu_present_mask))
			tegra_all_cpus_booted = true;
}

static int tegra20_power_up_cpu(unsigned int cpu)
{

	/* Enable the CPU clock. */
	tegra_enable_cpu_clock(cpu);

	/* Clear flow controller CSR. */
	flowctrl_write_cpu_csr(cpu, 0);

	return 0;
}

static int tegra30_power_up_cpu(unsigned int cpu)
{
	int ret;
	unsigned long timeout;
	bool booted = false;

	BUG_ON(cpu == raw_smp_processor_id());
	BUG_ON(is_lp_cluster());

	if (cpu_isset(cpu, tegra_cpu_init_map))
		booted = true;

	cpu = cpu_logical_map(cpu);

	/* If this cpu has booted this function is entered after
	 * CPU has been already un-gated by flow controller. Wait
	 * for confirmation that cpu is powered and remove clamps.
	 * On first boot entry do not wait - go to direct ungate.
	 */
	if (booted) {
		timeout = jiffies + msecs_to_jiffies(50);
		do {
			if (is_cpu_powered(cpu))
				goto remove_clamps;
			udelay(10);
		} while (time_before(jiffies, timeout));
	}

	/* First boot or Flow controller did not work as expected. Try to
	   directly toggle power gates. Error if direct power on also fails. */
	if (!is_cpu_powered(cpu)) {
		ret = tegra_unpowergate_partition(TEGRA_CPU_POWERGATE_ID(cpu));
		if (ret)
			goto fail;

		/* Wait for the power to come up. */
		timeout = jiffies + 10*HZ;

		do {
			if (is_cpu_powered(cpu))
				goto remove_clamps;
			udelay(10);
		} while (time_before(jiffies, timeout));
		ret = -ETIMEDOUT;
		goto fail;
	}

remove_clamps:
	/* CPU partition is powered. Enable the CPU clock. */
	tegra_enable_cpu_clock(cpu);
	udelay(10);

	/* Remove I/O clamps. */
	ret = tegra_powergate_remove_clamping(TEGRA_CPU_POWERGATE_ID(cpu));
	if (ret)
		return ret;

	udelay(10);
fail:

	/* Clear flow controller CSR. */
	flowctrl_write_cpu_csr(cpu, 0);

	return 0;
}

static int tegra11x_power_up_cpu(unsigned int cpu)
{
	BUG_ON(cpu == raw_smp_processor_id());
	BUG_ON(is_lp_cluster());

	cpu = cpu_logical_map(cpu);

#if defined(CONFIG_ARM_PSCI)
	/* monitor takes care of CPU_ON */
	if (tegra_cpu_is_secure())
		return psci_ops.cpu_on(cpu, __pa(tegra_secondary_startup));
#endif

	if (cpu_isset(cpu, tegra_cpu_power_map)) {
		/* set SCLK as event trigger for flow conroller */
		flowctrl_write_cpu_csr(cpu, 0x1);
		flowctrl_write_cpu_halt(cpu, 0x48000000);
	} else {
		u32 reg;

		reg = PMC_TOGGLE_START | TEGRA_CPU_POWERGATE_ID(cpu);
		pmc_writel(reg, PWRGATE_TOGGLE);
		pmc_readl(PWRGATE_TOGGLE);
	}

	return 0;
}

static int __cpuinit tegra_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	int status;

	cpu = cpu_logical_map(cpu);

	/* Avoid timer calibration on slave cpus. Use the value calibrated
	 * on master cpu. This reduces the bringup time for each slave cpu
	 * by around 260ms.
	 */
	preset_lpj = loops_per_jiffy;
	if (is_lp_cluster()) {
		struct clk *cpu_clk, *cpu_g_clk;

		/* The G CPU may not be available for a variety of reasons. */
		status = is_g_cluster_available(cpu);
		if (status)
			goto done;

		cpu_clk = tegra_get_clock_by_name("cpu");
		cpu_g_clk = tegra_get_clock_by_name("cpu_g");

		/* Switch to G CPU before continuing. */
		if (!cpu_clk || !cpu_g_clk) {
			/* Early boot, clock infrastructure is not initialized
			   - CPU mode switch is not allowed */
			status = -EINVAL;
		} else {
#if defined(CONFIG_CPU_FREQ) && !defined(CONFIG_COMMON_CLK)
			/* set cpu rate is within g-mode range before switch */
			unsigned int speed = max(
				(unsigned long)tegra_getspeed(0),
				clk_get_min_rate(cpu_g_clk) / 1000);
			tegra_update_cpu_speed(speed);
#endif
			status = tegra_cluster_switch(cpu_clk, cpu_g_clk);
		}

		if (status)
			goto done;
	}

	smp_wmb();

#if defined(CONFIG_ARCH_TEGRA_2x_SOC) || defined(CONFIG_ARCH_TEGRA_3x_SOC)
	/*
	 * Force the CPU into reset. The CPU must remain in reset when the
	 * flow controller state is cleared (which will cause the flow
	 * controller to stop driving reset if the CPU has been power-gated
	 * via the flow controller). This will have no effect on first boot
	 * of the CPU since it should already be in reset.
	 */
	tegra_put_cpu_in_reset(cpu);
#endif

	switch (tegra_get_chip_id()) {
	case TEGRA_CHIPID_TEGRA2:
		/*
		 * Unhalt the CPU. If the flow controller was used to power-gate
		 * the CPU this will cause the flow controller to stop driving
		 * reset. The CPU will remain in reset because the clock and
		 * reset block is now driving reset.
		 */
		flowctrl_write_cpu_halt(cpu, 0);
		status = tegra20_power_up_cpu(cpu);
		break;
	case TEGRA_CHIPID_TEGRA3:
		/*
		 * Unhalt the CPU. If the flow controller was used to power-gate
		 * the CPU this will cause the flow controller to stop driving
		 * reset. The CPU will remain in reset because the clock and
		 * reset block is now driving reset.
		 */
		flowctrl_write_cpu_halt(cpu, 0);
		status = tegra30_power_up_cpu(cpu);
		break;
	case TEGRA_CHIPID_TEGRA11:
	case TEGRA_CHIPID_TEGRA14:
	case TEGRA_CHIPID_TEGRA12:
		status = tegra11x_power_up_cpu(cpu);
		break;
	default:
		status = -EINVAL;
		break;
	}

#if defined(CONFIG_ARCH_TEGRA_2x_SOC) || defined(CONFIG_ARCH_TEGRA_3x_SOC)
	if (status)
		goto done;

	/* Take the CPU out of reset. */
	tegra_cpu_out_of_reset(cpu);
#endif
done:
	return status;
}

/*
 * Initialise the CPU possible map early - this describes the CPUs
 * which may be present or become present in the system.
 */
static void __init tegra_smp_init_cpus(void)
{
	unsigned int ncores;
	unsigned int i;

	setup_core_count();

	ncores = available_cpus();

	if (ncores > nr_cpu_ids) {
		pr_warn("SMP: %u cores greater than maximum (%u), clipping\n",
			ncores, nr_cpu_ids);
		ncores = nr_cpu_ids;
	}

	for (i = 0; i < ncores; i++)
		set_cpu_possible(i, true);

	tegra_all_cpus_booted = true;
}

static void __init tegra_smp_prepare_cpus(unsigned int max_cpus)
{
	/* Always mark the boot CPU as initialized. */
	cpumask_set_cpu(0, to_cpumask(tegra_cpu_init_bits));

	/* Always mark the boot CPU as initially powered up */
	cpumask_set_cpu(0, tegra_cpu_power_mask);

#ifdef CONFIG_HAVE_ARM_SCU
	scu_enable(scu_base);
#endif
}

#if !defined(CONFIG_ARCH_TEGRA_2x_SOC) && !defined(CONFIG_ARCH_TEGRA_3x_SOC)
void tegra_smp_clear_power_mask()
{
	cpumask_clear(tegra_cpu_power_mask);
	cpumask_set_cpu(0, tegra_cpu_power_mask);
}
#endif

#ifdef CONFIG_HOTPLUG_CPU
int tegra_cpu_disable(unsigned int cpu)
{
	if (cpu == 0) return -EPERM;
	/* only call this if the cputimer is in use */
#if !defined(CONFIG_ARM_ARCH_TIMER) && !defined(CONFIG_HAVE_ARM_TWD)
	tegra_cputimer_reset_irq_affinity(cpu);
#endif
	return 0;
}
#endif

struct smp_operations tegra_smp_ops __initdata = {
	.smp_init_cpus		= tegra_smp_init_cpus,
	.smp_prepare_cpus	= tegra_smp_prepare_cpus,
	.smp_secondary_init	= tegra_secondary_init,
	.smp_boot_secondary	= tegra_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_kill		= tegra_cpu_kill,
	.cpu_die		= tegra_cpu_die,
	.cpu_disable		= tegra_cpu_disable,
#endif
};
