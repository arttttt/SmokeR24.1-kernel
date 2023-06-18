/*
 * drivers/platform/tegra/reset.c
 *
 * Copyright (C) 2011-2014, NVIDIA Corporation. All rights reserved.
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

#include <linux/init.h>
#include <linux/io.h>
#include <linux/cpumask.h>
#include <linux/bitops.h>
#include <linux/tegra-soc.h>
#include <linux/tegra-fuse.h>

#include <asm/cacheflush.h>
#include <asm/psci.h>

#include "iomap.h"
#include "irammap.h"
#include <linux/platform/tegra/reset.h>
#include "sleep.h"
#include "pm.h"
#include <linux/platform/tegra/common.h>

#define TEGRA_IRAM_RESET_BASE (TEGRA_IRAM_BASE + \
				TEGRA_IRAM_RESET_HANDLER_OFFSET)

static bool is_enabled;

static void tegra_cpu_reset_handler_enable(void)
{
	void __iomem *iram_base = IO_ADDRESS(TEGRA_IRAM_BASE);
	void __iomem *evp_cpu_reset =
		IO_ADDRESS(TEGRA_EXCEPTION_VECTORS_BASE + 0x100);
	void __iomem *sb_ctrl = IO_ADDRESS(TEGRA_SB_BASE);
	unsigned long reg;

	BUG_ON(is_enabled);
	BUG_ON(tegra_cpu_reset_handler_size > TEGRA_RESET_HANDLER_SIZE);

	memcpy(iram_base, (void *)__tegra_cpu_reset_handler_start,
		tegra_cpu_reset_handler_size);

	/* NOTE: This must be the one and only write to the EVP CPU
	 * reset vector in the entire system. */
	writel(TEGRA_RESET_HANDLER_BASE +
		tegra_cpu_reset_handler_offset, evp_cpu_reset);
	wmb();
	reg = readl(evp_cpu_reset);

	/*
	 * Prevent further modifications to the physical reset vector.
	 *  NOTE: Has no effect on chips prior to Tegra30.
	 */
	if (tegra_get_chip_id() != TEGRA_CHIPID_TEGRA2) {
		reg = readl(sb_ctrl);
		reg |= 2;
		writel(reg, sb_ctrl);
		wmb();
	}

	is_enabled = true;
}

#ifdef CONFIG_PM_SLEEP
void tegra_cpu_reset_handler_save(void)
{
	unsigned int i;
	BUG_ON(!is_enabled);
	for (i = 0; i < TEGRA_RESET_DATA_SIZE; i++)
		__tegra_cpu_reset_handler_data[i] =
			tegra_cpu_reset_handler_ptr[i];
	is_enabled = false;
}

void tegra_cpu_reset_handler_restore(void)
{
	unsigned int i;
	BUG_ON(is_enabled);
	tegra_cpu_reset_handler_enable();
	for (i = 0; i < TEGRA_RESET_DATA_SIZE; i++)
		tegra_cpu_reset_handler_ptr[i] =
			__tegra_cpu_reset_handler_data[i];
	is_enabled = true;
}
#endif

static int __init tegra_cpu_reset_handler_init(void)
{
#ifdef CONFIG_SMP
	__tegra_cpu_reset_handler_data[TEGRA_RESET_MASK_PRESENT] =
		*((ulong *)cpu_present_mask);
	__tegra_cpu_reset_handler_data[TEGRA_RESET_STARTUP_SECONDARY] =
		virt_to_phys((void *)tegra_secondary_startup);
#endif

#ifdef CONFIG_PM_SLEEP
	__tegra_cpu_reset_handler_data[TEGRA_RESET_STARTUP_LP1] =
		TEGRA_IRAM_CODE_AREA;
	__tegra_cpu_reset_handler_data[TEGRA_RESET_STARTUP_LP2] =
		virt_to_phys((void *)tegra_resume);
#endif

#ifdef CONFIG_ARM64
	flush_icache_range(
	(unsigned long)&__tegra_cpu_reset_handler_data[0],
	(unsigned long)&__tegra_cpu_reset_handler_data[TEGRA_RESET_DATA_SIZE]);
#else
	/* Push all of reset handler data out to the L3 memory system. */
	__cpuc_coherent_kern_range(
	(unsigned long)&__tegra_cpu_reset_handler_data[0],
	(unsigned long)&__tegra_cpu_reset_handler_data[TEGRA_RESET_DATA_SIZE]);

	outer_clean_range(__pa(&__tegra_cpu_reset_handler_data[0]),
		__pa(&__tegra_cpu_reset_handler_data[TEGRA_RESET_DATA_SIZE]));
#endif

	if (!tegra_cpu_is_dsim()) /* Can't write IRAM on DSIM/MTS (yet) */
		tegra_cpu_reset_handler_enable();

	__tegra_cpu_reset_handler_data[TEGRA_RESET_SECURE_FW_PRESENT] =
		tegra_cpu_is_secure();

	return 0;
}
early_initcall(tegra_cpu_reset_handler_init);
