/*
 * arch/arm/mach-tegra/board-ardbeg-sdhci.c
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/resource.h>
#include <linux/platform_device.h>
#include <linux/wlan_plat.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/mmc/host.h>
#include <linux/wl12xx.h>
#include <linux/platform_data/mmc-sdhci-tegra.h>
#include <linux/mfd/max77660/max77660-core.h>
#include <linux/tegra-fuse.h>
#include <linux/dma-mapping.h>
#include <linux/clk/tegra.h>

#include <asm/mach-types.h>
#include <mach/irqs.h>
#include <mach/nct.h>

#include "gpio-names.h"
#include "board.h"
#include "board-ardbeg.h"
#include "iomap.h"
#include "tegra-board-id.h"

#define ARDBEG_WLAN_RST	TEGRA_GPIO_PCC5
#define ARDBEG_WLAN_PWR	TEGRA_GPIO_PX7
#define ARDBEG_WLAN_WOW	TEGRA_GPIO_PU5

#define ARDBEG_SD_CD	TEGRA_GPIO_PV2
#define ARDBEG_SD_WP	TEGRA_GPIO_PQ4
#define FUSE_SOC_SPEEDO_0	0x134

static void (*wifi_status_cb)(int card_present, void *dev_id);
static void *wifi_status_cb_devid;
static int ardbeg_wifi_status_register(void (*callback)(int , void *), void *);

static struct resource sdhci_resource0[] = {
	[0] = {
		.start  = INT_SDMMC1,
		.end    = INT_SDMMC1,
		.flags  = IORESOURCE_IRQ,
	},
	[1] = {
		.start	= TEGRA_SDMMC1_BASE,
		.end	= TEGRA_SDMMC1_BASE + TEGRA_SDMMC1_SIZE-1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct resource sdhci_resource2[] = {
	[0] = {
		.start  = INT_SDMMC3,
		.end    = INT_SDMMC3,
		.flags  = IORESOURCE_IRQ,
	},
	[1] = {
		.start	= TEGRA_SDMMC3_BASE,
		.end	= TEGRA_SDMMC3_BASE + TEGRA_SDMMC3_SIZE-1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct resource sdhci_resource3[] = {
	[0] = {
		.start  = INT_SDMMC4,
		.end    = INT_SDMMC4,
		.flags  = IORESOURCE_IRQ,
	},
	[1] = {
		.start	= TEGRA_SDMMC4_BASE,
		.end	= TEGRA_SDMMC4_BASE + TEGRA_SDMMC4_SIZE-1,
		.flags	= IORESOURCE_MEM,
	},
};

#ifdef CONFIG_MMC_EMBEDDED_SDIO
static struct embedded_sdio_data embedded_sdio_data0 = {
	.cccr   = {
		.sdio_vsn       = 2,
		.multi_block    = 1,
		.low_speed      = 0,
		.wide_bus       = 0,
		.high_power     = 1,
		.high_speed     = 1,
	},
	.cis  = {
		.vendor	 = 0x02d0,
		.device	 = 0x4329,
	},
};
#endif

static u64 tegra_sdhci_dmamask = DMA_BIT_MASK(64);

static struct tegra_sdhci_platform_data tegra_sdhci_platform_data0 = {
	.mmc_data = {
		.register_status_notify	= ardbeg_wifi_status_register,
#ifdef CONFIG_MMC_EMBEDDED_SDIO
		.embedded_sdio = &embedded_sdio_data0,
#endif
		.built_in = 0,
		.ocr_mask = MMC_OCR_1V8_MASK,
	},
	.cd_gpio = -1,
	.wp_gpio = -1,
	.power_gpio = -1,
	.tap_delay = 0,
	.power_off_rail = true,
	.trim_delay = 0x2,
	.ddr_clk_limit = 41000000,
	.uhs_mask = MMC_UHS_MASK_DDR50,
	.calib_3v3_offsets = 0x7676,
	.calib_1v8_offsets = 0x7676,
	.max_clk_limit = 136000000,
};

static struct tegra_sdhci_platform_data tegra_sdhci_platform_data2 = {
	.cd_gpio = ARDBEG_SD_CD,
	.wp_gpio = -1,
	.power_gpio = -1,
	.tap_delay = 0,
	.power_off_rail = true,
	.trim_delay = 0x3,
	.uhs_mask = MMC_UHS_MASK_DDR50,
	.calib_3v3_offsets = 0x7676,
	.calib_1v8_offsets = 0x7676,
	.enb_ext_loopback = 1,
};

static struct tegra_sdhci_platform_data tegra_sdhci_platform_data3 = {
	.cd_gpio = -1,
	.wp_gpio = -1,
	.power_gpio = -1,
	.is_8bit = 1,
	.tap_delay = 0x4,
	.power_off_rail = true,
	.trim_delay = 0x4,
	.is_ddr_trim_delay = true,
	.ddr_trim_delay = 0x0,
	.mmc_data = {
		.built_in = 1,
		.ocr_mask = MMC_OCR_1V8_MASK,
	},
	.ddr_clk_limit = 51000000,
	.max_clk_limit = 200000000,
	.calib_3v3_offsets = 0x0202,
	.calib_1v8_offsets = 0x0202,
};

static struct platform_device tegra_sdhci_device0 = {
	.name		= "sdhci-tegra",
	.id		= 0,
	.resource	= sdhci_resource0,
	.num_resources	= ARRAY_SIZE(sdhci_resource0),
	.dev = {
		.dma_mask = &tegra_sdhci_dmamask,
		.coherent_dma_mask = DMA_BIT_MASK(64),
		.platform_data = &tegra_sdhci_platform_data0,
	},
};

static struct platform_device tegra_sdhci_device2 = {
	.name		= "sdhci-tegra",
	.id		= 2,
	.resource	= sdhci_resource2,
	.num_resources	= ARRAY_SIZE(sdhci_resource2),
	.dev = {
		.dma_mask = &tegra_sdhci_dmamask,
		.coherent_dma_mask = DMA_BIT_MASK(64),
		.platform_data = &tegra_sdhci_platform_data2,
	},
};

static struct platform_device tegra_sdhci_device3 = {
	.name		= "sdhci-tegra",
	.id		= 3,
	.resource	= sdhci_resource3,
	.num_resources	= ARRAY_SIZE(sdhci_resource3),
	.dev = {
		.dma_mask = &tegra_sdhci_dmamask,
		.coherent_dma_mask = DMA_BIT_MASK(64),
		.platform_data = &tegra_sdhci_platform_data3,
	},
};

static int ardbeg_wifi_status_register(
		void (*callback)(int card_present, void *dev_id),
		void *dev_id)
{
	if (wifi_status_cb)
		return -EAGAIN;
	wifi_status_cb = callback;
	wifi_status_cb_devid = dev_id;
	return 0;
}

int __init ardbeg_sdhci_init(void)
{
	int nominal_core_mv;
	int min_vcore_override_mv;
	int boot_vcore_mv;
	u32 speedo;
	struct board_info board_info;

	tegra_get_board_info(&board_info);
	
	/* We moved the sdchi init to dtb for mocha */
	if (board_info.board_id == BOARD_E1780)
		return 0;

	nominal_core_mv =
		tegra_dvfs_get_core_nominal_millivolts();
	if (nominal_core_mv) {
		tegra_sdhci_platform_data0.nominal_vcore_mv = nominal_core_mv;
		tegra_sdhci_platform_data2.nominal_vcore_mv = nominal_core_mv;
		tegra_sdhci_platform_data3.nominal_vcore_mv = nominal_core_mv;
	}
	min_vcore_override_mv =
		tegra_dvfs_get_core_override_floor();
	if (min_vcore_override_mv) {
		tegra_sdhci_platform_data0.min_vcore_override_mv =
			min_vcore_override_mv;
		tegra_sdhci_platform_data2.min_vcore_override_mv =
			min_vcore_override_mv;
		tegra_sdhci_platform_data3.min_vcore_override_mv =
			min_vcore_override_mv;
	}
	boot_vcore_mv = tegra_dvfs_get_core_boot_level();
	if (boot_vcore_mv) {
		tegra_sdhci_platform_data0.boot_vcore_mv = boot_vcore_mv;
		tegra_sdhci_platform_data2.boot_vcore_mv = boot_vcore_mv;
		tegra_sdhci_platform_data3.boot_vcore_mv = boot_vcore_mv;
	}

	if (of_machine_is_compatible("nvidia,laguna") ||
	    of_machine_is_compatible("nvidia,jetson-tk1"))
		tegra_sdhci_platform_data2.wp_gpio = ARDBEG_SD_WP;

	tegra_get_board_info(&board_info);
	if (board_info.board_id == BOARD_E1780 ||
			board_info.board_id == BOARD_P2267)
		tegra_sdhci_platform_data2.max_clk_limit = 204000000;

	/* E1780, E2141, E1784 are using interposer E1816, Due to this the
	 * SDIO trace length got increased. So hard coding the drive
	 * strength to type A for these boards to support 204 Mhz */
	if ((board_info.board_id == BOARD_E1780) ||
		(board_info.board_id == BOARD_E2141) ||
		(board_info.board_id == BOARD_E1784) ||
		(board_info.board_id == BOARD_P2267)) {
		tegra_sdhci_platform_data0.default_drv_type =
			MMC_SET_DRIVER_TYPE_A;
	}

	if (board_info.board_id == BOARD_P1761 ||
			board_info.board_id == BOARD_P2267)
		tegra_sdhci_platform_data0.max_clk_limit = 204000000;

	if (board_info.board_id == BOARD_E1781)
		tegra_sdhci_platform_data3.uhs_mask = MMC_MASK_HS200;

	if (board_info.board_id == BOARD_PM374 ||
		board_info.board_id == BOARD_PM358 ||
		board_info.board_id == BOARD_PM363 ||
		board_info.board_id == BOARD_PM359)
			tegra_sdhci_platform_data0.disable_clock_gate = 1;

	/*
	 * FIXME: Set max clk limit to 200MHz for SDMMC3 for PM375.
	 * Requesting 208MHz results in getting 204MHz from PLL_P
	 * and CRC errors are seen with same.
	 */
	if (board_info.board_id == BOARD_PM375)
		tegra_sdhci_platform_data2.max_clk_limit = 200000000;

	speedo = tegra_fuse_readl(FUSE_SOC_SPEEDO_0);
	tegra_sdhci_platform_data0.cpu_speedo = speedo;
	tegra_sdhci_platform_data2.cpu_speedo = speedo;
	tegra_sdhci_platform_data3.cpu_speedo = speedo;

	if (board_info.board_id == BOARD_E1991 ||
		board_info.board_id == BOARD_E1971) {
			tegra_sdhci_platform_data0.uhs_mask =
				MMC_UHS_MASK_SDR50 | MMC_UHS_MASK_DDR50;
			tegra_sdhci_platform_data2.uhs_mask =
				MMC_UHS_MASK_SDR50;
	}

	if (board_info.board_id == BOARD_E1991)
		tegra_sdhci_platform_data0.max_clk_limit = 204000000;

	if (board_info.board_id == BOARD_PM374 ||
		board_info.board_id == BOARD_PM359) {
			tegra_sdhci_platform_data2.uhs_mask =
				MMC_UHS_MASK_SDR50;
			tegra_sdhci_platform_data0.uhs_mask =
				MMC_UHS_MASK_SDR50;
			tegra_sdhci_platform_data3.max_clk_limit = 200000000;
			tegra_sdhci_platform_data2.max_clk_limit = 204000000;
	}

	/*
	 * To enable pm domain disable_clock_gate and
	 * enable_pm_domain should be set to one
	 */

	platform_device_register(&tegra_sdhci_device3);
	if (!is_uart_over_sd_enabled())
		platform_device_register(&tegra_sdhci_device2);
	if (board_info.board_id != BOARD_PM359 &&
			board_info.board_id != BOARD_PM375)
		platform_device_register(&tegra_sdhci_device0);

	return 0;
}
