/*
 * drivers/misc/mipi_cal.h
 *
 * Copyright (c) 2016, NVIDIA CORPORATION, All rights reserved.
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

#ifndef MIPI_CAL_H
#define MIPI_CAL_H

#define DSID	(1 << 31)
#define DSIC	(1 << 30)
#define DSIB	(1 << 29)
#define DSIA	(1 << 28)
#define CSIF	(1 << 25)
#define CSIE	(1 << 24)
#define CSID	(1 << 23)
#define CSIC	(1 << 22)
#define CSIB	(1 << 21)
#define CSIA	(1 << 20)

#if defined(CONFIG_ARCH_TEGRA_21x_SOC)
extern int tegra_mipi_bias_pad_enable(void);
extern int tegra_mipi_bias_pad_disable(void);
extern int tegra_mipi_calibration(int lanes);
extern int tegra_mipi_select_mode(int mode);
#elif defined(CONFIG_ARCH_TEGRA_12x_SOC)
/*
 * T124 MIPI bias pad initialization.
 *
 * On T124 there is no runtime calibration like T210 — the bias pad
 * just needs two bits cleared once to enable the MIPI receiver.
 * Reference: vi2.c vi2_mipi_bias_pad_init() which does the same
 * via regmap on the MIPI_CAL block at 0x700e3000.
 *
 * We use direct ioremap here to avoid pulling in regmap dependencies
 * into the header. The writes are idempotent so calling multiple
 * times is safe.
 */
#include <linux/io.h>
#include <linux/clk.h>

#define TEGRA_T124_MIPI_CAL_BASE	0x700e3000
#define TEGRA_T124_MIPI_BIAS_PAD_CFG0	0x58
#define TEGRA_T124_MIPI_BIAS_PAD_CFG2	0x60
#define TEGRA_T124_E_VCLAMP_REF		(1 << 0)
#define TEGRA_T124_PDVREG		(1 << 1)

static inline int tegra_mipi_bias_pad_enable(void)
{
	void __iomem *base;
	struct clk *clk;
	u32 val;

	clk = clk_get_sys("mipi-cal", NULL);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	base = ioremap(TEGRA_T124_MIPI_CAL_BASE, 0x100);
	if (!base) {
		clk_put(clk);
		return -ENOMEM;
	}

	clk_prepare_enable(clk);

	/* Clear E_VCLAMP_REF in MIPI_BIAS_PAD_CFG0 */
	val = readl(base + TEGRA_T124_MIPI_BIAS_PAD_CFG0);
	val &= ~TEGRA_T124_E_VCLAMP_REF;
	writel(val, base + TEGRA_T124_MIPI_BIAS_PAD_CFG0);

	/* Clear PDVREG in MIPI_BIAS_PAD_CFG2 */
	val = readl(base + TEGRA_T124_MIPI_BIAS_PAD_CFG2);
	val &= ~TEGRA_T124_PDVREG;
	writel(val, base + TEGRA_T124_MIPI_BIAS_PAD_CFG2);

	clk_disable_unprepare(clk);
	clk_put(clk);
	iounmap(base);

	return 0;
}

static inline int tegra_mipi_bias_pad_disable(void) { return 0; }
static inline int tegra_mipi_calibration(int lanes) { return 0; }
static inline int tegra_mipi_select_mode(int mode) { return 0; }
#else
static inline int tegra_mipi_bias_pad_enable(void) { return -ENOSYS; }
static inline int tegra_mipi_bias_pad_disable(void) { return -ENOSYS; }
static inline int tegra_mipi_calibration(int lanes) { return -ENOSYS; }
static inline int tegra_mipi_select_mode(int mode) { return -ENOSYS; }
#endif

#endif
