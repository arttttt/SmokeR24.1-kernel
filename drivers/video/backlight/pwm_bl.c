/*
 * linux/drivers/video/backlight/pwm_bl.c
 *
 * Copyright (c) 2013-2015, NVIDIA CORPORATION, All rights reserved.
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
 * simple PWM based backlight control, board code has to setup
 * 1) pin configuration so PWM waveforms can output
 * 2) platform_data being correctly configured
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/pwm.h>
#include <linux/pwm_backlight.h>
#include <linux/slab.h>
#include "../../../arch/arm/mach-tegra/board.h"

static int pwm_backlight_set(struct backlight_device *bl, int brightness)
{
	struct pwm_bl_data *pb = bl_get_data(bl);
	int max = bl->props.max_brightness;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.fb_blank != FB_BLANK_UNBLANK ||
	    bl->props.state & BL_CORE_FBBLANK)
		brightness = 0;

	if (pb->notify)
		brightness = pb->notify(pb->dev, brightness);

	if (brightness == 0) {
		pwm_config(pb->pwm, 0, pb->period);
		pwm_disable(pb->pwm);
	} else {
		int duty_cycle;

		if (pb->levels) {
			duty_cycle = pb->levels[brightness];
			max = pb->levels[max];
		} else {
			duty_cycle = brightness;
		}

		duty_cycle = pb->lth_brightness +
		     (duty_cycle * (pb->period - pb->lth_brightness) / max);
		pwm_config(pb->pwm, duty_cycle, pb->period);
		pwm_enable(pb->pwm);
	}

	if (pb->notify_after)
		pb->notify_after(pb->dev, brightness);

	return 0;
}

static int pwm_backlight_update_status(struct backlight_device *bl)
{
	int brightness = bl->props.brightness;
	return pwm_backlight_set(bl, brightness);
}

static int pwm_backlight_get_brightness(struct backlight_device *bl)
{
	return bl->props.brightness;
}

static int pwm_backlight_check_fb(struct backlight_device *bl,
				  struct fb_info *info)
{
	struct pwm_bl_data *pb = bl_get_data(bl);

	return !pb->check_fb || pb->check_fb(pb->dev, info);
}

static const struct backlight_ops pwm_backlight_ops = {
	.update_status	= pwm_backlight_update_status,
	.get_brightness	= pwm_backlight_get_brightness,
	.check_fb	= pwm_backlight_check_fb,
};

#ifdef CONFIG_OF
static int pwm_backlight_parse_dt(struct device *dev,
				  struct platform_pwm_backlight_data *data,
				  const char *blnode_compatible,
				  struct device_node **target_bl_node)
{
	struct device_node *node = dev->of_node;
	struct device_node *bl_node = NULL;
	struct device_node *compat_node = NULL;
	struct property *prop;
	const __be32 *p;
	u32 u;
	int length;
	u32 value;
	int ret = 0;
	int n_bl_nonlinear = 0;
	int n_bl_measured = 0;

	if (!node)
		return -ENODEV;

	/* If there's compat_node which is contained in
	 * backlight parent node, that means, there are
	 * multi pwm-bl device nodes and right one is
	 * chosen, with blnode_compatible */
	if (blnode_compatible)
		compat_node = of_find_compatible_node(node, NULL,
			blnode_compatible);

	if (!blnode_compatible || !compat_node)
		bl_node = node;
	else
		bl_node = compat_node;

	*target_bl_node = bl_node;

	/* determine the number of brightness levels */
	prop = of_find_property(bl_node, "brightness-levels", &length);
	if (!prop) {
		/* if brightness levels array is not defined,
		 * parse max brightness and default brightness,
		 * directly.
		 */
		ret = of_property_read_u32(bl_node, "max-brightness",
					   &value);
		if (ret < 0) {
			pr_info("fail to parse max-brightness\n");
			goto fail_parse_dt;
		}

		data->max_brightness = value;

#ifdef CONFIG_ANDROID
		if (get_androidboot_mode_charger())
			ret = of_property_read_u32(bl_node,
						   "default-charge-brightness",
						   &value);
		else
#endif
		ret = of_property_read_u32(bl_node, "default-brightness",
					   &value);
		if (ret < 0) {
			pr_info("fail to parse default-brightness\n");
			goto fail_parse_dt;
		}

		data->dft_brightness = value;
	} else {
		size_t size = 0;
		int item_counts;
		item_counts = length / sizeof(u32);
		if (item_counts > 0)
			size = sizeof(*data->levels) * item_counts;

		data->levels = devm_kzalloc(dev, size, GFP_KERNEL);
		if (!data->levels)
			ret = -ENOMEM;
		goto fail_parse_dt;

		ret = of_property_read_u32_array(bl_node,
						 "brightness-levels",
						 data->levels,
						 item_counts);
		if (ret < 0) {
			pr_info("fail to parse brightness-levels\n");
			goto fail_parse_dt;
		}

		/*
		 * default-brightness-level: the default brightness level
		 * (index into the array defined by the "brightness-levels"
		 * property)
		 */
		ret = of_property_read_u32(bl_node,
					   "default-brightness-level",
					   &value);
		if (ret < 0) {
			pr_info("fail to parse default-brightness-level\n");
			goto fail_parse_dt;
		}

		if (value >= item_counts) {
			dev_warn(dev, "invalid default brightness level: %u, use %u\n",
				value, item_counts - 1);
			value = item_counts - 1;
		}

		data->dft_brightness = data->levels[value];
		data->max_brightness = data->levels[item_counts - 1];
	}

	value = 0;
	ret = of_property_read_u32(bl_node, "lth-brightness",
		&value);
	data->lth_brightness = (unsigned int)value;

	data->pwm_gpio = of_get_named_gpio(bl_node, "pwm-gpio", 0);

	of_property_for_each_u32(bl_node, "bl-nonlinear", prop, p, u)
		n_bl_nonlinear++;
	if (n_bl_nonlinear > 0) {
		data->bl_nonlinear = devm_kzalloc(dev,
			sizeof(*data->bl_nonlinear) * n_bl_nonlinear,
			GFP_KERNEL);
		if (!data->bl_nonlinear) {
			pr_err("bl_nonlinear memory allocation failed\n");
			ret = -ENOMEM;
			goto fail_parse_dt;
		}
		n_bl_nonlinear = 0;
		of_property_for_each_u32(bl_node,
			"bl-nonlinear", prop, p, u)
			data->bl_nonlinear[n_bl_nonlinear++] = u;
	}

	of_property_for_each_u32(bl_node, "bl-measured", prop, p, u)
		n_bl_measured++;
	if (n_bl_measured > 0) {
		data->bl_measured = devm_kzalloc(dev,
			sizeof(*data->bl_measured) * n_bl_measured, GFP_KERNEL);
		if (!data->bl_measured) {
			pr_err("bl_measured memory allocation failed\n");
			ret = -ENOMEM;
			goto fail_parse_dt;
		}
		n_bl_measured = 0;
		of_property_for_each_u32(bl_node,
			"bl-measured", prop, p, u)
			data->bl_measured[n_bl_measured++] = u;
	}
	of_node_put(compat_node);
	return 0;

fail_parse_dt:
	of_node_put(compat_node);
	return ret;
}

static struct of_device_id pwm_backlight_of_match[] = {
	{ .compatible = "pwm-backlight" },
	{ }
};

MODULE_DEVICE_TABLE(of, pwm_backlight_of_match);
#else
static int pwm_backlight_parse_dt(struct device *dev,
				  struct platform_pwm_backlight_data *data,
				  const char *blnode_compatible,
				  struct device_node **target_bl_node)
{
	return -ENODEV;
}
#endif

static int pwm_backlight_probe(struct platform_device *pdev)
{
	struct platform_pwm_backlight_data *data = pdev->dev.platform_data;
	struct device_node *np = pdev->dev.of_node;
	struct platform_pwm_backlight_data defdata;
	struct backlight_properties props;
	struct backlight_device *bl;
	struct pwm_bl_data *pb;
	struct device_node *target_bl_node = NULL;
	unsigned int max;
	int ret;
	const char *blnode_compatible = NULL;

	if (!np && !pdev->dev.platform_data) {
		dev_err(&pdev->dev, "no platform data for pwm_bl\n");
		return -ENOENT;
	}

	if (np) {
		struct pwm_bl_data_dt_ops *pops;
		pops = (struct pwm_bl_data_dt_ops *)platform_get_drvdata(pdev);
		memset(&defdata, 0, sizeof(defdata));
		if (pops) {
			defdata.init = pops->init;
			defdata.notify = pops->notify;
			defdata.notify_after = pops->notify_after;
			defdata.check_fb = pops->check_fb;
			defdata.exit = pops->exit;
			blnode_compatible = pops->blnode_compatible;
		}
		ret = pwm_backlight_parse_dt(&pdev->dev, &defdata,
			blnode_compatible, &target_bl_node);
		if (ret < 0) {
			dev_err(&pdev->dev, "fail to find platform data\n");
			return ret;
		}
		data = &defdata;

		/* initialize dev drv data */
		platform_set_drvdata(pdev, NULL);
	}

	if (data->init) {
		ret = data->init(&pdev->dev);
		if (ret < 0)
			return ret;
	}

	pb = devm_kzalloc(&pdev->dev, sizeof(*pb), GFP_KERNEL);
	if (!pb) {
		dev_err(&pdev->dev, "no memory for state\n");
		ret = -ENOMEM;
		goto err_alloc;
	}

	if (data->levels) {
		max = data->levels[data->max_brightness];
		pb->levels = data->levels;
	} else
		max = data->max_brightness;

	pb->notify = data->notify;
	pb->notify_after = data->notify_after;
	pb->bl_nonlinear = data->bl_nonlinear;
	pb->bl_measured = data->bl_measured;
	pb->check_fb = data->check_fb;
	pb->exit = data->exit;
	pb->dev = &pdev->dev;
	pb->pwm_gpio = data->pwm_gpio;
	/*
	 * For DT case, devm_pwm_get will finally call of_pwm_get.
	 * It is not necessary to parse data->pwm_id value from separate
	 * device tree property since in of_pwm_get, we will use 1st argument
	 * of pwms property for pwm_id, global PWM device index.
	 */

	pb->pwm = devm_pwm_get(&pdev->dev, NULL);
	if (IS_ERR(pb->pwm)) {
		dev_info(&pdev->dev,
			"PWM request fail by devm_pwm_get, trying of_pwm_get\n");
		pb->pwm = of_pwm_get(target_bl_node, NULL);
		if (IS_ERR(pb->pwm)) {
			dev_info(&pdev->dev, "Trying PWM req with legacy API\n");
			pb->pwm = pwm_request(data->pwm_id, "pwm-backlight");
			if (IS_ERR(pb->pwm)) {
				dev_err(&pdev->dev,
					"unable to request legacy PWM\n");
				ret = PTR_ERR(pb->pwm);
				goto err_alloc;
			}
		}
	}

	dev_dbg(&pdev->dev, "got pwm for backlight\n");

	/*
	 * The DT case will not set pwm_period_ns. Instead, it stores the
	 * period, parsed from the DT, in the PWM device. In other words,
	 * the 2nd argument of pwms property indicates pwm_period in
	 * nonoseconds. For the non-DT case, set the period from
	 * platform data.
	 */
	if (data->pwm_period_ns > 0)
		pwm_set_period(pb->pwm, data->pwm_period_ns);

	pb->period = pwm_get_period(pb->pwm);
	pb->lth_brightness = data->lth_brightness * (pb->period / max);

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = data->max_brightness;

	if (gpio_is_valid(pb->pwm_gpio)) {
		ret = gpio_request(pb->pwm_gpio, "disp_bl");
		if (ret)
			dev_err(&pdev->dev, "backlight gpio request failed\n");
	}

	bl = backlight_device_register(dev_name(&pdev->dev), &pdev->dev, pb,
				       &pwm_backlight_ops, &props);
	if (IS_ERR(bl)) {
		dev_err(&pdev->dev, "failed to register backlight\n");
		ret = PTR_ERR(bl);
		goto err_alloc;
	}

	if (data->dft_brightness > data->max_brightness) {
		dev_warn(&pdev->dev,
			 "invalid dft brightness: %u, using max one %u\n",
			 data->dft_brightness, data->max_brightness);
		data->dft_brightness = data->max_brightness;
	}

	platform_set_drvdata(pdev, bl);
	bl->props.brightness = data->dft_brightness;
	backlight_update_status(bl);

	if (gpio_is_valid(pb->pwm_gpio))
		gpio_free(pb->pwm_gpio);

	return 0;

err_alloc:
	if (data->exit)
		data->exit(&pdev->dev);
	return ret;
}

static int pwm_backlight_remove(struct platform_device *pdev)
{
	struct backlight_device *bl = platform_get_drvdata(pdev);
	struct pwm_bl_data *pb = bl_get_data(bl);

	backlight_device_unregister(bl);
	pwm_config(pb->pwm, 0, pb->period);
	pwm_disable(pb->pwm);
	if (pb->exit)
		pb->exit(&pdev->dev);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int pwm_backlight_suspend(struct device *dev)
{
	struct backlight_device *bl = dev_get_drvdata(dev);

	return pwm_backlight_set(bl, 0);
}

static int pwm_backlight_resume(struct device *dev)
{
	struct backlight_device *bl = dev_get_drvdata(dev);

	backlight_update_status(bl);
	return 0;
}

static SIMPLE_DEV_PM_OPS(pwm_backlight_pm_ops, pwm_backlight_suspend,
			 pwm_backlight_resume);

#endif

static struct platform_driver pwm_backlight_driver = {
	.driver		= {
		.name		= "pwm-backlight",
		.owner		= THIS_MODULE,
#ifdef CONFIG_PM_SLEEP
		.pm		= &pwm_backlight_pm_ops,
#endif
		.of_match_table	= of_match_ptr(pwm_backlight_of_match),
	},
	.probe		= pwm_backlight_probe,
	.remove		= pwm_backlight_remove,
};

module_platform_driver(pwm_backlight_driver);

MODULE_DESCRIPTION("PWM based Backlight Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:pwm-backlight");

