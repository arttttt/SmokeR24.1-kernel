/*
 * arch/arm/mach-tegra/board-ardbeg-power.c
 *
 * Copyright (c) 2013-2015, NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/io.h>
#include <mach/edp.h>
#include <mach/irqs.h>
#include <linux/edp.h>
#include <linux/platform_data/tegra_edp.h>
#include <linux/pid_thermal_gov.h>
#include <linux/regulator/fixed.h>
#include <linux/regulator/machine.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/regulator/tegra-dfll-bypass-regulator.h>
#include <linux/tegra-fuse.h>
#include <linux/tegra-pmc.h>
#include <linux/pinctrl/pinconf-tegra.h>

#include <asm/mach-types.h>
#include <linux/tegra_soctherm.h>

#include "pm.h"
#include <linux/platform/tegra/dvfs.h>
#include "board.h"
#include <linux/platform/tegra/common.h>
#include "tegra-board-id.h"
#include "board-pmu-defines.h"
#include "board-common.h"
#include "board-ardbeg.h"
#include "board-pmu-defines.h"
#include "devices.h"
#include "iomap.h"
#include <linux/platform/tegra/tegra_cl_dvfs.h>

#define E1735_EMULATE_E1767_SKU	1001
static struct tegra_suspend_platform_data ardbeg_suspend_data = {
	.cpu_timer      = 500,
	.cpu_off_timer  = 300,
	.cpu_suspend_freq = 1044000,
	.suspend_mode   = TEGRA_SUSPEND_LP0,
	.core_timer     = 0x157e,
	.core_off_timer = 10,
	.corereq_high   = true,
	.sysclkreq_high = true,
	.cpu_lp2_min_residency = 1000,
	.min_residency_vmin_fmin = 1000,
	.min_residency_ncpu_fast = 8000,
	.min_residency_ncpu_slow = 5000,
	.min_residency_mclk_stop = 5000,
	.min_residency_crail = 20000,
#ifdef CONFIG_TEGRA_LP1_LOW_COREVOLTAGE
	.lp1_lowvolt_support = true,
	.i2c_base_addr = TEGRA_I2C5_BASE,
	.pmuslave_addr = 0xB0,
	.core_reg_addr = 0x33,
	.lp1_core_volt_low_cold = 0x33,
	.lp1_core_volt_low = 0x33,
	.lp1_core_volt_high = 0x42,
#endif
};

#ifdef CONFIG_REGULATOR_TEGRA_DFLL_BYPASS
static void e1735_suspend_dfll_bypass(void)
{
	__gpio_set_value(TEGRA_GPIO_PS5, 1); /* tristate external PWM buffer */
}

static void e1735_resume_dfll_bypass(void)
{
	__gpio_set_value(TEGRA_GPIO_PS5, 0); /* enable PWM buffer operations */
}

static void e1767_configure_dvfs_pwm_tristate(const char *gname, int tristate)
{
	struct pinctrl_dev *pctl_dev = NULL;
	unsigned long config;
	int ret;

	pctl_dev = tegra_get_pinctrl_device_handle();
	if (!pctl_dev)
		return;

	config = TEGRA_PINCONF_PACK(TEGRA_PINCONF_PARAM_TRISTATE, tristate);
	ret = pinctrl_set_config_for_group_name(pctl_dev, gname, config);
	if (ret < 0)
		pr_err("%s(): ERROR: Not able to set %s to TRISTATE %d: %d\n",
			__func__, gname, tristate, ret);
}

static void e1767_suspend_dfll_bypass(void)
{
	e1767_configure_dvfs_pwm_tristate("dvfs_pwm_px0", TEGRA_PIN_ENABLE);
}

static void e1767_resume_dfll_bypass(void)
{
	e1767_configure_dvfs_pwm_tristate("dvfs_pwm_px0", TEGRA_PIN_DISABLE);
}
#endif

int __init ardbeg_suspend_init(void)
{
	struct board_info pmu_board_info;

	tegra_get_pmu_board_info(&pmu_board_info);

	if (pmu_board_info.board_id == BOARD_E1735) {
		struct tegra_suspend_platform_data *data = &ardbeg_suspend_data;
		if (pmu_board_info.sku != E1735_EMULATE_E1767_SKU) {
			data->cpu_timer = 2000;
			data->crail_up_early = true;
#ifdef CONFIG_REGULATOR_TEGRA_DFLL_BYPASS
			data->suspend_dfll_bypass = e1735_suspend_dfll_bypass;
			data->resume_dfll_bypass = e1735_resume_dfll_bypass;
		} else {
			data->suspend_dfll_bypass = e1767_suspend_dfll_bypass;
			data->resume_dfll_bypass = e1767_resume_dfll_bypass;
#endif
		}
	}

	tegra_init_suspend(&ardbeg_suspend_data);
	return 0;
}
