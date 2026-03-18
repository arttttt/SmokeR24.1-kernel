/*
 * drivers/media/platform/tegra/mipical/mipi_cal_t124.c
 *
 * MIPI bias pad initialization for Tegra T124 (Tegra K1).
 *
 * On T124 there is no runtime per-lane calibration like T210.
 * The MIPI receiver bias pad just needs two configuration bits
 * cleared once to enable proper operation.
 *
 * Reference: vi2.c vi2_mipi_bias_pad_init() from the legacy
 * soc_camera framework, which performs the same register writes.
 *
 * Copyright (c) 2025, Smoke Team. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <linux/io.h>
#include <linux/clk.h>
#include <linux/export.h>

#include "mipi_cal.h"

#define MIPI_CAL_BASE			0x700e3000
#define MIPI_CAL_SIZE			0x100

#define MIPI_BIAS_PAD_CFG0		0x58
#define  E_VCLAMP_REF			(1 << 0)

#define MIPI_BIAS_PAD_CFG2		0x60
#define  PDVREG				(1 << 1)

static DEFINE_MUTEX(t124_mipi_lock);
static bool t124_mipi_bias_enabled;

int tegra_mipi_bias_pad_enable(void)
{
	void __iomem *base;
	struct clk *clk;
	u32 val;
	int ret = 0;

	mutex_lock(&t124_mipi_lock);
	if (t124_mipi_bias_enabled)
		goto out;

	clk = clk_get_sys("mipi-cal", NULL);
	if (IS_ERR(clk)) {
		ret = PTR_ERR(clk);
		pr_err("%s: cannot get mipi-cal clk: %d\n", __func__, ret);
		goto out;
	}

	base = ioremap(MIPI_CAL_BASE, MIPI_CAL_SIZE);
	if (!base) {
		clk_put(clk);
		ret = -ENOMEM;
		goto out;
	}

	clk_prepare_enable(clk);

	/* Clear E_VCLAMP_REF in MIPI_BIAS_PAD_CFG0 */
	val = readl(base + MIPI_BIAS_PAD_CFG0);
	val &= ~E_VCLAMP_REF;
	writel(val, base + MIPI_BIAS_PAD_CFG0);

	/* Clear PDVREG in MIPI_BIAS_PAD_CFG2 */
	val = readl(base + MIPI_BIAS_PAD_CFG2);
	val &= ~PDVREG;
	writel(val, base + MIPI_BIAS_PAD_CFG2);

	clk_disable_unprepare(clk);
	clk_put(clk);
	iounmap(base);

	t124_mipi_bias_enabled = true;
out:
	mutex_unlock(&t124_mipi_lock);
	return ret;
}
EXPORT_SYMBOL(tegra_mipi_bias_pad_enable);

int tegra_mipi_bias_pad_disable(void)
{
	/* T124 bias pad doesn't need runtime disable */
	return 0;
}
EXPORT_SYMBOL(tegra_mipi_bias_pad_disable);

int tegra_mipi_calibration(int lanes)
{
	/* T124 does not have runtime per-lane calibration like T210 */
	return 0;
}
EXPORT_SYMBOL(tegra_mipi_calibration);

int tegra_mipi_select_mode(int mode)
{
	return 0;
}
EXPORT_SYMBOL(tegra_mipi_select_mode);
