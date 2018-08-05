/*
 * arch/arm/mach-tegra/board-ardbeg-sensors.c
 *
 * Copyright (c) 2013-2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/mpu_iio.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/nct1008.h>
#include <linux/pid_thermal_gov.h>
#include <linux/tegra-fuse.h>
#include <linux/of_platform.h>
#include <mach/edp.h>
#include <mach/io_dpd.h>
#include <media/camera.h>
#include <media/imx179.h>
#include <media/ov5693.h>
#include <media/ad5823.h>

#include <linux/platform_device.h>
#include <media/soc_camera.h>
#include <media/soc_camera_platform.h>
#include <media/tegra_v4l2_camera.h>

#include <linux/platform/tegra/cpu-tegra.h>
#include "devices.h"
#include "board.h"
#include "board-common.h"
#include "board-ardbeg.h"
#include "tegra-board-id.h"

#if defined(ARCH_TEGRA_12x_SOC)
static struct i2c_board_info ardbeg_i2c_board_info_cm32181[] = {
	{
		I2C_BOARD_INFO("cm32181", 0x48),
	},
};
#endif

/*
 * Soc Camera platform driver for testing
 */
#if IS_ENABLED(CONFIG_SOC_CAMERA_PLATFORM)
static int ardbeg_soc_camera_add(struct soc_camera_device *icd);
static void ardbeg_soc_camera_del(struct soc_camera_device *icd);

static int ardbeg_soc_camera_set_capture(struct soc_camera_platform_info *info,
		int enable)
{
	/* TODO: probably add clk opertaion here */
	return 0; /* camera sensor always enabled */
}

static struct soc_camera_platform_info ardbeg_soc_camera_info = {
	.format_name = "RGB4",
	.format_depth = 32,
	.format = {
		.code = V4L2_MBUS_FMT_RGBA8888_4X8_LE,
		.colorspace = V4L2_COLORSPACE_SRGB,
		.field = V4L2_FIELD_NONE,
		.width = 1280,
		.height = 720,
	},
	.set_capture = ardbeg_soc_camera_set_capture,
};

static struct tegra_camera_platform_data ardbeg_camera_platform_data = {
	.flip_v                 = 0,
	.flip_h                 = 0,
	.port                   = TEGRA_CAMERA_PORT_CSI_A,
	.lanes                  = 4,
	.continuous_clk         = 0,
};

static struct soc_camera_link ardbeg_soc_camera_link = {
	.bus_id         = 0,
	.add_device     = ardbeg_soc_camera_add,
	.del_device     = ardbeg_soc_camera_del,
	.module_name    = "soc_camera_platform",
	.priv		= &ardbeg_camera_platform_data,
	.dev_priv	= &ardbeg_soc_camera_info,
};

static struct platform_device *ardbeg_pdev;

static void ardbeg_soc_camera_release(struct device *dev)
{
	soc_camera_platform_release(&ardbeg_pdev);
}

static int ardbeg_soc_camera_add(struct soc_camera_device *icd)
{
	return soc_camera_platform_add(icd, &ardbeg_pdev,
			&ardbeg_soc_camera_link,
			ardbeg_soc_camera_release, 0);
}

static void ardbeg_soc_camera_del(struct soc_camera_device *icd)
{
	soc_camera_platform_del(icd, ardbeg_pdev, &ardbeg_soc_camera_link);
}

static struct platform_device ardbeg_soc_camera_device = {
	.name   = "soc-camera-pdrv",
	.id     = 0,
	.dev    = {
		.platform_data = &ardbeg_soc_camera_link,
	},
};
#endif

#if IS_ENABLED(CONFIG_SOC_CAMERA_IMX135)
static int ardbeg_imx135_power(struct device *dev, int enable)
{
	return 0;
}

struct imx135_platform_data ardbeg_imx135_data;

static struct i2c_board_info ardbeg_imx135_camera_i2c_device = {
	I2C_BOARD_INFO("imx135_v4l2", 0x10),
	.platform_data = &ardbeg_imx135_data,
};

static struct tegra_camera_platform_data ardbeg_imx135_camera_platform_data = {
	.flip_v			= 0,
	.flip_h			= 0,
	.port			= TEGRA_CAMERA_PORT_CSI_A,
	.lanes			= 4,
	.continuous_clk		= 0,
};

static struct soc_camera_link imx135_iclink = {
	.bus_id		= 0, /* This must match the .id of tegra_vi01_device */
	.board_info	= &ardbeg_imx135_camera_i2c_device,
	.module_name	= "imx135_v4l2",
	.i2c_adapter_id	= 2,
	.power		= ardbeg_imx135_power,
	.priv		= &ardbeg_imx135_camera_platform_data,
};

static struct platform_device ardbeg_imx135_soc_camera_device = {
	.name	= "soc-camera-pdrv",
	.id	= 0,
	.dev	= {
		.platform_data = &imx135_iclink,
	},
};
#endif

#if IS_ENABLED(CONFIG_SOC_CAMERA_AR0261)
static int ardbeg_ar0261_power(struct device *dev, int enable)
{
	return 0;
}

struct ar0261_platform_data ardbeg_ar0261_data;

static struct i2c_board_info ardbeg_ar0261_camera_i2c_device = {
	I2C_BOARD_INFO("ar0261_v4l2", 0x36),
	.platform_data = &ardbeg_ar0261_data,
};

static struct tegra_camera_platform_data ardbeg_ar0261_camera_platform_data = {
	.flip_v			= 0,
	.flip_h			= 0,
	.port			= TEGRA_CAMERA_PORT_CSI_B,
	.lanes			= 1,
	.continuous_clk		= 0,
};

static struct soc_camera_link ar0261_iclink = {
	.bus_id		= 1, /* This must match the .id of tegra_vi01_device */
	.board_info	= &ardbeg_ar0261_camera_i2c_device,
	.module_name	= "ar0261_v4l2",
	.i2c_adapter_id	= 2,
	.power		= ardbeg_ar0261_power,
	.priv		= &ardbeg_ar0261_camera_platform_data,
};

static struct platform_device ardbeg_ar0261_soc_camera_device = {
	.name	= "soc-camera-pdrv",
	.id	= 1,
	.dev	= {
		.platform_data = &ar0261_iclink,
	},
};
#endif

static struct regulator *ov5693_1v2;
static struct regulator *ov5693_1v8;
static struct regulator *ov5693_afvdd;

static int ardbeg_get_extra_regulators(void)
{
	if (!ov5693_1v2) {
		ov5693_1v2 = regulator_get(NULL, "vdd_cam_1v2");
		if (WARN_ON(IS_ERR(ov5693_1v2))) {
			pr_err("%s: can't get regulator ov5693_1v2: %ld\n",
				__func__, PTR_ERR(ov5693_1v2));
			ov5693_1v2 = NULL;
			return -ENODEV;
		}
	}

	if (!ov5693_1v8) {
		ov5693_1v8 = regulator_get(NULL, "vdd_cam_1v8");
		if (WARN_ON(IS_ERR(ov5693_1v8))) {
			pr_err("%s: can't get regulator ov5693_1v8: %ld\n",
				__func__, PTR_ERR(ov5693_1v8));
			ov5693_1v8 = NULL;
			return -ENODEV;
		}
	}
	if (!ov5693_afvdd) {
		ov5693_afvdd = regulator_get(NULL, "imx179_reg1");
		if (WARN_ON(IS_ERR(ov5693_afvdd))) {
			pr_err("%s: can't get regulator ov5693_afvdd: %ld\n",
				__func__, PTR_ERR(ov5693_afvdd));
			ov5693_afvdd = NULL;
			return -ENODEV;
		}
	}

	return 0;
}

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

static struct tegra_io_dpd csie_io = {
	.name			= "CSIE",
	.io_dpd_reg_index	= 1,
	.io_dpd_bit		= 12,
};

static int ardbeg_imx179_get_extra_regulators(struct imx179_power_rail *pw)
{
	if (!pw->ext_reg1) {
		pw->ext_reg1 = regulator_get(NULL, "imx179_reg1");
		if (WARN_ON(IS_ERR(pw->ext_reg1))) {
			pr_err("%s: can't get regulator imx179_reg1: %ld\n",
				__func__, PTR_ERR(pw->ext_reg1));
			pw->ext_reg1 = NULL;
			return -ENODEV;
		}
	}

	if (!pw->ext_reg2) {
		pw->ext_reg2 = regulator_get(NULL, "vdd_cam_1v2");
		if (WARN_ON(IS_ERR(pw->ext_reg2))) {
			pr_err("%s: can't get regulator imx179_reg2: %ld\n",
				__func__, PTR_ERR(pw->ext_reg2));
			pw->ext_reg2 = NULL;
			return -ENODEV;
		}
	}

	if (!pw->ext_reg3) {
		pw->ext_reg3 = regulator_get(NULL, "vdd_cam_1v8");
		if (WARN_ON(IS_ERR(pw->ext_reg3))) {
			pr_err("%s: can't get regulator imx179_reg3: %ld\n",
				__func__, PTR_ERR(pw->ext_reg3));
			pw->ext_reg3 = NULL;
			return -ENODEV;
		}
	}

	return 0;
}

static int ardbeg_imx179_power_on(struct imx179_power_rail *pw)
{
	int err;
	pr_info("%s\n", __func__);
	if (unlikely(WARN_ON(!pw || !pw->iovdd || !pw->avdd)))
		return -EFAULT;

	/* disable CSIA/B IOs DPD mode to turn on camera for ardbeg */
	tegra_io_dpd_disable(&csia_io);
	tegra_io_dpd_disable(&csib_io);

	if (ardbeg_imx179_get_extra_regulators(pw))
		goto imx179_poweron_fail;

	err = regulator_enable(pw->ext_reg1);
	if (unlikely(err))
		goto imx179_ext_reg1_fail;

	err = regulator_enable(pw->ext_reg2);
	if (unlikely(err))
		goto imx179_ext_reg2_fail;

	err = regulator_enable(pw->ext_reg3);
	if (unlikely(err))
		goto imx179_ext_reg3_fail;

	err = regulator_enable(pw->avdd);
	if (err)
		goto imx179_avdd_fail;

	usleep_range(1, 2);

	gpio_set_value(CAM_AF_PWDN, 1);
	gpio_set_value(CAM_RSTN, 0);
	usleep_range(10, 20);

	gpio_set_value(CAM_RSTN, 1);

	usleep_range(300, 310);
	pr_info("%s finished\n", __func__);
	return 0;



imx179_avdd_fail:
	if (pw->ext_reg3)
		regulator_disable(pw->ext_reg3);

imx179_ext_reg3_fail:
	if (pw->ext_reg2)
		regulator_disable(pw->ext_reg2);
	gpio_set_value(CAM_AF_PWDN, 0);

imx179_ext_reg2_fail:
	if (pw->ext_reg1)
		regulator_disable(pw->ext_reg1);
	gpio_set_value(CAM_AF_PWDN, 0);

imx179_ext_reg1_fail:
imx179_poweron_fail:
	tegra_io_dpd_enable(&csia_io);
	tegra_io_dpd_enable(&csib_io);
	pr_err("%s failed.\n", __func__);
	return -ENODEV;
}

static int ardbeg_imx179_power_off(struct imx179_power_rail *pw)
{
	if (unlikely(WARN_ON(!pw || !pw->iovdd || !pw->avdd))) {
		tegra_io_dpd_enable(&csia_io);
		tegra_io_dpd_enable(&csib_io);
		return -EFAULT;
	}
	gpio_set_value(CAM_RSTN, 0);
	gpio_set_value(CAM_AF_PWDN, 0);
	usleep_range(1, 2);
	regulator_disable(pw->avdd);
	regulator_disable(pw->ext_reg1);
	regulator_disable(pw->ext_reg2);
	regulator_disable(pw->ext_reg3);

	/* put CSIA/B IOs into DPD mode to save additional power for ardbeg */
	tegra_io_dpd_enable(&csia_io);
	tegra_io_dpd_enable(&csib_io);
	pr_info("%s finished\n", __func__);
	return 0;
}

struct imx179_platform_data ardbeg_imx179_pdata = {
	.power_on = ardbeg_imx179_power_on,
	.power_off = ardbeg_imx179_power_off,
};

static int ardbeg_ov5693_power_on(struct ov5693_power_rail *pw)
{
	int err;
	pr_info("%s\n", __func__);
	if (unlikely(WARN_ON(!pw || !pw->dovdd || !pw->avdd)))
		return -EFAULT;

	/* disable CSIA/B IOs DPD mode to turn on camera for ardbeg */
	tegra_io_dpd_disable(&csie_io);

	if (ardbeg_get_extra_regulators())
		goto ov5693_poweron_fail;

	gpio_set_value(CAM2_PWDN, 0);
	gpio_set_value(CAM2_RSTN, 0);
	gpio_set_value(CAM_AF_PWDN, 0);
	usleep_range(10, 20);

	err = regulator_enable(ov5693_afvdd);
	if (err)
		goto ov5693_afvdd_fail;

	err = regulator_enable(pw->avdd);
	if (err)
		goto ov5693_avdd_fail;


	err = regulator_enable(ov5693_1v8);
	if (err)
		goto ov5693_1v8_fail;

	gpio_set_value(CAM2_PWDN, 1);

	err = regulator_enable(ov5693_1v2);
	if (err)
		goto ov5693_1v2_fail;


	udelay(2);
	gpio_set_value(CAM2_RSTN, 1);

	usleep_range(300, 310);

	pr_info("%s finished\n", __func__);
	return 0;

ov5693_afvdd_fail:
	regulator_disable(pw->avdd);

ov5693_avdd_fail:
	regulator_disable(ov5693_1v2);

ov5693_1v2_fail:
	regulator_disable(ov5693_1v8);

ov5693_1v8_fail:
	gpio_set_value(CAM2_PWDN, 0);
	gpio_set_value(CAM2_RSTN, 0);

ov5693_poweron_fail:
	/* put CSIA/B IOs into DPD mode to save additional power for ardbeg */
	tegra_io_dpd_enable(&csia_io);
	tegra_io_dpd_enable(&csib_io);
	pr_err("%s FAILED\n", __func__);
	return -ENODEV;
}

static int ardbeg_ov5693_power_off(struct ov5693_power_rail *pw)
{
	pr_info("%s\n", __func__);
	if (unlikely(WARN_ON(!pw || !pw->dovdd || !pw->avdd))) {
		tegra_io_dpd_enable(&csie_io);
		return -EFAULT;
	}

	usleep_range(21, 25);
	gpio_set_value(CAM2_RSTN, 0);
	udelay(2);

	regulator_disable(ov5693_1v2);
	regulator_disable(ov5693_1v8);
	gpio_set_value(CAM2_PWDN, 0);
	gpio_set_value(CAM_AF_PWDN, 0);
	regulator_disable(pw->avdd);
	regulator_disable(ov5693_afvdd);
	tegra_io_dpd_enable(&csie_io);

	/* put CSIA/B IOs into DPD mode to save additional power for ardbeg */
	tegra_io_dpd_enable(&csia_io);
	tegra_io_dpd_enable(&csib_io);
	pr_info("%s finished\n", __func__);
	return 0;
}

static struct nvc_gpio_pdata ov5693_gpio_pdata[] = {
	{ OV5693_GPIO_TYPE_PWRDN, CAM2_RSTN, true, 0, },
};

static struct ov5693_platform_data ardbeg_ov5693_pdata = {
	.gpio_count	= ARRAY_SIZE(ov5693_gpio_pdata),
	.gpio		= ov5693_gpio_pdata,
	.power_on	= ardbeg_ov5693_power_on,
	.power_off	= ardbeg_ov5693_power_off,
	.mclk_name	= "mclk2",
};

static int ardbeg_ad5823_power_on(struct ad5823_platform_data *pdata)
{
	int err = 0;

	pr_info("%s\n", __func__);
	gpio_set_value_cansleep(pdata->gpio, 1);
	pdata->pwr_dev = AD5823_PWR_DEV_ON;

	return err;
}

static int ardbeg_ad5823_power_off(struct ad5823_platform_data *pdata)
{
	pr_info("%s\n", __func__);
	gpio_set_value_cansleep(pdata->gpio, 0);
	pdata->pwr_dev = AD5823_PWR_DEV_OFF;

	return 0;
}

static struct ad5823_platform_data ardbeg_ad5823_pdata = {
	.gpio = CAM_AF_PWDN,
	.power_on	= ardbeg_ad5823_power_on,
	.power_off	= ardbeg_ad5823_power_off,
	.support_mfi = true,
};

static struct camera_data_blob ardbeg_camera_lut[] = {
	{},
};

static struct i2c_board_info ardbeg_camera_board_info[] = {
	{
		I2C_BOARD_INFO("imx179", 0x10),
		.platform_data = &ardbeg_imx179_pdata,
	},
	{
		I2C_BOARD_INFO("ad5823", 0x0c),
		.platform_data = &ardbeg_ad5823_pdata,
	},
	{
		I2C_BOARD_INFO("ov5693", 0x36),
		.platform_data = &ardbeg_ov5693_pdata,
	},
};

void __init ardbeg_camera_auxdata(void *data)
{
	struct of_dev_auxdata *aux_lut = data;
	while (aux_lut && aux_lut->compatible) {
		if (!strcmp(aux_lut->compatible, "nvidia,tegra124-camera")) {
			pr_info("%s: update camera lookup table.\n", __func__);
			aux_lut->platform_data = ardbeg_camera_lut;
		}
		aux_lut++;
	}
}

static int ardbeg_camera_init(void)
{
	struct board_info board_info;

	pr_debug("%s: ++\n", __func__);
	tegra_get_board_info(&board_info);

	/* put CSIA/B/C/D/E IOs into DPD mode to
	 * save additional power for ardbeg
	 */
	tegra_io_dpd_enable(&csia_io);
	tegra_io_dpd_enable(&csib_io);
	tegra_io_dpd_enable(&csie_io);

	i2c_register_board_info(2, ardbeg_camera_board_info,
		ARRAY_SIZE(ardbeg_camera_board_info));

#if IS_ENABLED(CONFIG_SOC_CAMERA_PLATFORM)
	platform_device_register(&ardbeg_soc_camera_device);
#endif

#if IS_ENABLED(CONFIG_SOC_CAMERA_IMX135)
	platform_device_register(&ardbeg_imx135_soc_camera_device);
#endif

#if IS_ENABLED(CONFIG_SOC_CAMERA_AR0261)
	platform_device_register(&ardbeg_ar0261_soc_camera_device);
#endif
	return 0;
}

static struct pid_thermal_gov_params cpu_pid_params = {
	.max_err_temp = 4000,
	.max_err_gain = 1000,

	.gain_p = 1000,
	.gain_d = 0,

	.up_compensation = 15,
	.down_compensation = 15,
};

static struct thermal_zone_params cpu_tzp = {
	.governor_name = "pid_thermal_gov",
	.governor_params = &cpu_pid_params,
};

static struct thermal_zone_params board_tzp = {
	.governor_name = "step_wise"
};

static struct nct1008_platform_data ardbeg_nct72_pdata = {
	.loc_name = "tegra",
	.supported_hwrev = true,
	.conv_rate = 0x06, /* 4Hz conversion rate */
	.offset = 0,
	.extended_range = true,

	.sensors = {
		[LOC] = {
			.tzp = &board_tzp,
			.shutdown_limit = 120, /* C */
			.passive_delay = 1000,
			.num_trips = 1,
			.trips = {
				{
					.cdev_type = "therm_est_activ",
					.trip_temp = 40000,
					.trip_type = THERMAL_TRIP_ACTIVE,
					.hysteresis = 1000,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
					.mask = 1,
				},
			},
		},
		[EXT] = {
			.tzp = &cpu_tzp,
			.shutdown_limit = 95, /* C */
			.passive_delay = 1000,
			.num_trips = 2,
			.trips = {
				{
					.cdev_type = "shutdown_warning",
					.trip_temp = 93000,
					.trip_type = THERMAL_TRIP_PASSIVE,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
					.mask = 0,
				},
				{
					.cdev_type = "cpu-balanced",
					.trip_temp = 83000,
					.trip_type = THERMAL_TRIP_PASSIVE,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
					.hysteresis = 1000,
					.mask = 1,
				},
			}
		}
	}
};

#ifdef CONFIG_TEGRA_SKIN_THROTTLE
static struct pid_thermal_gov_params skin_pid_params = {
	.max_err_temp = 4000,
	.max_err_gain = 1000,

	.gain_p = 1000,
	.gain_d = 0,

	.up_compensation = 15,
	.down_compensation = 15,
};

static struct thermal_zone_params skin_tzp = {
	.governor_name = "pid_thermal_gov",
	.governor_params = &skin_pid_params,
};

static struct nct1008_platform_data ardbeg_nct72_tskin_pdata = {
	.loc_name = "skin",

	.supported_hwrev = true,
	.conv_rate = 0x06, /* 4Hz conversion rate */
	.offset = 0,
	.extended_range = true,

	.sensors = {
		[LOC] = {
			.shutdown_limit = 95, /* C */
			.num_trips = 0,
			.tzp = NULL,
		},
		[EXT] = {
			.shutdown_limit = 85, /* C */
			.passive_delay = 10000,
			.polling_delay = 1000,
			.tzp = &skin_tzp,
			.num_trips = 1,
			.trips = {
				{
					.cdev_type = "skin-balanced",
					.trip_temp = 50000,
					.trip_type = THERMAL_TRIP_PASSIVE,
					.upper = THERMAL_NO_LIMIT,
					.lower = THERMAL_NO_LIMIT,
					.mask = 1,
				},
			},
		}
	}
};
#endif

static struct i2c_board_info ardbeg_i2c_nct72_board_info[] = {
	{
		I2C_BOARD_INFO("nct72", 0x4c),
		.platform_data = &ardbeg_nct72_pdata,
		.irq = -1,
	},
#ifdef CONFIG_TEGRA_SKIN_THROTTLE
	{
		I2C_BOARD_INFO("nct72", 0x4d),
		.platform_data = &ardbeg_nct72_tskin_pdata,
		.irq = -1,
	}
#endif
};

static int ardbeg_nct72_init(void)
{
	int nct72_port = TEGRA_GPIO_PI6;
	int ret = 0;
	int i;
	struct thermal_trip_info *trip_state;
	struct board_info board_info;

	tegra_get_board_info(&board_info);
	/* raise NCT's thresholds if soctherm CP,FT fuses are ok */
	if ((tegra_fuse_calib_base_get_cp(NULL, NULL) >= 0) &&
	    (tegra_fuse_calib_base_get_ft(NULL, NULL) >= 0)) {
		ardbeg_nct72_pdata.sensors[EXT].shutdown_limit += 20;
		for (i = 0; i < ardbeg_nct72_pdata.sensors[EXT].num_trips;
			 i++) {
			trip_state = &ardbeg_nct72_pdata.sensors[EXT].trips[i];
			if (!strncmp(trip_state->cdev_type, "cpu-balanced",
					THERMAL_NAME_LENGTH)) {
				trip_state->cdev_type = "_none_";
				break;
			}
		}
	} else {
		tegra_platform_edp_init(
			ardbeg_nct72_pdata.sensors[EXT].trips,
			&ardbeg_nct72_pdata.sensors[EXT].num_trips,
					12000); /* edp temperature margin */
		tegra_add_cpu_vmax_trips(
			ardbeg_nct72_pdata.sensors[EXT].trips,
			&ardbeg_nct72_pdata.sensors[EXT].num_trips);
		tegra_add_tgpu_trips(
			ardbeg_nct72_pdata.sensors[EXT].trips,
			&ardbeg_nct72_pdata.sensors[EXT].num_trips);
		tegra_add_vc_trips(
			ardbeg_nct72_pdata.sensors[EXT].trips,
			&ardbeg_nct72_pdata.sensors[EXT].num_trips);
		tegra_add_core_vmax_trips(
			ardbeg_nct72_pdata.sensors[EXT].trips,
			&ardbeg_nct72_pdata.sensors[EXT].num_trips);
	}

	/* vmin trips are bound to soctherm on norrin and bowmore */
	if (!(board_info.board_id == BOARD_PM374 ||
		board_info.board_id == BOARD_E2141 ||
		board_info.board_id == BOARD_E1971 ||
		board_info.board_id == BOARD_E1991))
		tegra_add_all_vmin_trips(ardbeg_nct72_pdata.sensors[EXT].trips,
			&ardbeg_nct72_pdata.sensors[EXT].num_trips);

	/* T210_interposer use GPIO_PC7 for alert*/
	if (board_info.board_id == BOARD_E2141)
		nct72_port = TEGRA_GPIO_PC7;

	ardbeg_i2c_nct72_board_info[0].irq = gpio_to_irq(nct72_port);

	ret = gpio_request(nct72_port, "temp_alert");
	if (ret < 0)
		return ret;

	ret = gpio_direction_input(nct72_port);
	if (ret < 0) {
		pr_info("%s: calling gpio_free(nct72_port)", __func__);
		gpio_free(nct72_port);
	}

	/* norrin has thermal sensor on GEN1-I2C i.e. instance 0 */
	if (board_info.board_id == BOARD_PM374)
		i2c_register_board_info(0, ardbeg_i2c_nct72_board_info,
					1); /* only register device[0] */
	/* ardbeg has thermal sensor on GEN2-I2C i.e. instance 1 */
	else if (board_info.board_id == BOARD_PM358 ||
			board_info.board_id == BOARD_PM359 ||
			board_info.board_id == BOARD_PM370 ||
			board_info.board_id == BOARD_PM363)
		i2c_register_board_info(1, ardbeg_i2c_nct72_board_info,
		ARRAY_SIZE(ardbeg_i2c_nct72_board_info));
	else if (board_info.board_id == BOARD_PM375) {
		ardbeg_nct72_pdata.sensors[EXT].shutdown_limit = 100;
		ardbeg_nct72_pdata.sensors[LOC].shutdown_limit = 95;
		i2c_register_board_info(0, ardbeg_i2c_nct72_board_info,
					1); /* only register device[0] */
	}
	else
		i2c_register_board_info(0, ardbeg_i2c_nct72_board_info,
			ARRAY_SIZE(ardbeg_i2c_nct72_board_info));

	return ret;
}

int __init ardbeg_sensors_init(void)
{
	struct board_info board_info;
	tegra_get_board_info(&board_info);
	ardbeg_camera_init();

	if (board_info.board_id == BOARD_P1761 ||
		board_info.board_id == BOARD_E1780 ||
		board_info.board_id == BOARD_E1784 ||
		board_info.board_id == BOARD_E1971 ||
		board_info.board_id == BOARD_E1991 ||
		board_info.board_id == BOARD_E1922 ||
		of_machine_is_compatible("nvidia,green-arrow")) {
		/* Sensor is on DT */
		pr_warn("Temp sensor are from DT\n");
	} else
		ardbeg_nct72_init();

#if defined(ARCH_TEGRA_12x_SOC)
	/* TN8 and PM359 don't have ALS CM32181 */
	if (!of_machine_is_compatible("nvidia,tn8") &&
		!of_machine_is_compatible("nvidia,green-arrow") &&
		board_info.board_id != BOARD_PM359 &&
		board_info.board_id != BOARD_PM375)
		i2c_register_board_info(0, ardbeg_i2c_board_info_cm32181,
			ARRAY_SIZE(ardbeg_i2c_board_info_cm32181));
#endif
	return 0;
}
