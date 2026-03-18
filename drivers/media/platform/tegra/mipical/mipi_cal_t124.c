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
 * soc_camera framework, which performs the same register writes
 * via the same regmap API.
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
#include <linux/regmap.h>

#include "mipi_cal.h"

#define MIPI_CAL_BASE			0x700e3000
#define MIPI_CAL_SIZE			0x100

#define MIPI_BIAS_PAD_CFG0		0x58
#define  E_VCLAMP_REF			(1 << 0)

#define MIPI_BIAS_PAD_CFG2		0x60
#define  PDVREG				(1 << 1)

static const struct regmap_config t124_mipi_cal_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.cache_type = REGCACHE_NONE,
	.fast_io = 1,
};

static DEFINE_MUTEX(t124_mipi_lock);
static struct regmap *t124_mipi_regmap;

int tegra_mipi_bias_pad_enable(void)
{
	void __iomem *base;
	struct clk *clk;
	int ret = 0;

	mutex_lock(&t124_mipi_lock);

	if (t124_mipi_regmap)
		goto do_enable;

	base = ioremap(MIPI_CAL_BASE, MIPI_CAL_SIZE);
	if (!base) {
		ret = -ENOMEM;
		goto out;
	}

	t124_mipi_regmap = regmap_init_mmio(NULL, base,
					    &t124_mipi_cal_regmap_config);
	if (IS_ERR(t124_mipi_regmap)) {
		ret = PTR_ERR(t124_mipi_regmap);
		t124_mipi_regmap = NULL;
		iounmap(base);
		pr_err("%s: regmap init failed: %d\n", __func__, ret);
		goto out;
	}

do_enable:
	clk = clk_get_sys("mipi-cal", NULL);
	if (IS_ERR(clk)) {
		ret = PTR_ERR(clk);
		pr_err("%s: cannot get mipi-cal clk: %d\n", __func__, ret);
		goto out;
	}

	clk_prepare_enable(clk);

	/* Clear E_VCLAMP_REF in MIPI_BIAS_PAD_CFG0 */
	regmap_update_bits(t124_mipi_regmap, MIPI_BIAS_PAD_CFG0,
			   E_VCLAMP_REF, 0);
	/* Clear PDVREG in MIPI_BIAS_PAD_CFG2 */
	regmap_update_bits(t124_mipi_regmap, MIPI_BIAS_PAD_CFG2,
			   PDVREG, 0);

	clk_disable_unprepare(clk);
	clk_put(clk);
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
