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

#define TEGRA_DSI_GANGED_MODE	1

#define DSI_PANEL_RESET		1

#define DC_CTRL_MODE	(TEGRA_DC_OUT_CONTINUOUS_MODE | \
						TEGRA_DC_OUT_INITIALIZED_MODE)

static bool reg_requested;
static struct regulator *avdd_lcd_vsp_5v5;
static struct regulator *avdd_lcd_vsn_5v5;
static struct regulator *dvdd_lcd_1v8;
static u16 en_panel_rst;

static int ardbeg_dsi_regulator_get(struct device *dev)
{
	int err = 0;

	if (reg_requested)
		return 0;
	dvdd_lcd_1v8 = regulator_get(dev, "dvdd_lcdio");
	if (IS_ERR_OR_NULL(dvdd_lcd_1v8)) {
		err = PTR_ERR(dvdd_lcd_1v8);
		dvdd_lcd_1v8 = NULL;
	}
	avdd_lcd_vsp_5v5 = regulator_get(dev, "avdd_lcd");
	if (IS_ERR_OR_NULL(avdd_lcd_vsp_5v5)) {
		pr_err("avdd_lcd regulator get failed\n");
		err = PTR_ERR(avdd_lcd_vsp_5v5);
		avdd_lcd_vsp_5v5 = NULL;
	}
	avdd_lcd_vsn_5v5 = regulator_get(dev, "bvdd_lcd");
	if (IS_ERR_OR_NULL(avdd_lcd_vsn_5v5)) {
		pr_err("avdd_lcd regulator get failed\n");
		err = PTR_ERR(avdd_lcd_vsn_5v5);
		avdd_lcd_vsn_5v5 = NULL;
	}

	reg_requested = true;
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

	err = tegra_panel_gpio_get_dt("s,wqxga-7-9-x6", &panel_of);
	if (err < 0) {
		pr_err("dsi gpio request failed\n");
	}

	if (gpio_is_valid(panel_of.panel_gpio[TEGRA_GPIO_RESET]))
		en_panel_rst = panel_of.panel_gpio[TEGRA_GPIO_RESET];

	if (dvdd_lcd_1v8) {
		err = regulator_enable(dvdd_lcd_1v8);
		if (err < 0) {
			pr_err("dvdd_lcd regulator enable failed\n");
		}
		msleep(12);
	}

	if (avdd_lcd_vsp_5v5) {
		err = regulator_enable(avdd_lcd_vsp_5v5);
		if (err < 0) {
			pr_err("avdd_lcd regulator enable failed\n");
		}
		msleep(12);
	}

	if (avdd_lcd_vsn_5v5) {
		err = regulator_enable(avdd_lcd_vsn_5v5);
		if (err < 0) {
			pr_err("bvdd_lcd regulator enable failed\n");
		}
	}

#if DSI_PANEL_RESET
	pr_err("panel: gpio value %d\n", gpio_get_value(en_panel_rst));
	if (gpio_get_value(en_panel_rst) == 0) {
		pr_info("panel: %s\n", __func__);
		gpio_direction_output(en_panel_rst, 1);
		usleep_range(1000, 3000);
		gpio_set_value(en_panel_rst, 0);
		usleep_range(1000, 3000);
		gpio_set_value(en_panel_rst, 1);
		msleep(32);
	}
#endif
	return 0;
}

static int dsi_s_wqxga_7_9_disable(struct device *dev)
{
	int err = 0;
	
	err = tegra_panel_gpio_get_dt("s,wqxga-7-9-x6", &panel_of);
	if (err < 0) {
		pr_err("dsi gpio request failed\n");
	}

	/* If panel rst gpio is specified in device tree,
	 * use that.
	 */
	if (gpio_is_valid(panel_of.panel_gpio[TEGRA_GPIO_RESET]))
		en_panel_rst = panel_of.panel_gpio[TEGRA_GPIO_RESET];
		
	pr_info("panel: %s\n", __func__);
	gpio_set_value(en_panel_rst, 0);
	msleep(10);
	if (avdd_lcd_vsn_5v5)
		regulator_disable(avdd_lcd_vsn_5v5);
	msleep(10);
	if (avdd_lcd_vsp_5v5)
		regulator_disable(avdd_lcd_vsp_5v5);
	msleep(10);
	if (dvdd_lcd_1v8)
		regulator_disable(dvdd_lcd_1v8);

	return 0;
}

static int dsi_s_wqxga_7_9_postsuspend(void)
{
	int err = 0;
	err = tegra_panel_gpio_get_dt("s,wqxga-7-9-x6", &panel_of);
	if (err < 0) {
		pr_err("dsi gpio request failed\n");
	}

	/* If panel rst gpio is specified in device tree,
	 * use that.
	 */
	if (gpio_is_valid(panel_of.panel_gpio[TEGRA_GPIO_RESET]))
		en_panel_rst = panel_of.panel_gpio[TEGRA_GPIO_RESET];
		
	pr_info("%s\n", __func__);
	gpio_set_value(en_panel_rst, 0);
	return 0;
}

struct tegra_panel_ops dsi_s_wqxga_7_9_x6_ops = {
	.enable = dsi_s_wqxga_7_9_enable,
	.postpoweron = dsi_s_wqxga_7_9_postpoweron,
	.postsuspend = dsi_s_wqxga_7_9_postsuspend,
	.disable = dsi_s_wqxga_7_9_disable,
};
