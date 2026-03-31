/*
 * drivers/media/platform/tegra/mipical/mipi_cal_t124.c
 *
 * MIPI calibration platform driver for Tegra T124 (Tegra K1).
 *
 * Provides common MIPI calibration API for both camera (CSI) and
 * display (DSI) subsystems, following the T210 COMMON_MIPICAL_SUPPORTED
 * pattern. Replaces the separate dc/mipi_cal.c for T124.
 *
 * DSI calibration register values ported from:
 *   drivers/video/tegra/dc/dsi.c tegra_dsi_mipi_calibration_12x()
 *
 * Bias pad init ported from:
 *   drivers/media/platform/soc_camera/tegra_camera/vi2.c vi2_mipi_bias_pad_init()
 *
 * Copyright (c) 2025, Smoke Team. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <linux/device.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/export.h>

#include "mipi_cal.h"

/*
 * Register offsets for T124 MIPI calibration block at 0x700e3000.
 *
 * T124 has different CONFIG_2 register layout than T210:
 *   T124: DSI1 uses CILC(0x6c)/CILD(0x70), no CSIE_CONFIG_2
 *   T210: DSI1 uses DSIC(0x70)/DSID(0x74), has CSIE_CONFIG_2(0x74)
 *
 * Using explicit defines instead of mipi_cal_t21x.h to avoid
 * offset confusion between SoC generations.
 */

/* Control and status */
#define MIPI_CAL_CTRL			0x00
#define  CAL_NOISE_FLT(x)		(((x) & 0xf) << 26)
#define  CAL_PRESCALE(x)		(((x) & 0x3) << 24)
#define  CAL_CLKEN_OVR			(1 << 4)
#define  CAL_AUTOCAL_EN			(1 << 1)
#define  CAL_STARTCAL			(1 << 0)

#define CIL_MIPI_CAL_STATUS		0x08
#define  CAL_DONE_DSID			(1 << 31)
#define  CAL_DONE_DSIC			(1 << 30)
#define  CAL_DONE_DSIB			(1 << 29)
#define  CAL_DONE_DSIA			(1 << 28)
#define  CAL_DONE			(1 << 16)
#define  CAL_ACTIVE			(1 << 0)

/* CSI lane config (for deselection during DSI calibration) */
#define CILA_MIPI_CAL_CONFIG		0x14
#define CILB_MIPI_CAL_CONFIG		0x18
#define CILC_MIPI_CAL_CONFIG		0x1c
#define CILD_MIPI_CAL_CONFIG		0x20
#define CILE_MIPI_CAL_CONFIG		0x24
#define CILF_MIPI_CAL_CONFIG		0x28
#define  CIL_SEL			(1 << 21)

/* DSI lane config */
#define DSIA_MIPI_CAL_CONFIG		0x38
#define DSIB_MIPI_CAL_CONFIG		0x3c
#define DSIC_MIPI_CAL_CONFIG		0x40
#define DSID_MIPI_CAL_CONFIG		0x44
#define  DSI_SEL			(1 << 21)
#define  DSI_OVERIDE			(1 << 30)
#define  DSI_HSPDOS(x)			(((x) & 0x1f) << 16)
#define  DSI_HSPUOS(x)			(((x) & 0x1f) << 8)
#define  DSI_TERMOS(x)			(((x) & 0x1f) << 0)

/* Bias pad */
#define MIPI_BIAS_PAD_CFG0		0x58
#define  BIAS_PDVCLAMP			(1 << 1)
#define  BIAS_E_VCLAMP_REF		(1 << 0)

#define MIPI_BIAS_PAD_CFG1		0x5c
#define  BIAS_PAD_DRIV_UP_REF(x)	(((x) & 0x7) << 8)

#define MIPI_BIAS_PAD_CFG2		0x60
#define  BIAS_PAD_VCLAMP_LEVEL(x)	(((x) & 0x7) << 16)
#define  BIAS_PAD_VAUXP_LEVEL(x)	(((x) & 0x7) << 4)
#define  BIAS_PDVREG			(1 << 1)

/*
 * T124 CONFIG_2 registers — different from T210!
 * DSI instance 0: DSIA(0x64), DSIB(0x68)
 * DSI instance 1: CILC(0x6c), CILD(0x70)  ← T124-specific
 */
#define DSIA_MIPI_CAL_CONFIG_2		0x64
#define DSIB_MIPI_CAL_CONFIG_2		0x68
#define CILC_MIPI_CAL_CONFIG_2		0x6c	/* T124: DSI1 clock lane C */
#define CILD_MIPI_CAL_CONFIG_2		0x70	/* T124: DSI1 clock lane D */
#define CSIE_MIPI_CAL_CONFIG_2		0x74	/* T124: CSI-E clock lane */
#define  CFG2_CLKSEL			(1 << 21)
#define  CFG2_HSCLKPDOS(x)		(((x) & 0x1f) << 8)
#define  CFG2_HSCLKPUOS(x)		(((x) & 0x1f) << 0)

#define DRV_NAME "tegra_mipi_cal"
#define MIPI_CAL_TIMEOUT_MSEC		10

struct tegra_mipi {
	struct device *dev;
	struct clk *mipi_cal_clk;
	struct clk *mipi_cal_fixed;
	struct regmap *regmap;
	struct mutex lock;
	atomic_t refcount;
};

static const struct regmap_config t124_mipi_cal_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.cache_type = REGCACHE_NONE,
};

static struct tegra_mipi *get_mipi(void)
{
	struct device_node *np;
	struct platform_device *dev;
	struct tegra_mipi *mipi;

	np = of_find_node_by_name(NULL, "mipical");
	if (!np) {
		pr_err("%s: cannot find mipical node\n", __func__);
		return NULL;
	}
	dev = of_find_device_by_node(np);
	of_node_put(np);
	if (!dev) {
		pr_err("%s: cannot find mipical device\n", __func__);
		return NULL;
	}
	mipi = platform_get_drvdata(dev);
	if (!mipi)
		pr_err("%s: mipical not yet probed\n", __func__);

	return mipi;
}

static int tegra_mipi_clk_enable(struct tegra_mipi *mipi)
{
	int err;

	if (mipi->mipi_cal_fixed) {
		err = clk_prepare_enable(mipi->mipi_cal_fixed);
		if (err)
			return err;
	}
	err = clk_prepare_enable(mipi->mipi_cal_clk);
	if (err && mipi->mipi_cal_fixed)
		clk_disable_unprepare(mipi->mipi_cal_fixed);
	return err;
}

static void tegra_mipi_clk_disable(struct tegra_mipi *mipi)
{
	clk_disable_unprepare(mipi->mipi_cal_clk);
	if (mipi->mipi_cal_fixed)
		clk_disable_unprepare(mipi->mipi_cal_fixed);
}

/*
 * Apply T124 DSI production calibration settings.
 * Register values from dsi.c tegra_dsi_mipi_calibration_12x().
 */
static void t124_apply_dsi_prod(struct tegra_mipi *mipi, int lanes)
{
	/* BIAS_PAD_CFG1: PAD_DRIV_UP_REF = 3, preserve other fields */
	regmap_update_bits(mipi->regmap, MIPI_BIAS_PAD_CFG1,
			   BIAS_PAD_DRIV_UP_REF(0x7),
			   BIAS_PAD_DRIV_UP_REF(0x3));

	/* Deselect CSI clock lanes */
	regmap_write(mipi->regmap, CILC_MIPI_CAL_CONFIG_2, 0);
	regmap_write(mipi->regmap, CILD_MIPI_CAL_CONFIG_2, 0);

	/*
	 * DSI instance 0 (DSIA/DSIB):
	 *   CONFIG_0: SEL=1, OVERIDE=0, HSPDOS=0, HSPUOS=0, TERMOS=0
	 *   CONFIG_2: CLKSEL=1, HSCLKPDOS=1, HSCLKPUOS=2
	 */
	if (lanes & DSIA) {
		regmap_write(mipi->regmap, DSIA_MIPI_CAL_CONFIG, DSI_SEL);
		regmap_write(mipi->regmap, DSIB_MIPI_CAL_CONFIG, DSI_SEL);
		regmap_write(mipi->regmap, DSIA_MIPI_CAL_CONFIG_2,
			     CFG2_CLKSEL | CFG2_HSCLKPDOS(0x1) |
			     CFG2_HSCLKPUOS(0x2));
		regmap_write(mipi->regmap, DSIB_MIPI_CAL_CONFIG_2,
			     CFG2_CLKSEL | CFG2_HSCLKPDOS(0x1) |
			     CFG2_HSCLKPUOS(0x2));
	}

	/*
	 * DSI instance 1 (DSIC/DSID → CILC/CILD on T124):
	 *   CILC/CILD CONFIG_0: SEL=1
	 *   CILC/CILD CONFIG_2: CLKSEL=1, HSCLKPDOS=1, HSCLKPUOS=2
	 */
	if (lanes & DSIC) {
		regmap_write(mipi->regmap, CILC_MIPI_CAL_CONFIG, CIL_SEL);
		regmap_write(mipi->regmap, CILD_MIPI_CAL_CONFIG, CIL_SEL);
		regmap_write(mipi->regmap, CILC_MIPI_CAL_CONFIG_2,
			     CFG2_CLKSEL | CFG2_HSCLKPDOS(0x1) |
			     CFG2_HSCLKPUOS(0x2));
		regmap_write(mipi->regmap, CILD_MIPI_CAL_CONFIG_2,
			     CFG2_CLKSEL | CFG2_HSCLKPDOS(0x1) |
			     CFG2_HSCLKPUOS(0x2));
	}
}

static int _tegra_mipi_calibration(struct tegra_mipi *mipi, int lanes)
{
	int err = 0;
	u32 val;
	int retry = 500;

	mutex_lock(&mipi->lock);
	err = tegra_mipi_clk_enable(mipi);
	if (err)
		goto err_unlock;

	/*
	 * Exact port of vi2_mipi_calibration() from R21.5 vi2.c.
	 * Do NOT deviate from this sequence.
	 */

	/* 1. CLKEN_OVR */
	regmap_update_bits(mipi->regmap, MIPI_CAL_CTRL,
			   CAL_CLKEN_OVR, CAL_CLKEN_OVR);

	/* 2. Clear status */
	regmap_write(mipi->regmap, CIL_MIPI_CAL_STATUS, 0xF1F10000);

	/* 3. Deselect DSI */
	regmap_update_bits(mipi->regmap, DSIA_MIPI_CAL_CONFIG, DSI_SEL, 0);
	regmap_update_bits(mipi->regmap, DSIB_MIPI_CAL_CONFIG, DSI_SEL, 0);

	/* 4. Bias pad */
	regmap_update_bits(mipi->regmap, MIPI_BIAS_PAD_CFG0,
			   BIAS_E_VCLAMP_REF, BIAS_E_VCLAMP_REF);
	regmap_update_bits(mipi->regmap, MIPI_BIAS_PAD_CFG2,
			   BIAS_PDVREG, 0);

	/* 5. Deselect all CIL and clock lanes */
	regmap_update_bits(mipi->regmap, CILA_MIPI_CAL_CONFIG, CIL_SEL, 0);
	regmap_update_bits(mipi->regmap, DSIA_MIPI_CAL_CONFIG_2,
			   CFG2_CLKSEL, 0);
	regmap_update_bits(mipi->regmap, CILB_MIPI_CAL_CONFIG, CIL_SEL, 0);
	regmap_update_bits(mipi->regmap, DSIB_MIPI_CAL_CONFIG_2,
			   CFG2_CLKSEL, 0);
	regmap_update_bits(mipi->regmap, CILC_MIPI_CAL_CONFIG, CIL_SEL, 0);
	regmap_update_bits(mipi->regmap, CILC_MIPI_CAL_CONFIG_2,
			   CFG2_CLKSEL, 0);
	regmap_update_bits(mipi->regmap, CILD_MIPI_CAL_CONFIG, CIL_SEL, 0);
	regmap_update_bits(mipi->regmap, CILD_MIPI_CAL_CONFIG_2,
			   CFG2_CLKSEL, 0);
	regmap_update_bits(mipi->regmap, CILE_MIPI_CAL_CONFIG, CIL_SEL, 0);
	regmap_update_bits(mipi->regmap, CSIE_MIPI_CAL_CONFIG_2,
			   CFG2_CLKSEL, 0);

	/* 6. Select target lanes (from vi2.c switch(port)) */
	if (lanes & (CSIA | CSIB)) {
		/* CSI_A: CILA + optionally CILB for 4-lane */
		regmap_update_bits(mipi->regmap, CILA_MIPI_CAL_CONFIG,
				   CIL_SEL, CIL_SEL);
		regmap_update_bits(mipi->regmap, DSIA_MIPI_CAL_CONFIG_2,
				   CFG2_CLKSEL, 0);
		if (lanes & CSIB) {
			regmap_update_bits(mipi->regmap, CILB_MIPI_CAL_CONFIG,
					   CIL_SEL, CIL_SEL);
			regmap_update_bits(mipi->regmap, DSIB_MIPI_CAL_CONFIG_2,
					   CFG2_CLKSEL, 0);
		}
	}
	if (lanes & (CSIC | CSID)) {
		/* CSI_B: CILC + optionally CILD for 4-lane */
		regmap_update_bits(mipi->regmap, CILC_MIPI_CAL_CONFIG,
				   CIL_SEL, CIL_SEL);
		regmap_update_bits(mipi->regmap, CILC_MIPI_CAL_CONFIG_2,
				   CFG2_CLKSEL, 0);
		if (lanes & CSID) {
			regmap_update_bits(mipi->regmap, CILD_MIPI_CAL_CONFIG,
					   CIL_SEL, CIL_SEL);
			regmap_update_bits(mipi->regmap, CILD_MIPI_CAL_CONFIG_2,
					   CFG2_CLKSEL, 0);
		}
	}
	if (lanes & CSIE) {
		/* CSI_C: CILE with clock */
		regmap_update_bits(mipi->regmap, CILE_MIPI_CAL_CONFIG,
				   CIL_SEL, CIL_SEL);
		regmap_update_bits(mipi->regmap, CSIE_MIPI_CAL_CONFIG_2,
				   CFG2_CLKSEL, CFG2_CLKSEL);
	}

	/* DSI calibration */
	if (lanes & (DSIA | DSIB | DSIC | DSID))
		t124_apply_dsi_prod(mipi, lanes);

	/* 7. Trigger */
	regmap_update_bits(mipi->regmap, MIPI_CAL_CTRL,
			   CAL_STARTCAL, CAL_STARTCAL);

	/* 8. Wait — vi2 uses 500 retries at 200-300us */
	while (--retry) {
		regmap_read(mipi->regmap, CIL_MIPI_CAL_STATUS, &val);
		if (val & CAL_DONE)
			break;
		usleep_range(200, 300);
	}
	if (!retry) {
		dev_err(mipi->dev,
			"MIPI cal timeout, status: 0x%x, lanes: 0x%x\n",
			val, lanes);
		err = -ETIMEDOUT;
	}

	/* 9. Cleanup — restore clock lane selects (from vi2) */
	regmap_update_bits(mipi->regmap, CILA_MIPI_CAL_CONFIG, CIL_SEL, 0);
	regmap_update_bits(mipi->regmap, DSIA_MIPI_CAL_CONFIG_2,
			   CFG2_CLKSEL, CFG2_CLKSEL);
	regmap_update_bits(mipi->regmap, CILB_MIPI_CAL_CONFIG, CIL_SEL, 0);
	regmap_update_bits(mipi->regmap, DSIB_MIPI_CAL_CONFIG_2,
			   CFG2_CLKSEL, CFG2_CLKSEL);
	regmap_update_bits(mipi->regmap, CILC_MIPI_CAL_CONFIG, CIL_SEL, 0);
	regmap_update_bits(mipi->regmap, CILC_MIPI_CAL_CONFIG_2,
			   CFG2_CLKSEL, CFG2_CLKSEL);
	regmap_update_bits(mipi->regmap, CILD_MIPI_CAL_CONFIG, CIL_SEL, 0);
	regmap_update_bits(mipi->regmap, CILD_MIPI_CAL_CONFIG_2,
			   CFG2_CLKSEL, CFG2_CLKSEL);
	regmap_update_bits(mipi->regmap, CILE_MIPI_CAL_CONFIG, CIL_SEL, 0);
	regmap_update_bits(mipi->regmap, CSIE_MIPI_CAL_CONFIG_2,
			   CFG2_CLKSEL, 0);

	tegra_mipi_clk_disable(mipi);
err_unlock:
	mutex_unlock(&mipi->lock);
	return err;
}

int tegra_mipi_calibration(int lanes)
{
	struct tegra_mipi *mipi;

	mipi = get_mipi();
	if (!mipi)
		return -ENODEV;
	dev_info(mipi->dev, "%s lanes=0x%x\n", __func__, lanes);

	return _tegra_mipi_calibration(mipi, lanes);
}
EXPORT_SYMBOL(tegra_mipi_calibration);

int tegra_mipi_bias_pad_enable(void)
{
	struct tegra_mipi *mipi;
	int err;

	mipi = get_mipi();
	if (!mipi)
		return -EPROBE_DEFER;
	dev_dbg(mipi->dev, "%s\n", __func__);

	if (atomic_inc_return(&mipi->refcount) > 1)
		return 0;

	mutex_lock(&mipi->lock);
	err = tegra_mipi_clk_enable(mipi);
	if (err) {
		atomic_dec(&mipi->refcount);
		goto out;
	}

	regmap_update_bits(mipi->regmap, MIPI_BIAS_PAD_CFG2, BIAS_PDVREG, 0);
	regmap_update_bits(mipi->regmap, MIPI_BIAS_PAD_CFG0,
			   BIAS_E_VCLAMP_REF, 0);

	tegra_mipi_clk_disable(mipi);
out:
	mutex_unlock(&mipi->lock);
	return err;
}
EXPORT_SYMBOL(tegra_mipi_bias_pad_enable);

int tegra_mipi_bias_pad_disable(void)
{
	struct tegra_mipi *mipi;

	mipi = get_mipi();
	if (!mipi)
		return -ENODEV;
	dev_dbg(mipi->dev, "%s\n", __func__);

	if (atomic_dec_return(&mipi->refcount) > 0)
		return 0;

	mutex_lock(&mipi->lock);
	tegra_mipi_clk_enable(mipi);
	regmap_update_bits(mipi->regmap, MIPI_BIAS_PAD_CFG2,
			   BIAS_PDVREG, BIAS_PDVREG);
	tegra_mipi_clk_disable(mipi);
	mutex_unlock(&mipi->lock);

	return 0;
}
EXPORT_SYMBOL(tegra_mipi_bias_pad_disable);

int tegra_mipi_select_mode(int mode)
{
	return 0;
}
EXPORT_SYMBOL(tegra_mipi_select_mode);

static int tegra_mipi_probe(struct platform_device *pdev)
{
	struct tegra_mipi *mipi;
	struct resource *mem;
	void __iomem *regs;

	mipi = devm_kzalloc(&pdev->dev, sizeof(*mipi), GFP_KERNEL);
	if (!mipi)
		return -ENOMEM;

	mipi->dev = &pdev->dev;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(&pdev->dev, "no memory resource\n");
		return -EINVAL;
	}

	regs = devm_ioremap(&pdev->dev, mem->start, resource_size(mem));
	if (!regs) {
		dev_err(&pdev->dev, "ioremap failed\n");
		return -ENOMEM;
	}

	mipi->regmap = devm_regmap_init_mmio(&pdev->dev, regs,
					     &t124_mipi_cal_regmap_config);
	if (IS_ERR(mipi->regmap)) {
		dev_err(&pdev->dev, "regmap init failed\n");
		return PTR_ERR(mipi->regmap);
	}

	/*
	 * T124 mipical DT node has no clock properties (legacy clock
	 * framework). Use clk_get_sys() as vi2.c does.
	 */
	mipi->mipi_cal_clk = clk_get_sys("mipi-cal", NULL);
	if (IS_ERR_OR_NULL(mipi->mipi_cal_clk)) {
		dev_err(&pdev->dev, "cannot get mipi-cal clock\n");
		return mipi->mipi_cal_clk ? PTR_ERR(mipi->mipi_cal_clk)
					  : -ENODEV;
	}

	mipi->mipi_cal_fixed = clk_get_sys("clk72mhz", NULL);
	if (IS_ERR(mipi->mipi_cal_fixed)) {
		dev_warn(&pdev->dev, "clk72mhz not found, trying mipi-cal-fixed\n");
		mipi->mipi_cal_fixed = clk_get_sys("mipi-cal-fixed", NULL);
	}
	if (IS_ERR(mipi->mipi_cal_fixed)) {
		dev_warn(&pdev->dev, "cannot get mipi-cal-fixed clock, proceeding without\n");
		mipi->mipi_cal_fixed = NULL;
	}

	mutex_init(&mipi->lock);
	atomic_set(&mipi->refcount, 0);
	platform_set_drvdata(pdev, mipi);

	dev_info(&pdev->dev, "T124 MIPI calibration driver probed\n");
	return 0;
}

static int tegra_mipi_remove(struct platform_device *pdev)
{
	struct tegra_mipi *mipi = platform_get_drvdata(pdev);

	if (mipi->mipi_cal_fixed)
		clk_put(mipi->mipi_cal_fixed);
	if (mipi->mipi_cal_clk)
		clk_put(mipi->mipi_cal_clk);

	return 0;
}

static const struct of_device_id tegra_mipi_of_match[] = {
	{ .compatible = "nvidia,tegra124-mipical" },
};
MODULE_DEVICE_TABLE(of, tegra_mipi_of_match);

static struct platform_driver tegra_mipi_cal_platform_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = tegra_mipi_of_match,
	},
	.probe = tegra_mipi_probe,
	.remove = tegra_mipi_remove,
};

static int __init tegra_mipi_module_init(void)
{
	return platform_driver_register(&tegra_mipi_cal_platform_driver);
}

static void __exit tegra_mipi_module_exit(void)
{
	platform_driver_unregister(&tegra_mipi_cal_platform_driver);
}

subsys_initcall(tegra_mipi_module_init);
module_exit(tegra_mipi_module_exit);

MODULE_AUTHOR("Artem Bambalov <artembambalov1993@gmail.com>");
MODULE_DESCRIPTION("Common MIPI calibration driver for Tegra T124");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRV_NAME);
