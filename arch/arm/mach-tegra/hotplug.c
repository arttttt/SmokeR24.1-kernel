/*
 *
 *  Copyright (C) 2002 ARM Ltd.
 *  All Rights Reserved
 *  Copyright (C) 2010-2015 NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/clk/tegra.h>
#include <linux/cpu_pm.h>
#include <linux/clk/tegra.h>
#include <linux/irqchip/tegra.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/platform/tegra/common.h>
#include <uapi/linux/psci.h>

#include <asm/cacheflush.h>
#include <asm/smp_plat.h>
#include <asm/psci.h>

#include "sleep.h"

static void (*tegra_hotplug_shutdown)(void);

int tegra_cpu_kill(unsigned int cpu)
{
#if defined(CONFIG_ARM_PSCI)
	int err, i;
#endif
	cpu = cpu_logical_map(cpu);

	if (!tegra_cpu_is_secure()) {
		tegra_wait_cpu_in_reset(cpu);
		tegra_disable_cpu_clock(cpu);
		return 1;
	}

#if defined(CONFIG_ARM_PSCI)
	/*
	 * cpu_kill could race with cpu_die and we can
	 * potentially end up declaring this cpu undead
	 * while it is dying. So, try again a few times.
	 */

	for (i = 0; i < 10; i++) {
		err = psci_ops.affinity_info(cpu, 0);
		if (err == PSCI_0_2_AFFINITY_LEVEL_OFF) {
			pr_debug("CPU%d killed.\n", cpu);
			return 1;
		}

		msleep(10);
		pr_debug("Retrying again to check for CPU%d kill\n", cpu);
	}

	pr_warn("CPU%d may not have shut down cleanly (AFFINITY_INFO reports %d)\n",
			cpu, err);
#endif

	/* Make cpu_kill() fail. */
	return 0;
}

/*
 * platform-specific code to shutdown a CPU
 *
 * Called with IRQs disabled
 */
void tegra_cpu_die(unsigned int cpu)
{
#if defined(CONFIG_ARM_PSCI)
	struct psci_power_state state = {
		.type = PSCI_POWER_STATE_TYPE_POWER_DOWN
	};

	if (psci_ops.cpu_off) {
		psci_ops.cpu_off(state);

		/* Should never return here. */
		BUG();
	}
#endif
	cpu = cpu_logical_map(cpu);

#ifndef CONFIG_ARCH_TEGRA_2x_SOC
	/* Disable GIC CPU interface for this CPU. */
	tegra_gic_cpu_disable(false);
#endif

	/* Flush the L1 data cache. */
	tegra_flush_l1_cache();

	/* Shut down the current CPU. */
	tegra_hotplug_shutdown();

	/* Clock gate the CPU */
	tegra_wait_cpu_in_reset(cpu);
	tegra_disable_cpu_clock(cpu);

	/* Should never return here. */
	BUG();
}

#ifdef CONFIG_ARCH_TEGRA_2x_SOC
extern void tegra20_hotplug_shutdown(void);
void __init tegra20_hotplug_init(void)
{
	tegra_hotplug_shutdown = tegra20_hotplug_shutdown;
}
#endif

extern void tegra30_hotplug_shutdown(void);
void __init tegra30_hotplug_init(void)
{
	tegra_hotplug_shutdown = tegra30_hotplug_shutdown;
}
