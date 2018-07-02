/*
 * arch/arm/mach-tegra/panel-s-wqxga-7-9-x6.c
 *
 * Copyright (c) 2014, XIAOMI CORPORATION. All rights reserved.
 * Copyright (C) 2016 XiaoMi, Inc.
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
 * along with this program.  If not, see <http:
 */

#include <mach/dc.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/tegra_pwm_bl.h>
#include <linux/regulator/consumer.h>
#include <linux/pwm_backlight.h>
#include <linux/max8831_backlight.h>
#include <linux/platform_data/lp855x.h>
#include <linux/leds.h>
#include <linux/ioport.h>
#include <generated/mach-types.h>
#include "board.h"
#include "board-panel.h"
#include "devices.h"
#include "gpio-names.h"
#include "tegra11_host1x_devices.h"

struct dsi_s_wqxga_7_9_regulator {
	char *reg_name;
	struct regulator *reg;
};

static struct dsi_s_wqxga_7_9_data {
	struct dsi_s_wqxga_7_9_regulator avdd_lcd_vsp_5v5;
	struct dsi_s_wqxga_7_9_regulator avdd_lcd_vsn_5v5;
	struct dsi_s_wqxga_7_9_regulator dvdd_lcd_1v8;
	bool reg_requested;
	int rst_gpio;
} wqxga_7_9_mocha = {
	.avdd_lcd_vsp_5v5 = {
		.reg_name = "avdd_lcd",
	},
	.avdd_lcd_vsn_5v5 = {
		.reg_name = "bvdd_lcd",
	},
	.dvdd_lcd_1v8 = {
		.reg_name = "dvdd_lcdio",
	},
	.rst_gpio = TEGRA_GPIO_PH3,
};

static int ardbeg_dsi_regulator_get(struct device *dev)
{
	int err = 0;
	char *reg_name;
	struct regulator *reg;

	if (wqxga_7_9_mocha.reg_requested)
		return 0;

	reg_name = wqxga_7_9_mocha.dvdd_lcd_1v8.reg_name;
	reg = regulator_get(dev, reg_name);
	if (IS_ERR_OR_NULL(reg)) {
		err = PTR_ERR(reg);
		reg = NULL;
	}
	wqxga_7_9_mocha.dvdd_lcd_1v8.reg = reg;

	reg_name = wqxga_7_9_mocha.avdd_lcd_vsp_5v5.reg_name;
	reg = regulator_get(dev, reg_name);
	if (IS_ERR_OR_NULL(reg)) {
		pr_err("avdd_lcd regulator get failed\n");
		err = PTR_ERR(reg);
		reg = NULL;
	}
	wqxga_7_9_mocha.avdd_lcd_vsp_5v5.reg = reg;

	reg_name = wqxga_7_9_mocha.avdd_lcd_vsn_5v5.reg_name;
	reg = regulator_get(dev, reg_name);
	if (IS_ERR_OR_NULL(reg)) {
		pr_err("avdd_lcd regulator get failed\n");
		err = PTR_ERR(reg);
		reg = NULL;
	}
	wqxga_7_9_mocha.avdd_lcd_vsn_5v5.reg = reg;

	wqxga_7_9_mocha.reg_requested = true;
	return 0;
}

static int dsi_s_wqxga_7_9_postpoweron(struct device *dev)
{
	return 0;
}

static int dsi_s_wqxga_7_9_enable(struct device *dev)
{
	int err = 0;

	err = ardbeg_dsi_regulator_get(dev);
	if (err < 0) {
		pr_err("dsi regulator get failed\n");
	}

	err = gpio_request(wqxga_7_9_mocha.rst_gpio, NULL);
	if (err < 0) {
		pr_err("dsi gpio request failed\n");
	}

	if (wqxga_7_9_mocha.dvdd_lcd_1v8.reg) {
		err = regulator_enable(wqxga_7_9_mocha.dvdd_lcd_1v8.reg);
		if (err < 0) {
			pr_err("dvdd_lcd regulator enable failed\n");
		}
		msleep(12);
	}

	if (wqxga_7_9_mocha.avdd_lcd_vsp_5v5.reg) {
		err = regulator_enable(wqxga_7_9_mocha.avdd_lcd_vsp_5v5.reg);
		if (err < 0) {
			pr_err("avdd_lcd regulator enable failed\n");
		}
		msleep(12);
	}

	if (wqxga_7_9_mocha.avdd_lcd_vsn_5v5.reg) {
		err = regulator_enable(wqxga_7_9_mocha.avdd_lcd_vsn_5v5.reg);
		if (err < 0) {
			pr_err("bvdd_lcd regulator enable failed\n");
		}
	}

	gpio_direction_output(wqxga_7_9_mocha.rst_gpio, 1);
	msleep(10);

	return 0;
}

static int dsi_s_wqxga_7_9_disable(struct device *dev)
{
	int err = 0;
	
	err = gpio_request(wqxga_7_9_mocha.rst_gpio, NULL);
	if (err < 0) {
		pr_err("dsi gpio request failed\n");
	}

	if (gpio_get_value(wqxga_7_9_mocha.rst_gpio) != 0)
		gpio_set_value(wqxga_7_9_mocha.rst_gpio, 0);

	gpio_free(wqxga_7_9_mocha.rst_gpio);

	msleep(10);
	if (wqxga_7_9_mocha.avdd_lcd_vsn_5v5.reg)
		regulator_disable(wqxga_7_9_mocha.avdd_lcd_vsn_5v5.reg);
	msleep(10);
	if (wqxga_7_9_mocha.avdd_lcd_vsp_5v5.reg)
		regulator_disable(wqxga_7_9_mocha.avdd_lcd_vsp_5v5.reg);
	msleep(10);
	if (wqxga_7_9_mocha.dvdd_lcd_1v8.reg)
		regulator_disable(wqxga_7_9_mocha.dvdd_lcd_1v8.reg);

	return 0;
}

static int dsi_s_wqxga_7_9_postsuspend(void)
{
	int err = 0;
	err = gpio_request(wqxga_7_9_mocha.rst_gpio, NULL);
	if (err < 0) {
		pr_err("dsi gpio request failed\n");
	}

	if (gpio_get_value(wqxga_7_9_mocha.rst_gpio) != 0)
		gpio_set_value(wqxga_7_9_mocha.rst_gpio, 0);

	gpio_free(wqxga_7_9_mocha.rst_gpio);

	return 0;
}

struct tegra_panel_ops dsi_s_wqxga_7_9_x6_ops = {
	.enable = dsi_s_wqxga_7_9_enable,
	.postpoweron = dsi_s_wqxga_7_9_postpoweron,
	.postsuspend = dsi_s_wqxga_7_9_postsuspend,
	.disable = dsi_s_wqxga_7_9_disable,
};
