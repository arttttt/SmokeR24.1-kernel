/*
 * imx179_mocha.c - IMX179 sensor driver for Xiaomi Mi Pad 1 (mocha)
 *
 * Fork of imx179.c with mocha-specific power sequence:
 * - 4 lanes on CSI-A
 * - 3 ext regulators + avdd (4 total)
 * - 2 GPIOs (reset, af)
 * - CSI-A/B DPD control
 * - RGGB Bayer format
 *
 * Power sequence from Xiaomi R21.5 board-ardbeg-sensors.c
 *
 * Copyright (c) 2014-2016, NVIDIA CORPORATION.  All rights reserved.
 * Copyright (c) 2025, Artem Bambalov <artembambalov1993@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/module.h>

#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

#include <linux/platform/tegra/io-dpd.h>

#include <media/camera_common.h>

#include "cam_dev/camera_gpio.h"

#include "imx179_mocha_mode_tbls.h"

/* CSI-A and CSI-B DPD for IMX179 on CSI-A/B port (4 lanes) */
static struct tegra_io_dpd csia_io = {
	.name			= "CSIA",
	.io_dpd_reg_index	= 0,
	.io_dpd_bit		= 0,
};

static struct tegra_io_dpd csib_io = {
	.name			= "CSIB",
	.io_dpd_reg_index	= 0,
	.io_dpd_bit		= 1,
};

#define IMX179_MAX_COARSE_DIFF		6

#define IMX179_OTP_SIZE			803
#define IMX179_OTP_STR_SIZE		(IMX179_OTP_SIZE * 2)

#define IMX179_GAIN_SHIFT		0
#define IMX179_MIN_GAIN			(1 << IMX179_GAIN_SHIFT)
#define IMX179_MAX_GAIN			(16 << IMX179_GAIN_SHIFT)
#define IMX179_MIN_FRAME_LENGTH		(0x0)
#define IMX179_MAX_FRAME_LENGTH		(0x7fff)
#define IMX179_MIN_EXPOSURE_COARSE	(0x0002)
#define IMX179_MAX_EXPOSURE_COARSE	\
	(IMX179_MAX_FRAME_LENGTH - IMX179_MAX_COARSE_DIFF)

#define IMX179_DEFAULT_GAIN		IMX179_MIN_GAIN
#define IMX179_DEFAULT_FRAME_LENGTH	(0x09CE)
#define IMX179_DEFAULT_EXPOSURE_COARSE	\
	(IMX179_DEFAULT_FRAME_LENGTH - IMX179_MAX_COARSE_DIFF)

#define IMX179_DEFAULT_MODE		IMX179_MODE_3280X2460
#define IMX179_DEFAULT_WIDTH		3264
#define IMX179_DEFAULT_HEIGHT		2448
#define IMX179_DEFAULT_DATAFMT		V4L2_MBUS_FMT_SRGGB10_1X10

static const struct camera_common_colorfmt imx179_color_fmts[] = {
	{ V4L2_MBUS_FMT_SRGGB10_1X10, V4L2_COLORSPACE_SRGB, V4L2_PIX_FMT_SRGGB10, },
};

struct imx179 {
	struct camera_common_power_rail	power;
	int				numctrls;
	struct v4l2_ctrl_handler	ctrl_handler;
	struct i2c_client		*i2c_client;
	struct v4l2_subdev		*subdev;
	struct media_pad		pad;

	s32				group_hold_prev;
	bool				group_hold_en;
	struct regmap			*regmap;
	struct camera_common_data	*s_data;
	struct camera_common_pdata	*pdata;

	/*
	 * Mocha-specific regulators (not in camera_common_power_rail):
	 *   ext_reg1 = imx179_reg1 (LDO7, 2.7V)
	 *   ext_reg2 = vdd_cam_1v2 (fixed, 1.2V)
	 *   ext_reg3 = vdd_cam_1v8 (fixed, 1.8V)
	 * avdd = LDO4 (2.7V) via camera_common pw->avdd
	 */
	struct regulator		*ext_reg1;
	struct regulator		*ext_reg2;
	struct regulator		*ext_reg3;

	struct v4l2_ctrl		*ctrls[];
};

static struct regmap_config imx179_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
};

static int imx179_g_volatile_ctrl(struct v4l2_ctrl *ctrl);
static int imx179_s_ctrl(struct v4l2_ctrl *ctrl);

static const struct v4l2_ctrl_ops imx179_ctrl_ops = {
	.g_volatile_ctrl = imx179_g_volatile_ctrl,
	.s_ctrl		= imx179_s_ctrl,
};

static struct v4l2_ctrl_config ctrl_config_list[] = {
	{
		.ops = &imx179_ctrl_ops,
		.id = V4L2_CID_GAIN,
		.name = "Gain",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.min = IMX179_MIN_GAIN,
		.max = IMX179_MAX_GAIN,
		.def = IMX179_DEFAULT_GAIN,
		.step = 1,
	},
	{
		.ops = &imx179_ctrl_ops,
		.id = V4L2_CID_FRAME_LENGTH,
		.name = "Frame Length",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.min = IMX179_MIN_FRAME_LENGTH,
		.max = IMX179_MAX_FRAME_LENGTH,
		.def = IMX179_DEFAULT_FRAME_LENGTH,
		.step = 1,
	},
	{
		.ops = &imx179_ctrl_ops,
		.id = V4L2_CID_COARSE_TIME,
		.name = "Coarse Time",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.min = IMX179_MIN_EXPOSURE_COARSE,
		.max = IMX179_MAX_EXPOSURE_COARSE,
		.def = IMX179_DEFAULT_EXPOSURE_COARSE,
		.step = 1,
	},
	{
		.ops = &imx179_ctrl_ops,
		.id = V4L2_CID_EXPOSURE,
		.name = "Exposure (us)",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.min = 1,
		.max = 1000000,
		.def = 33000,
		.step = 1,
	},
	{
		.ops = &imx179_ctrl_ops,
		.id = V4L2_CID_GROUP_HOLD,
		.name = "Group Hold",
		.type = V4L2_CTRL_TYPE_INTEGER_MENU,
		.min = 0,
		.max = ARRAY_SIZE(switch_ctrl_qmenu) - 1,
		.menu_skip_mask = 0,
		.def = 0,
		.qmenu_int = switch_ctrl_qmenu,
	},
	{
		.ops = &imx179_ctrl_ops,
		.id = V4L2_CID_HDR_EN,
		.name = "HDR enable",
		.type = V4L2_CTRL_TYPE_INTEGER_MENU,
		.min = 0,
		.max = ARRAY_SIZE(switch_ctrl_qmenu) - 1,
		.menu_skip_mask = 0,
		.def = 0,
		.qmenu_int = switch_ctrl_qmenu,
	},
	{
		.ops = &imx179_ctrl_ops,
		.id = V4L2_CID_OTP_DATA,
		.name = "OTP Data",
		.type = V4L2_CTRL_TYPE_STRING,
		.flags = V4L2_CTRL_FLAG_READ_ONLY,
		.min = 0,
		.max = IMX179_OTP_STR_SIZE,
		.step = 2,
	},
};

static inline void imx179_get_frame_length_regs(imx179_reg *regs,
				u32 frame_length)
{
	regs->addr = IMX179_FRAME_LENGTH_ADDR_MSB;
	regs->val = (frame_length >> 8) & 0xff;
	(regs + 1)->addr = IMX179_FRAME_LENGTH_ADDR_LSB;
	(regs + 1)->val = (frame_length) & 0xff;
}

static inline void imx179_get_coarse_time_regs(imx179_reg *regs,
				u32 coarse_time)
{
	regs->addr = IMX179_COARSE_TIME_ADDR_MSB;
	regs->val = (coarse_time >> 8) & 0xff;
	(regs + 1)->addr = IMX179_COARSE_TIME_ADDR_LSB;
	(regs + 1)->val = (coarse_time) & 0xff;
}

static inline void imx179_get_gain_reg(imx179_reg *regs, u16 gain)
{
	regs->addr = IMX179_GAIN_ADDR;
	regs->val = gain & 0xff;
}

static inline int imx179_read_reg(struct camera_common_data *s_data,
				u16 addr, u8 *val)
{
	struct imx179 *priv = (struct imx179 *)s_data->priv;

	return regmap_read(priv->regmap, addr, (unsigned int *)val);
}

static int imx179_write_reg(struct camera_common_data *s_data, u16 addr, u8 val)
{
	int err;
	struct imx179 *priv = (struct imx179 *)s_data->priv;

	err = regmap_write(priv->regmap, addr, val);
	if (err)
		dev_err(&priv->i2c_client->dev,
			"%s: i2c write failed, 0x%x = 0x%x\n",
			__func__, addr, val);

	return err;
}

static int imx179_write_table(struct imx179 *priv,
			      const imx179_reg table[])
{
	return regmap_util_write_table_8(priv->regmap,
					 table,
					 NULL, 0,
					 IMX179_TABLE_WAIT_MS,
					 IMX179_TABLE_END);
}

static int imx179_write_table_with_overrides(struct imx179 *priv,
			const imx179_reg table[],
			const imx179_reg overrides[],
			int num_overrides)
{
	const imx179_reg *next;
	int err, i;

	for (next = table; next->addr != IMX179_TABLE_END; next++) {
		if (next->addr == IMX179_TABLE_WAIT_MS) {
			msleep(next->val);
			continue;
		}

		u8 val = next->val;

		if (overrides) {
			for (i = 0; i < num_overrides; i++) {
				if (next->addr == overrides[i].addr) {
					val = overrides[i].val;
					break;
				}
			}
		}

		err = imx179_write_reg(priv->s_data, next->addr, val);
		if (err)
			return err;
	}
	return 0;
}

static void imx179_gpio_set(struct imx179 *priv,
			    unsigned int gpio, int val)
{
	if (priv->pdata->use_cam_gpio)
		cam_gpio_ctrl(priv->i2c_client, gpio, val, 1);
	else {
		if (gpio_cansleep(gpio))
			gpio_set_value_cansleep(gpio, val);
		else
			gpio_set_value(gpio, val);
	}
}

/*
 * Mocha IMX179 power-on sequence from Xiaomi R21.5 board-ardbeg-sensors.c
 * ardbeg_imx179_power_on():
 *
 *   1. DPD disable (csia_io + csib_io)
 *   2. ext_reg1 ON (imx179_reg1, LDO7, 2.7V)
 *   3. ext_reg2 ON (vdd_cam_1v2, fixed, 1.2V)
 *   4. ext_reg3 ON (vdd_cam_1v8, fixed, 1.8V)
 *   5. avdd ON (LDO4, 2.7V)
 *   6. usleep(1, 2)
 *   7. CAM_AF_PWDN=1, CAM_RSTN=0
 *   8. usleep(10, 20)
 *   9. CAM_RSTN=1
 *  10. usleep(300, 310)
 */
static int imx179_power_on(struct camera_common_data *s_data)
{
	int err = 0;
	struct imx179 *priv = (struct imx179 *)s_data->priv;
	struct camera_common_power_rail *pw = &priv->power;

	dev_dbg(&priv->i2c_client->dev, "%s: power on\n", __func__);

	if (priv->pdata && priv->pdata->power_on) {
		err = priv->pdata->power_on(pw);
		if (err)
			dev_err(&priv->i2c_client->dev, "%s failed\n", __func__);
		else
			pw->state = SWITCH_ON;
		return err;
	}

	/* Step 1: disable CSI-A/B IO DPD */
	tegra_io_dpd_disable(&csia_io);
	tegra_io_dpd_disable(&csib_io);

	/* MCLK is managed by camera_common_s_power() — not here */

	/* Step 2: ext_reg1 ON (imx179_reg1, LDO7, 2.7V) */
	if (priv->ext_reg1) {
		err = regulator_enable(priv->ext_reg1);
		if (err)
			goto imx179_ext_reg1_fail;
	}

	/* Step 3: ext_reg2 ON (vdd_cam_1v2, fixed, 1.2V) */
	if (priv->ext_reg2) {
		err = regulator_enable(priv->ext_reg2);
		if (err)
			goto imx179_ext_reg2_fail;
	}

	/* Step 4: ext_reg3 ON (vdd_cam_1v8, fixed, 1.8V) */
	if (priv->ext_reg3) {
		err = regulator_enable(priv->ext_reg3);
		if (err)
			goto imx179_ext_reg3_fail;
	}

	/* Step 5: avdd ON (LDO4, 2.7V) */
	if (pw->avdd) {
		err = regulator_enable(pw->avdd);
		if (err)
			goto imx179_avdd_fail;
	}

	/* Step 6: settling time */
	usleep_range(1, 2);

	/* Step 7: CAM_AF_PWDN=1, CAM_RSTN=0 */
	if (pw->af_gpio)
		imx179_gpio_set(priv, pw->af_gpio, 1);
	if (pw->reset_gpio)
		imx179_gpio_set(priv, pw->reset_gpio, 0);

	/* Step 8: settling time */
	usleep_range(10, 20);

	/* Step 9: CAM_RSTN=1 */
	if (pw->reset_gpio)
		imx179_gpio_set(priv, pw->reset_gpio, 1);

	/* Step 10: post-reset settling */
	usleep_range(300, 310);

	pw->state = SWITCH_ON;
	return 0;

imx179_avdd_fail:
	if (priv->ext_reg3)
		regulator_disable(priv->ext_reg3);
imx179_ext_reg3_fail:
	if (priv->ext_reg2)
		regulator_disable(priv->ext_reg2);
imx179_ext_reg2_fail:
	if (priv->ext_reg1)
		regulator_disable(priv->ext_reg1);
imx179_ext_reg1_fail:
	tegra_io_dpd_enable(&csia_io);
	tegra_io_dpd_enable(&csib_io);
	dev_err(&priv->i2c_client->dev, "%s failed\n", __func__);
	return -ENODEV;
}

/*
 * Mocha IMX179 power-off sequence from Xiaomi R21.5 board-ardbeg-sensors.c
 * ardbeg_imx179_power_off():
 *
 *   1. CAM_RSTN=0, CAM_AF_PWDN=0
 *   2. usleep(1, 2)
 *   3. avdd OFF
 *   4. ext_reg1 OFF
 *   5. ext_reg2 OFF
 *   6. ext_reg3 OFF
 *   7. DPD enable (csia + csib)
 */
static int imx179_power_off(struct camera_common_data *s_data)
{
	struct imx179 *priv = (struct imx179 *)s_data->priv;
	struct camera_common_power_rail *pw = &priv->power;

	dev_dbg(&priv->i2c_client->dev, "%s: power off\n", __func__);

	if (priv->pdata && priv->pdata->power_off) {
		int err = priv->pdata->power_off(pw);
		if (!err)
			pw->state = SWITCH_OFF;
		else
			dev_err(&priv->i2c_client->dev, "%s failed\n", __func__);
		return err;
	}

	/* Step 1: reset and af GPIOs low */
	if (pw->reset_gpio)
		imx179_gpio_set(priv, pw->reset_gpio, 0);
	if (pw->af_gpio)
		imx179_gpio_set(priv, pw->af_gpio, 0);

	/* Step 2: settling time */
	usleep_range(1, 2);

	/* Step 3: avdd OFF */
	if (pw->avdd)
		regulator_disable(pw->avdd);

	/* Step 4: ext_reg1 OFF */
	if (priv->ext_reg1)
		regulator_disable(priv->ext_reg1);

	/* Step 5: ext_reg2 OFF */
	if (priv->ext_reg2)
		regulator_disable(priv->ext_reg2);

	/* Step 6: ext_reg3 OFF */
	if (priv->ext_reg3)
		regulator_disable(priv->ext_reg3);

	/* MCLK is managed by camera_common_s_power() - not here */

	/* Step 7: enable CSI-A/B IO DPD */
	tegra_io_dpd_enable(&csia_io);
	tegra_io_dpd_enable(&csib_io);

	pw->state = SWITCH_OFF;
	return 0;
}

static int imx179_power_put(struct imx179 *priv)
{
	struct camera_common_power_rail *pw = &priv->power;

	if (unlikely(!pw))
		return -EFAULT;

	if (likely(pw->avdd))
		regulator_put(pw->avdd);

	if (likely(priv->ext_reg1))
		regulator_put(priv->ext_reg1);

	if (likely(priv->ext_reg2))
		regulator_put(priv->ext_reg2);

	if (likely(priv->ext_reg3))
		regulator_put(priv->ext_reg3);

	pw->avdd = NULL;
	priv->ext_reg1 = NULL;
	priv->ext_reg2 = NULL;
	priv->ext_reg3 = NULL;

	if (priv->pdata->use_cam_gpio) {
		cam_gpio_deregister(priv->i2c_client, pw->reset_gpio);
	} else {
		if (pw->reset_gpio)
			gpio_free(pw->reset_gpio);
		if (pw->af_gpio)
			gpio_free(pw->af_gpio);
	}

	return 0;
}

static int imx179_power_get(struct imx179 *priv)
{
	struct camera_common_power_rail *pw = &priv->power;
	struct camera_common_pdata *pdata = priv->pdata;
	const char *mclk_name;
	const char *parentclk_name;
	struct clk *parent;
	int err = 0;

	mclk_name = priv->pdata->mclk_name ?
		    priv->pdata->mclk_name : "mclk";
	pw->mclk = devm_clk_get(&priv->i2c_client->dev, mclk_name);
	if (IS_ERR(pw->mclk)) {
		dev_err(&priv->i2c_client->dev,
			"unable to get clock %s\n", mclk_name);
		return PTR_ERR(pw->mclk);
	}

	parentclk_name = priv->pdata->parentclk_name;
	if (parentclk_name) {
		parent = devm_clk_get(&priv->i2c_client->dev, parentclk_name);
		if (IS_ERR(parent))
			dev_err(&priv->i2c_client->dev,
				"unable to get parent clock %s",
				parentclk_name);
		else
			clk_set_parent(pw->mclk, parent);
	}

	/* analog 2.7V (LDO4) — only standard rail, rest are ext_reg in probe */
	err |= camera_common_regulator_get(priv->i2c_client,
			&pw->avdd, pdata->regulators.avdd);

	if (!err) {
		pw->reset_gpio = pdata->reset_gpio;
		pw->af_gpio = pdata->af_gpio;
	}

	if (priv->pdata->use_cam_gpio) {
		err = cam_gpio_register(priv->i2c_client, pw->reset_gpio);
		if (err)
			dev_err(&priv->i2c_client->dev,
				"%s ERR can't register cam gpio %u!\n",
				 __func__, pw->reset_gpio);
	} else {
		int gpio_err;
		if (pw->reset_gpio) {
			gpio_err = gpio_request_one(pw->reset_gpio,
					 GPIOF_OUT_INIT_LOW, "cam_reset_gpio");
			if (gpio_err) {
				dev_warn(&priv->i2c_client->dev,
					"reset gpio request failed: %d\n", gpio_err);
				pw->reset_gpio = 0;
			}
		}
		if (pw->af_gpio) {
			gpio_err = gpio_request_one(pw->af_gpio,
					 GPIOF_OUT_INIT_LOW, "cam_af_pwdn_gpio");
			if (gpio_err) {
				dev_warn(&priv->i2c_client->dev,
					"af gpio request failed: %d\n", gpio_err);
				pw->af_gpio = 0;
			}
		}
	}

	pw->state = SWITCH_OFF;
	return err;
}

static int imx179_set_gain(struct imx179 *priv, s32 val);
static int imx179_set_frame_length(struct imx179 *priv, s32 val);
static int imx179_set_coarse_time(struct imx179 *priv, s32 val);
static int imx179_set_group_hold(struct imx179 *priv);

static int imx179_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(client);
	struct imx179 *priv = (struct imx179 *)s_data->priv;
	struct v4l2_control control;
	int err;

	dev_dbg(&client->dev, "%s: enable=%d mode=%d\n",
		 __func__, enable, s_data->mode);

	if (!enable) {
		return imx179_write_table(priv,
			mode_table[IMX179_MODE_STOP_STREAM]);
	}

	/* Build override list for exposure/gain/frame_length —
	 * stock driver replaces these registers during mode table write
	 * so they take effect BEFORE stream starts */
	{
		imx179_reg overrides[5]; /* 2 frame_length + 2 coarse + 1 gain */
		u32 frame_length, coarse_time, gain;
		const struct camera_common_frmfmt *fmt =
			&s_data->frmfmt[s_data->mode];

		/* Read cached ctrl values */
		control.id = V4L2_CID_FRAME_LENGTH;
		err = v4l2_g_ctrl(&priv->ctrl_handler, &control);
		frame_length = err ? IMX179_DEFAULT_FRAME_LENGTH : control.value;

		control.id = V4L2_CID_COARSE_TIME;
		err = v4l2_g_ctrl(&priv->ctrl_handler, &control);
		coarse_time = err ? (frame_length - IMX179_MAX_COARSE_DIFF) :
				    control.value;

		/* V4L2_CID_EXPOSURE overrides COARSE_TIME if set */
		control.id = V4L2_CID_EXPOSURE;
		err = v4l2_g_ctrl(&priv->ctrl_handler, &control);
		if (!err && control.value > 0) {
			u32 max_coarse = frame_length - IMX179_MAX_COARSE_DIFF;
			coarse_time = (u32)div_u64(
				(u64)control.value * fmt->pix_clk_hz,
				(u64)fmt->line_length * 1000000ULL);
			if (coarse_time > max_coarse)
				coarse_time = max_coarse;
		}

		control.id = V4L2_CID_GAIN;
		err = v4l2_g_ctrl(&priv->ctrl_handler, &control);
		gain = err ? IMX179_DEFAULT_GAIN : control.value;

		dev_info(&client->dev,
			 "%s: mode=%d frame_len=%u coarse=%u gain=%u\n",
			 __func__, s_data->mode, frame_length,
			 coarse_time, gain);

		/* Build override register list */
		imx179_get_frame_length_regs(overrides, frame_length);
		imx179_get_coarse_time_regs(overrides + 2, coarse_time);
		imx179_get_gain_reg(overrides + 4, gain);

		/* Write mode table with overrides (like stock driver) */
		err = imx179_write_table_with_overrides(priv,
			mode_table[s_data->mode], overrides, 5);
		if (err) {
			dev_err(&client->dev,
				"%s: write_table mode %d failed: %d\n",
				__func__, s_data->mode, err);
			goto exit;
		}
	}

	err = imx179_write_table(priv, mode_table[IMX179_MODE_START_STREAM]);
	if (err)
		goto exit;

	dev_dbg(&client->dev, "%s--\n", __func__);
	return 0;
exit:
	dev_dbg(&client->dev, "%s: error setting stream\n", __func__);
	return err;
}

static int imx179_g_input_status(struct v4l2_subdev *sd, u32 *status)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(client);
	struct imx179 *priv = (struct imx179 *)s_data->priv;
	struct camera_common_power_rail *pw = &priv->power;

	*status = pw->state == SWITCH_ON;
	return 0;
}

static int imx179_s_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *param)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(client);

	if (param->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	s_data->requested_fps =
		param->parm.capture.timeperframe.denominator /
		(param->parm.capture.timeperframe.numerator ?
		 param->parm.capture.timeperframe.numerator : 1);

	dev_info(&client->dev, "s_parm: requested_fps=%d\n",
		 s_data->requested_fps);
	return 0;
}

static int imx179_g_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *param)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(client);

	if (param->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	param->parm.capture.timeperframe.numerator = 1;
	param->parm.capture.timeperframe.denominator =
		s_data->requested_fps ? s_data->requested_fps : 30;
	return 0;
}

static struct v4l2_subdev_video_ops imx179_subdev_video_ops = {
	.s_stream	= imx179_s_stream,
	.s_mbus_fmt	= camera_common_s_fmt,
	.g_mbus_fmt	= camera_common_g_fmt,
	.try_mbus_fmt	= camera_common_try_fmt,
	.enum_mbus_fmt	= camera_common_enum_fmt,
	.g_mbus_config	= camera_common_g_mbus_config,
	.g_input_status = imx179_g_input_status,
	.enum_framesizes	= camera_common_enum_framesizes,
	.enum_frameintervals	= camera_common_enum_frameintervals,
	.s_parm		= imx179_s_parm,
	.g_parm		= imx179_g_parm,
};

static struct v4l2_subdev_core_ops imx179_subdev_core_ops = {
	.s_power	= camera_common_s_power,
};

static int imx179_get_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_fh *fh,
		struct v4l2_subdev_format *format)
{
	return camera_common_g_fmt(sd, &format->format);
}

static int imx179_set_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_fh *fh,
	struct v4l2_subdev_format *format)
{
	int ret;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		ret = camera_common_try_fmt(sd, &format->format);
	else
		ret = camera_common_s_fmt(sd, &format->format);

	return ret;
}

static struct v4l2_subdev_pad_ops imx179_subdev_pad_ops = {
	.enum_mbus_code = camera_common_enum_mbus_code,
	.set_fmt = imx179_set_fmt,
	.get_fmt = imx179_get_fmt,
};

static struct v4l2_subdev_ops imx179_subdev_ops = {
	.core	= &imx179_subdev_core_ops,
	.video	= &imx179_subdev_video_ops,
	.pad	= &imx179_subdev_pad_ops,
};

static struct of_device_id imx179_of_match[] = {
	{ .compatible = "nvidia,imx179-mocha", },
	{ },
};

static struct camera_common_sensor_ops imx179_common_ops = {
	.power_on = imx179_power_on,
	.power_off = imx179_power_off,
	.write_reg = imx179_write_reg,
	.read_reg = imx179_read_reg,
};

static int imx179_set_group_hold(struct imx179 *priv)
{
	int err;
	int gh_prev = switch_ctrl_qmenu[priv->group_hold_prev];

	if (priv->group_hold_en == true && gh_prev == SWITCH_OFF) {
		/* enter group hold */
		err = imx179_write_reg(priv->s_data,
				       IMX179_GROUP_HOLD_ADDR, 0x01);
		if (err)
			goto fail;

		priv->group_hold_prev = 1;

		dev_dbg(&priv->i2c_client->dev,
			 "%s: enter group hold\n", __func__);
	} else if (priv->group_hold_en == false && gh_prev == SWITCH_ON) {
		/* leave group hold */
		err = imx179_write_reg(priv->s_data,
				       IMX179_GROUP_HOLD_ADDR, 0x00);
		if (err)
			goto fail;

		priv->group_hold_prev = 0;

		dev_dbg(&priv->i2c_client->dev,
			 "%s: leave group hold\n", __func__);
	}

	return 0;

fail:
	dev_dbg(&priv->i2c_client->dev,
		 "%s: Group hold control error\n", __func__);
	return err;
}

static int imx179_set_gain(struct imx179 *priv, s32 val)
{
	imx179_reg reg_list;
	int err;
	u16 gain;

	if (!priv->group_hold_prev)
		imx179_set_group_hold(priv);

	/* IMX179 gain register 0x0205: 0=1x ... 255=~16x */
	gain = (u16)val;

	imx179_get_gain_reg(&reg_list, gain);
	dev_dbg(&priv->i2c_client->dev,
		 "%s: gain %04x val: %04x\n", __func__, val, gain);

	err = imx179_write_reg(priv->s_data, reg_list.addr, reg_list.val);
	if (err)
		goto fail;

	return 0;

fail:
	dev_dbg(&priv->i2c_client->dev,
		 "%s: GAIN control error\n", __func__);
	return err;
}

static int imx179_set_frame_length(struct imx179 *priv, s32 val)
{
	imx179_reg reg_list[2];
	int err;
	u32 frame_length;
	int i;

	if (!priv->group_hold_prev)
		imx179_set_group_hold(priv);

	frame_length = (u32)val;

	imx179_get_frame_length_regs(reg_list, frame_length);
	dev_dbg(&priv->i2c_client->dev,
		 "%s: val: %d\n", __func__, frame_length);

	for (i = 0; i < 2; i++) {
		err = imx179_write_reg(priv->s_data, reg_list[i].addr,
				 reg_list[i].val);
		if (err)
			goto fail;
	}

	return 0;

fail:
	dev_dbg(&priv->i2c_client->dev,
		 "%s: FRAME_LENGTH control error\n", __func__);
	return err;
}

static int imx179_set_coarse_time(struct imx179 *priv, s32 val)
{
	imx179_reg reg_list[2];
	int err;
	u32 coarse_time;
	int i;

	if (!priv->group_hold_prev)
		imx179_set_group_hold(priv);

	coarse_time = (u32)val;

	imx179_get_coarse_time_regs(reg_list, coarse_time);
	dev_dbg(&priv->i2c_client->dev,
		 "%s: val: %d\n", __func__, coarse_time);

	for (i = 0; i < 2; i++) {
		err = imx179_write_reg(priv->s_data, reg_list[i].addr,
				 reg_list[i].val);
		if (err)
			goto fail;
	}

	return 0;

fail:
	dev_dbg(&priv->i2c_client->dev,
		 "%s: COARSE_TIME control error\n", __func__);
	return err;
}

static int imx179_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx179 *priv =
		container_of(ctrl->handler, struct imx179, ctrl_handler);

	if (priv->power.state == SWITCH_OFF)
		return 0;

	switch (ctrl->id) {
	default:
		dev_err(&priv->i2c_client->dev,
			"%s: unknown ctrl id %d\n", __func__, ctrl->id);
		return -EINVAL;
	}

	return 0;
}

static int imx179_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx179 *priv =
		container_of(ctrl->handler, struct imx179, ctrl_handler);
	int err = 0;

	if (priv->power.state == SWITCH_OFF)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_GAIN:
		err = imx179_set_gain(priv, ctrl->val);
		break;
	case V4L2_CID_FRAME_LENGTH:
		err = imx179_set_frame_length(priv, ctrl->val);
		break;
	case V4L2_CID_EXPOSURE: {
		/* Convert microseconds to coarse_time (sensor lines) */
		struct camera_common_data *s_data = priv->s_data;
		const struct camera_common_frmfmt *fmt =
			&s_data->frmfmt[s_data->mode];
		u32 coarse = (u32)div_u64((u64)ctrl->val * fmt->pix_clk_hz,
					  (u64)fmt->line_length * 1000000ULL);
		err = imx179_set_coarse_time(priv, coarse);
		break;
	}
	case V4L2_CID_COARSE_TIME:
		err = imx179_set_coarse_time(priv, ctrl->val);
		break;
	case V4L2_CID_GROUP_HOLD:
		if (switch_ctrl_qmenu[ctrl->val] == SWITCH_ON) {
			priv->group_hold_en = true;
		} else {
			priv->group_hold_en = false;
			err = imx179_set_group_hold(priv);
		}
		break;
	case V4L2_CID_HDR_EN:
		break;
	default:
		dev_err(&priv->i2c_client->dev,
			"%s: unknown ctrl id %d\n", __func__, ctrl->id);
		return -EINVAL;
	}

	return err;
}

static int imx179_otp_setup(struct imx179 *priv)
{
	int err = 0;
	int i, j;
	struct v4l2_ctrl *ctrl;
	u8 otp_buf[IMX179_OTP_SIZE];
	u8 *otp = otp_buf;
	u8 bak = 0;

	err = camera_common_s_power(priv->subdev, true);
	if (err)
		return -ENODEV;

	/* Setup OTP read mode */
	imx179_write_reg(priv->s_data, 0x0100, 0x00);  /* standby */
	imx179_write_reg(priv->s_data, 0x3382, 0x05);
	imx179_write_reg(priv->s_data, 0x3383, 0xa0);
	imx179_write_reg(priv->s_data, 0x3368, 0x24);
	imx179_write_reg(priv->s_data, 0x3369, 0x00);

	/* Check OTP write result */
	imx179_write_reg(priv->s_data, 0x3380, 0x08);
	imx179_write_reg(priv->s_data, 0x3400, 0x01);
	imx179_write_reg(priv->s_data, 0x3402, 0x12);
	udelay(10);
	for (i = 0x3443; i >= 0x3442; i--) {
		imx179_read_reg(priv->s_data, i, &bak);
		if ((bak == 0x11) || (bak == 0xEE))
			break;
	}

	if (bak == 0xEE) {
		dev_warn(&priv->i2c_client->dev, "OTP write check failed\n");
		goto ret;
	}

	/* Read bank 0: 45 bytes at 0x3417-0x3443 */
	imx179_write_reg(priv->s_data, 0x3380, 0x08);
	imx179_write_reg(priv->s_data, 0x3400, 0x01);
	imx179_write_reg(priv->s_data, 0x3402, 0x00);
	udelay(10);
	for (i = 0; i < 45; i++) {
		err |= imx179_read_reg(priv->s_data, 0x3417 + i, otp);
		otp++;
	}

	/* Read banks 1-11: 64 bytes each at 0x3404-0x3443 */
	for (j = 1; j <= 11; j++) {
		imx179_write_reg(priv->s_data, 0x3380, 0x08);
		imx179_write_reg(priv->s_data, 0x3400, 0x01);
		imx179_write_reg(priv->s_data, 0x3402, j);
		udelay(10);
		for (i = 0; i < 64; i++) {
			err |= imx179_read_reg(priv->s_data, 0x3404 + i, otp);
			otp++;
		}
	}

	/* Read bank 12: 54 bytes at 0x3404-0x3439 */
	imx179_write_reg(priv->s_data, 0x3380, 0x08);
	imx179_write_reg(priv->s_data, 0x3400, 0x01);
	imx179_write_reg(priv->s_data, 0x3402, 12);
	udelay(10);
	for (i = 0; i < 54; i++) {
		err |= imx179_read_reg(priv->s_data, 0x3404 + i, otp);
		otp++;
	}

	if (err) {
		dev_err(&priv->i2c_client->dev, "OTP read failed\n");
		goto ret;
	}

	ctrl = v4l2_ctrl_find(&priv->ctrl_handler, V4L2_CID_OTP_DATA);
	if (!ctrl) {
		dev_err(&priv->i2c_client->dev, "could not find OTP ctrl\n");
		err = -EINVAL;
		goto ret;
	}

	for (i = 0; i < IMX179_OTP_SIZE; i++)
		sprintf(&ctrl->string[i * 2], "%02x", otp_buf[i]);
	ctrl->cur.string = ctrl->string;

	dev_dbg(&priv->i2c_client->dev, "OTP data read successfully\n");

ret:
	camera_common_s_power(priv->subdev, false);
	return err;
}

static int imx179_ctrls_init(struct imx179 *priv)
{
	struct i2c_client *client = priv->i2c_client;
	struct v4l2_ctrl *ctrl;
	int numctrls;
	int err;
	int i;

	dev_dbg(&client->dev, "%s++\n", __func__);

	numctrls = ARRAY_SIZE(ctrl_config_list);
	v4l2_ctrl_handler_init(&priv->ctrl_handler, numctrls);

	for (i = 0; i < numctrls; i++) {
		ctrl = v4l2_ctrl_new_custom(&priv->ctrl_handler,
			&ctrl_config_list[i], NULL);
		if (ctrl == NULL) {
			dev_err(&client->dev, "Failed to init %s ctrl\n",
				ctrl_config_list[i].name);
			continue;
		}

		if (ctrl_config_list[i].type == V4L2_CTRL_TYPE_STRING &&
			ctrl_config_list[i].flags & V4L2_CTRL_FLAG_READ_ONLY) {
			ctrl->string = devm_kzalloc(&client->dev,
				ctrl_config_list[i].max + 1, GFP_KERNEL);
			if (!ctrl->string)
				return -ENOMEM;
		}
		priv->ctrls[i] = ctrl;
	}

	priv->numctrls = numctrls;
	priv->subdev->ctrl_handler = &priv->ctrl_handler;
	if (priv->ctrl_handler.error) {
		dev_err(&client->dev, "Error %d adding controls\n",
			priv->ctrl_handler.error);
		err = priv->ctrl_handler.error;
		goto error;
	}

	err = v4l2_ctrl_handler_setup(&priv->ctrl_handler);
	if (err) {
		dev_err(&client->dev,
			"Error %d setting default controls\n", err);
		goto error;
	}

	err = imx179_otp_setup(priv);
	if (err)
		dev_warn(&client->dev, "OTP read failed: %d (non-fatal)\n", err);

	return 0;

error:
	v4l2_ctrl_handler_free(&priv->ctrl_handler);
	return err;
}

MODULE_DEVICE_TABLE(of, imx179_of_match);

static struct camera_common_pdata *imx179_parse_dt(struct i2c_client *client)
{
	struct device_node *node = client->dev.of_node;
	struct camera_common_pdata *board_priv_pdata;
	const struct of_device_id *match;
	int gpio;
	int err;

	if (!node)
		return NULL;

	match = of_match_device(imx179_of_match, &client->dev);
	if (!match) {
		dev_err(&client->dev, "Failed to find matching dt id\n");
		return NULL;
	}

	board_priv_pdata = devm_kzalloc(&client->dev,
			   sizeof(*board_priv_pdata), GFP_KERNEL);
	if (!board_priv_pdata)
		return NULL;

	err = camera_common_parse_clocks(client, board_priv_pdata);
	if (err) {
		dev_err(&client->dev, "Failed to find clocks\n");
		goto error;
	}

	gpio = of_get_named_gpio(node, "reset-gpios", 0);
	if (gpio < 0) {
		dev_err(&client->dev, "reset gpios not in DT\n");
		goto error;
	}
	board_priv_pdata->reset_gpio = (unsigned int)gpio;

	/* Mocha: AF enable GPIO */
	gpio = of_get_named_gpio(node, "af-gpios", 0);
	if (gpio < 0) {
		dev_dbg(&client->dev, "af gpios not in DT\n");
		gpio = 0;
	}
	board_priv_pdata->af_gpio = (unsigned int)gpio;

	board_priv_pdata->use_cam_gpio =
		of_property_read_bool(node, "cam,use-cam-gpio");

	err = of_property_read_string(node, "avdd-reg",
			&board_priv_pdata->regulators.avdd);
	if (err) {
		dev_err(&client->dev, "avdd-reg not in DT\n");
		goto error;
	}
	/* iovdd/dvdd not used — power is via ext_reg1/2/3 + avdd */

	return board_priv_pdata;

error:
	devm_kfree(&client->dev, board_priv_pdata);
	return NULL;
}

static int imx179_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	dev_dbg(&client->dev, "%s:\n", __func__);
	return 0;
}

static const struct v4l2_subdev_internal_ops imx179_subdev_internal_ops = {
	.open = imx179_open,
};

static const struct media_entity_operations imx179_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int imx179_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct camera_common_data *common_data;
	struct device_node *node = client->dev.of_node;
	struct imx179 *priv;
	const char *ext_reg1_name;
	char debugfs_name[10];
	u16 chip_id;
	int err;

	dev_dbg(&client->dev, "probing v4l2 sensor on %s\n",
		dev_name(&client->dev));

	if (!IS_ENABLED(CONFIG_OF) || !node)
		return -EINVAL;

	common_data = devm_kzalloc(&client->dev,
			    sizeof(struct camera_common_data), GFP_KERNEL);
	if (!common_data)
		return -ENOMEM;

	priv = devm_kzalloc(&client->dev,
			    sizeof(struct imx179) + sizeof(struct v4l2_ctrl *) *
			    ARRAY_SIZE(ctrl_config_list),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = devm_regmap_init_i2c(client, &imx179_regmap_config);
	if (IS_ERR(priv->regmap)) {
		dev_err(&client->dev,
			"regmap init failed: %ld\n", PTR_ERR(priv->regmap));
		return -ENODEV;
	}

	priv->pdata = imx179_parse_dt(client);
	if (!priv->pdata) {
		dev_err(&client->dev, "unable to get platform data\n");
		return -EFAULT;
	}
	dev_dbg(&client->dev, "parse_dt done: mclk=%s reset=%u af=%u\n",
		priv->pdata->mclk_name ? priv->pdata->mclk_name : "(null)",
		priv->pdata->reset_gpio, priv->pdata->af_gpio);

	/* Mandatory regulators: ext_reg1/2/3 (part of stock power sequence) */
	err = of_property_read_string(node, "ext_reg1-reg", &ext_reg1_name);
	if (err) {
		dev_err(&client->dev, "ext_reg1-reg missing in DT\n");
		return -EINVAL;
	}
	err = camera_common_regulator_get(client,
			&priv->ext_reg1, ext_reg1_name);
	if (err) {
		dev_err(&client->dev,
			"unable to get ext_reg1 regulator %s: %d\n",
			ext_reg1_name, err);
		return err;
	}

	/* ext_reg2 (vdd_cam_1v2, fixed, 1.2V) */
	{
		const char *ext_reg2_name;
		err = of_property_read_string(node, "ext_reg2-reg", &ext_reg2_name);
		if (err) {
			dev_err(&client->dev, "ext_reg2-reg missing in DT\n");
			return -EINVAL;
		}
		err = camera_common_regulator_get(client,
				&priv->ext_reg2, ext_reg2_name);
		if (err) {
			dev_err(&client->dev,
				"unable to get ext_reg2 regulator %s: %d\n",
				ext_reg2_name, err);
			return err;
		}
	}

	/* ext_reg3 (vdd_cam_1v8, fixed, 1.8V) */
	{
		const char *ext_reg3_name;
		err = of_property_read_string(node, "ext_reg3-reg", &ext_reg3_name);
		if (err) {
			dev_err(&client->dev, "ext_reg3-reg missing in DT\n");
			return -EINVAL;
		}
		err = camera_common_regulator_get(client,
				&priv->ext_reg3, ext_reg3_name);
		if (err) {
			dev_err(&client->dev,
				"unable to get ext_reg3 regulator %s: %d\n",
				ext_reg3_name, err);
			return err;
		}
	}

	common_data->ops		= &imx179_common_ops;
	common_data->ctrl_handler	= &priv->ctrl_handler;
	common_data->i2c_client		= client;
	common_data->frmfmt		= imx179_frmfmt;
	common_data->colorfmt		= camera_common_find_datafmt(
					  IMX179_DEFAULT_DATAFMT);
	common_data->power		= &priv->power;
	common_data->ctrls		= priv->ctrls;
	common_data->priv		= (void *)priv;
	common_data->numctrls		= ARRAY_SIZE(ctrl_config_list);
	common_data->numfmts		= ARRAY_SIZE(imx179_frmfmt);
	common_data->def_mode		= IMX179_DEFAULT_MODE;
	common_data->def_width		= IMX179_DEFAULT_WIDTH;
	common_data->def_height		= IMX179_DEFAULT_HEIGHT;
	common_data->fmt_width		= common_data->def_width;
	common_data->fmt_height		= common_data->def_height;
	common_data->def_clk_freq	= IMX179_DEFAULT_CLK_FREQ;
	common_data->color_fmts		= imx179_color_fmts;
	common_data->num_color_fmts	= ARRAY_SIZE(imx179_color_fmts);

	priv->i2c_client = client;
	priv->s_data			= common_data;
	priv->subdev			= &common_data->subdev;
	priv->subdev->dev		= &client->dev;
	priv->s_data->dev		= &client->dev;

	err = imx179_power_get(priv);
	if (err) {
		dev_err(&client->dev, "power_get failed: %d\n", err);
		return err;
	}
	dev_dbg(&client->dev, "power_get done: avdd=%p iovdd=%p dvdd=%p ext1=%p ext2=%p\n",
		priv->power.avdd, priv->power.iovdd, priv->power.dvdd,
		priv->ext_reg1, priv->ext_reg2);

	err = camera_common_parse_ports(client, common_data);
	if (err) {
		dev_err(&client->dev, "Failed to find port info: %d\n", err);
		return err;
	}
	sprintf(debugfs_name, "imx179_%c", common_data->csi_port + 'a');
	dev_dbg(&client->dev, "%s: name %s\n", __func__, debugfs_name);
	camera_common_create_debugfs(common_data, debugfs_name);

	v4l2_i2c_subdev_init(priv->subdev, client, &imx179_subdev_ops);

	err = imx179_ctrls_init(priv);
	if (err)
		return err;

	priv->subdev->internal_ops = &imx179_subdev_internal_ops;
	priv->subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			       V4L2_SUBDEV_FL_HAS_EVENTS;

#if defined(CONFIG_MEDIA_CONTROLLER)
	priv->pad.flags = MEDIA_PAD_FL_SOURCE;
	priv->subdev->entity.type = MEDIA_ENT_T_V4L2_SUBDEV_SENSOR;
	priv->subdev->entity.ops = &imx179_media_ops;
	err = media_entity_init(&priv->subdev->entity, 1, &priv->pad, 0);
	if (err < 0) {
		dev_err(&client->dev, "unable to init media entity\n");
		return err;
	}
#endif

	/* Power on to read chip ID */
	err = camera_common_s_power(priv->subdev, true);
	if (err) {
		dev_err(&client->dev, "Failed to power on for chip ID: %d\n", err);
		goto error;
	}

	/* Read 16-bit chip ID from two 8-bit registers */
	{
		u8 id_msb, id_lsb;
		err = imx179_read_reg(common_data, IMX179_CHIP_ID_ADDR_MSB, &id_msb);
		err |= imx179_read_reg(common_data, IMX179_CHIP_ID_ADDR_LSB, &id_lsb);
		if (err) {
			dev_err(&client->dev, "Failed to read chip ID: %d\n", err);
			camera_common_s_power(priv->subdev, false);
			goto error;
		}
		chip_id = (id_msb << 8) | id_lsb;
	}

	if (chip_id != IMX179_CHIP_ID) {
		dev_err(&client->dev,
			"Chip ID mismatch: expected 0x%04x, got 0x%04x\n",
			IMX179_CHIP_ID, chip_id);
		camera_common_s_power(priv->subdev, false);
		err = -ENODEV;
		goto error;
	}

	dev_info(&client->dev, "Detected IMX179 sensor (chip ID 0x%04x)\n", chip_id);

	camera_common_s_power(priv->subdev, false);

	err = v4l2_async_register_subdev(priv->subdev);
	if (err)
		goto error;

	return 0;

error:
	v4l2_ctrl_handler_free(&priv->ctrl_handler);
	imx179_power_put(priv);
	camera_common_remove_debugfs(common_data);
	return err;
}

static int imx179_remove(struct i2c_client *client)
{
	struct camera_common_data *s_data = to_camera_common_data(client);
	struct imx179 *priv = (struct imx179 *)s_data->priv;

	v4l2_async_unregister_subdev(priv->subdev);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&priv->subdev->entity);
#endif

	v4l2_ctrl_handler_free(&priv->ctrl_handler);
	imx179_power_put(priv);
	camera_common_remove_debugfs(s_data);

	return 0;
}

static const struct i2c_device_id imx179_id[] = {
	{ "imx179_mocha", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, imx179_id);

static struct i2c_driver imx179_i2c_driver = {
	.driver = {
		.name = "imx179_mocha",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(imx179_of_match),
	},
	.probe = imx179_probe,
	.remove = imx179_remove,
	.id_table = imx179_id,
};

static int __init imx179_mocha_init(void)
{
	pr_info("[IMX179-mocha]: registering i2c driver\n");
	return i2c_add_driver(&imx179_i2c_driver);
}

static void __exit imx179_mocha_exit(void)
{
	i2c_del_driver(&imx179_i2c_driver);
}

module_init(imx179_mocha_init);
module_exit(imx179_mocha_exit);

MODULE_DESCRIPTION("V4L2 sensor driver for IMX179 on Xiaomi Mi Pad 1 (mocha)");
MODULE_AUTHOR("Artem Bambalov <artembambalov1993@gmail.com>");
MODULE_LICENSE("GPL v2");
