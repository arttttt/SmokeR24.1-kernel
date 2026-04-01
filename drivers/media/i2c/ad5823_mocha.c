/*
 * Copyright (c) 2025, Smoke Team.  All rights reserved.
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
 * V4L2 subdev focuser driver for AD5823 VCM on Xiaomi Mi Pad 1 (mocha)
 * Following the lc898212 pattern for Media Controller framework
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include <media/camera_common.h>

/* AD5823 register definitions */
#define AD5823_RESET			0x01
#define AD5823_MODE			0x02
#define AD5823_VCM_MOVE_TIME		0x03
#define AD5823_VCM_CODE_MSB		0x04
#define AD5823_VCM_CODE_LSB		0x05
#define AD5823_RING_CTRL		(1 << 2)
#define AD5823_MOVE_TIME_VALUE		0x43

#define AD5823_ACTUATOR_RANGE		1023
#define AD5823_POS_LOW_DEFAULT		0
#define AD5823_POS_HIGH_DEFAULT		1023
#define AD5823_FOCUS_MACRO		640
#define AD5823_FOCUS_INFINITY		140

#define SETTLETIME_MS			15
#define FOCAL_LENGTH			47300
#define MAX_APERTURE			22000
#define FNUMBER				22000

#define AD5823_PWR_DEV_OFF		0
#define AD5823_PWR_DEV_ON		1

#define NUM_FOCUS_STD_CTRLS		1
#define NUM_FOCUS_CTRLS			NUM_FOCUS_STD_CTRLS

struct ad5823 {
	struct i2c_client			*i2c_client;
	struct v4l2_subdev			*subdev;
	struct media_pad			pad;
	struct v4l2_ctrl_handler		ctrl_handler;
	struct camera_common_focuser_data	*s_data;
	struct regmap				*regmap;
	int					numctrls;
	struct v4l2_ctrl			*ctrls[];
};

static int ad5823_s_ctrl(struct v4l2_ctrl *ctrl);

static const struct v4l2_ctrl_ops ad5823_ctrl_ops = {
	.s_ctrl = ad5823_s_ctrl,
};

static int ad5823_set_position(struct ad5823 *priv, u32 position)
{
	int ret = 0;
	struct camera_common_focuser_data *s_data = priv->s_data;
	struct nv_focuser_config *cfg = &s_data->config;

	dev_dbg(&s_data->i2c_client->dev, "%s++\n", __func__);

	if (position < cfg->pos_actual_low || position > cfg->pos_actual_high) {
		dev_dbg(&priv->i2c_client->dev,
			"%s: position(%d) out of bound([%d, %d])\n",
			__func__, position, cfg->pos_actual_low,
			cfg->pos_actual_high);
		if (position < cfg->pos_actual_low)
			position = cfg->pos_actual_low;
		if (position > cfg->pos_actual_high)
			position = cfg->pos_actual_high;
	}

	ret |= regmap_write(priv->regmap, AD5823_VCM_MOVE_TIME,
			    AD5823_MOVE_TIME_VALUE);
	ret |= regmap_write(priv->regmap, AD5823_MODE, 0);
	ret |= regmap_write(priv->regmap, AD5823_VCM_CODE_MSB,
			    ((position >> 8) & 0x3) | AD5823_RING_CTRL);
	ret |= regmap_write(priv->regmap, AD5823_VCM_CODE_LSB,
			    position & 0xFF);

	dev_dbg(&s_data->i2c_client->dev, "%s: position=%d\n", __func__, position);
	return ret;
}

static int ad5823_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ad5823 *priv =
		container_of(ctrl->handler, struct ad5823, ctrl_handler);
	int err;

	dev_dbg(&priv->s_data->i2c_client->dev, "%s++\n", __func__);

	/* Power is managed by capture channel via s_power */
	if (priv->s_data->pwr_dev == AD5823_PWR_DEV_OFF)
		return -ENODEV;

	switch (ctrl->id) {
	case V4L2_CID_FOCUS_ABSOLUTE:
		err = ad5823_set_position(priv, ctrl->val);
		break;
	default:
		dev_err(&priv->i2c_client->dev, "%s: unknown v4l2 ctrl id\n",
			__func__);
		return -EINVAL;
	}

	return err;
}

static int ad5823_ctrls_init(struct camera_common_focuser_data *s_data)
{
	struct ad5823 *priv = (struct ad5823 *)s_data->priv;
	struct i2c_client *client = priv->i2c_client;
	struct v4l2_ctrl *ctrl;
	struct nv_focuser_config *cfg = &s_data->config;
	int min = cfg->pos_actual_low;
	int max = cfg->pos_actual_high;
	int def = s_data->def_position;
	int err = 0;

	v4l2_ctrl_handler_init(&priv->ctrl_handler, priv->numctrls);
	priv->subdev->ctrl_handler = &priv->ctrl_handler;
	err = priv->ctrl_handler.error;
	if (err) {
		dev_err(&client->dev, "Error %d adding controls\n", err);
		goto error;
	}

	/* add std controls */
	ctrl = v4l2_ctrl_new_std(&priv->ctrl_handler, &ad5823_ctrl_ops,
				 V4L2_CID_FOCUS_ABSOLUTE, min, max, 1, def);
	if (ctrl == NULL) {
		dev_err(&client->dev, "Error initializing controls\n");
		err = -EINVAL;
		goto error;
	}
	priv->ctrls[0] = ctrl;

	return 0;
error:
	v4l2_ctrl_handler_free(&priv->ctrl_handler);
	return err;
}

static int ad5823_init(struct ad5823 *priv)
{
	int err;

	/* Reset the device */
	err = regmap_write(priv->regmap, AD5823_RESET, 0x01);
	if (err)
		return err;

	usleep_range(300, 500);

	/* Set mode to 0 (normal operation) */
	err = regmap_write(priv->regmap, AD5823_MODE, 0x00);
	if (err)
		return err;

	/* Set VCM move time */
	err = regmap_write(priv->regmap, AD5823_VCM_MOVE_TIME,
			   AD5823_MOVE_TIME_VALUE);
	if (err)
		return err;

	return 0;
}

static int ad5823_load_config(struct camera_common_focuser_data *s_data)
{
	struct nv_focuser_config *cfg = &s_data->config;

	/* load default configuration */
	cfg->focal_length = FOCAL_LENGTH;
	cfg->fnumber = FNUMBER;
	cfg->max_aperture = MAX_APERTURE;
	cfg->range_ends_reversed = 0;

	cfg->pos_working_low = AD5823_FOCUS_INFINITY;
	cfg->pos_working_high = AD5823_FOCUS_MACRO;
	cfg->pos_actual_low = AD5823_POS_LOW_DEFAULT;
	cfg->pos_actual_high = AD5823_POS_HIGH_DEFAULT;

	cfg->num_focuser_sets = 1;
	cfg->focuser_set[0].macro = AD5823_FOCUS_MACRO;
	cfg->focuser_set[0].hyper = AD5823_FOCUS_INFINITY;
	cfg->focuser_set[0].inf = AD5823_FOCUS_INFINITY;
	cfg->focuser_set[0].settle_time = SETTLETIME_MS;

	return 0;
}

static int ad5823_power_off(struct camera_common_focuser_data *s_data)
{
	struct ad5823 *priv = (struct ad5823 *)s_data->priv;

	dev_dbg(&s_data->i2c_client->dev, "%s++\n", __func__);

	/* Move to default (infinity) position before power off */
	ad5823_set_position(priv, s_data->def_position);

	s_data->pwr_dev = AD5823_PWR_DEV_OFF;

	return 0;
}

static int ad5823_power_on(struct camera_common_focuser_data *s_data)
{
	struct ad5823 *priv = (struct ad5823 *)s_data->priv;
	int err;

	dev_dbg(&s_data->i2c_client->dev, "%s\n", __func__);

	err = ad5823_init(priv);
	if (err)
		return err;

	s_data->pwr_dev = AD5823_PWR_DEV_ON;
	return 0;
}

static struct camera_common_focuser_ops ad5823_ops = {
	.power_on = ad5823_power_on,
	.power_off = ad5823_power_off,
	.load_config = ad5823_load_config,
	.ctrls_init = ad5823_ctrls_init,
};

static int ad5823_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	dev_dbg(&client->dev, "%s:\n", __func__);
	return 0;
}

static struct v4l2_subdev_core_ops ad5823_subdev_core_ops = {
	.s_power = camera_common_focuser_s_power,
};

static struct v4l2_subdev_ops ad5823_subdev_ops = {
	.core = &ad5823_subdev_core_ops,
};

static const struct v4l2_subdev_internal_ops ad5823_subdev_internal_ops = {
	.open = ad5823_open,
};

static const struct media_entity_operations ad5823_media_ops = {
#ifdef CONFIG_MEDIA_CONTROLLER
	.link_validate = v4l2_subdev_link_validate,
#endif
};

static struct of_device_id ad5823_of_match[] = {
	{ .compatible = "nvidia,ad5823-mocha", },
	{ },
};
MODULE_DEVICE_TABLE(of, ad5823_of_match);

static int ad5823_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	int err;
	struct ad5823 *priv;
	struct camera_common_focuser_data *common_data;

	static struct regmap_config ad5823_regmap_config = {
		.reg_bits = 8,
		.val_bits = 8,
	};

	dev_info(&client->dev, "[ad5823_mocha]: probing focuser\n");

	common_data = devm_kzalloc(&client->dev,
			sizeof(struct camera_common_focuser_data), GFP_KERNEL);
	if (!common_data)
		return -ENOMEM;

	priv = devm_kzalloc(&client->dev,
		    (sizeof(struct ad5823) +
		     sizeof(struct v4l2_ctrl *) * NUM_FOCUS_CTRLS), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = devm_regmap_init_i2c(client, &ad5823_regmap_config);
	if (IS_ERR(priv->regmap)) {
		err = PTR_ERR(priv->regmap);
		dev_err(&client->dev,
			"Failed to allocate register map: %d\n", err);
		goto ERROR_RET;
	}

	common_data->ops = &ad5823_ops;
	common_data->ctrl_handler = &priv->ctrl_handler;
	common_data->i2c_client = client;
	common_data->ctrls = priv->ctrls;
	common_data->priv = (void *)priv;
	common_data->def_position = AD5823_FOCUS_INFINITY;

	priv->numctrls = NUM_FOCUS_CTRLS;
	priv->i2c_client = client;
	priv->s_data = common_data;
	priv->subdev = &common_data->subdev;
	priv->subdev->dev = &client->dev;

	/* Skip camera_common_focuser_init() — it calls power_on which
	 * does I2C, but at probe time IMX179 may not be powered.
	 * Call load_config + ctrls_init directly without power cycle.
	 * power_on (with I2C init) runs later via s_power from channel.
	 */
	err = ad5823_load_config(common_data);
	if (err) {
		dev_err(&client->dev, "unable to load focuser config\n");
		goto ERROR_RET;
	}
	err = ad5823_ctrls_init(common_data);
	if (err) {
		dev_err(&client->dev, "unable to init focuser controls\n");
		goto ERROR_RET;
	}

	v4l2_i2c_subdev_init(priv->subdev, client, &ad5823_subdev_ops);

	priv->subdev->internal_ops = &ad5823_subdev_internal_ops;
	priv->subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			       V4L2_SUBDEV_FL_HAS_EVENTS;

#if defined(CONFIG_MEDIA_CONTROLLER)
	priv->pad.flags = MEDIA_PAD_FL_SOURCE;
	priv->subdev->entity.type = MEDIA_ENT_T_V4L2_SUBDEV_LENS;
	priv->subdev->entity.ops = &ad5823_media_ops;
	err = media_entity_init(&priv->subdev->entity, 1, &priv->pad, 0);
	if (err < 0) {
		dev_err(&client->dev, "unable to init media entity\n");
		goto ERROR_RET;
	}
#endif

	err = v4l2_async_register_subdev(priv->subdev);
	if (err)
		goto ERROR_RET;

	dev_info(&client->dev, "Detected ad5823 focuser\n");
	return 0;

ERROR_RET:
	dev_err(&client->dev, "ad5823_mocha: probing focuser failed!!\n");
	return err;
}

static int ad5823_remove(struct i2c_client *client)
{
	struct camera_common_focuser_data *s_data =
		to_camera_common_focuser_data(client);
	struct ad5823 *priv = (struct ad5823 *)s_data->priv;

	v4l2_async_unregister_subdev(priv->subdev);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&priv->subdev->entity);
#endif
	v4l2_ctrl_handler_free(&priv->ctrl_handler);

	return 0;
}

static const struct i2c_device_id ad5823_id[] = {
	{ "ad5823_mocha", 0 },
	{ },
};

MODULE_DEVICE_TABLE(i2c, ad5823_id);

static struct i2c_driver ad5823_i2c_driver = {
	.driver = {
		.name = "ad5823_mocha",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ad5823_of_match),
	},
	.probe = ad5823_probe,
	.remove = ad5823_remove,
	.id_table = ad5823_id,
};

module_i2c_driver(ad5823_i2c_driver);

MODULE_DESCRIPTION("V4L2 subdev focuser driver for AD5823");
MODULE_AUTHOR("Smoke Team");
MODULE_LICENSE("GPL v2");
