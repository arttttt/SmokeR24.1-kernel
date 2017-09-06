/*
 * drivers/video/tegra/dc/dpaux.c
 *
 * Copyright (c) 2014 - 2017, NVIDIA CORPORATION, All rights reserved.
 * Author: Animesh Kishore <ankishore@nvidia.com>
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

#include <linux/mutex.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/delay.h>

#include "dpaux_regs.h"
#include "dc_priv.h"
#include "dpaux.h"
#include "../../../../arch/arm/mach-tegra/iomap.h"

static const char *const dpaux_clks[TEGRA_DPAUX_INSTANCE_N] = {
	 "dpaux",
	 "dpaux1",
};

#if !defined(CONFIG_TEGRA_NVDISPLAY)
static unsigned long dpaux_base_addr[TEGRA_DPAUX_INSTANCE_N] = {
	TEGRA_DPAUX_BASE,
	TEGRA_DPAUX1_BASE,
};
#endif

static void __iomem *dpaux_baseaddr[TEGRA_DPAUX_INSTANCE_N];

static DEFINE_MUTEX(dpaux_lock);

static inline struct clk *tegra_dpaux_clk_get(struct device_node *np,
					enum tegra_dpaux_instance id)
{
	if (id >= TEGRA_DPAUX_INSTANCE_N)
		return ERR_PTR(-EINVAL);
#ifdef CONFIG_TEGRA_NVDISPLAY
	return tegra_disp_of_clk_get_by_name(np, dpaux_clks[id]);
#else
	return clk_get_sys(dpaux_clks[id], NULL);
#endif
}

int tegra_dpaux_clk_en(struct device_node *np, enum tegra_dpaux_instance id)
{
	return tegra_disp_clk_prepare_enable(tegra_dpaux_clk_get(np, id));
}

void tegra_dpaux_clk_dis(struct device_node *np, enum tegra_dpaux_instance id)
{
	tegra_disp_clk_disable_unprepare(tegra_dpaux_clk_get(np, id));
}

static inline void _tegra_dpaux_pad_power(struct tegra_dc *dc,
					enum tegra_dpaux_instance id, bool on)
{
	void __iomem *regaddr;
#if !defined(CONFIG_TEGRA_NVDISPLAY)
	regaddr = IO_ADDRESS(dpaux_base_addr[id] + DPAUX_HYBRID_SPARE * 4);
#else
	regaddr = dpaux_baseaddr[id] + DPAUX_HYBRID_SPARE * 4;
#endif
	writel((on ? DPAUX_HYBRID_SPARE_PAD_PWR_POWERUP :
		DPAUX_HYBRID_SPARE_PAD_PWR_POWERDOWN),
		regaddr);
}

void tegra_dpaux_pad_power(struct tegra_dc *dc,
				enum tegra_dpaux_instance id, bool on)
{
	struct device_node *np_dp =
		(dc->ndev->id) ? of_find_node_by_path(DPAUX1_NODE)
		: of_find_node_by_path(DPAUX_NODE);

	if (!np_dp) {
		dev_err(&dc->ndev->dev, "dp node not available\n");
		return;
	}

	/* make sure host1x is clocked before dpaux clk access */
	tegra_dc_io_start(dc);

	tegra_dpaux_clk_en(np_dp, id);

	mutex_lock(&dpaux_lock);
	_tegra_dpaux_pad_power(dc, id, on);
	mutex_unlock(&dpaux_lock);

	tegra_dpaux_clk_dis(np_dp, id);

	/* make sure host1x clock disabled now*/
	tegra_dc_io_end(dc);

	of_node_put(np_dp);
}

static inline void _tegra_dpaux_config_pad_mode(struct tegra_dc *dc,
					enum tegra_dpaux_instance id,
					enum tegra_dpaux_pad_mode mode)
{
	u32 val;
	void __iomem *regaddr;
#if !defined(CONFIG_TEGRA_NVDISPLAY)
	regaddr = IO_ADDRESS(dpaux_base_addr[id] + DPAUX_HYBRID_PADCTL * 4);
#else
	regaddr = dpaux_baseaddr[id] + DPAUX_HYBRID_PADCTL * 4;
#endif

	val = readl(regaddr);
	val &= ~(DPAUX_HYBRID_PADCTL_I2C_SDA_INPUT_RCV_ENABLE |
		DPAUX_HYBRID_PADCTL_I2C_SCL_INPUT_RCV_ENABLE |
		DPAUX_HYBRID_PADCTL_MODE_I2C);
	val |= mode ? (DPAUX_HYBRID_PADCTL_I2C_SDA_INPUT_RCV_ENABLE |
		DPAUX_HYBRID_PADCTL_I2C_SCL_INPUT_RCV_ENABLE |
		mode) : 0;
	writel(val, regaddr);
}

void tegra_dpaux_config_pad_mode(struct tegra_dc *dc,
					enum tegra_dpaux_instance id,
					enum tegra_dpaux_pad_mode mode)
{
	struct device_node *np_dp =
		(dc->ndev->id) ? of_find_node_by_path(DPAUX1_NODE)
		: of_find_node_by_path(DPAUX_NODE);

	if (!np_dp) {
		dev_err(&dc->ndev->dev, "dp node not available\n");
		return;
	}

	/* make sure host1x is clocked before dpaux clk access */
	tegra_dc_io_start(dc);

	tegra_dpaux_clk_en(np_dp, id);
	mutex_lock(&dpaux_lock);
	/*
	 * Make sure to configure the pad mode before we power it on.
	 * If not done in this order, there is a chance that the pad
	 * runs in the default mode for a while causing intermittent
	 * glitches on the physical lines
	 */
	_tegra_dpaux_config_pad_mode(dc, id, mode);
	_tegra_dpaux_pad_power(dc, id, true);
	mutex_unlock(&dpaux_lock);

	tegra_dpaux_clk_dis(np_dp, id);

	/* make sure host1x clock disabled now*/
	tegra_dc_io_end(dc);

	of_node_put(np_dp);
}

void tegra_dpaux_clk_config(struct tegra_dc *dc,
					enum tegra_dpaux_instance id,
					bool enable)
{
	struct device_node *np_dp =
		(dc->ndev->id) ? of_find_node_by_path(DPAUX1_NODE)
		: of_find_node_by_path(DPAUX_NODE);

	if (!np_dp) {
		dev_err(&dc->ndev->dev, "dp node not available\n");
		return;
	}

	if (enable)
		tegra_dpaux_clk_en(np_dp, id);
	else
		tegra_dpaux_clk_dis(np_dp, id);

}

void tegra_dpaux_clk_reset(struct tegra_dc *dc,
					enum tegra_dpaux_instance id)
{
	struct device_node *np_dp =
		(dc->ndev->id) ? of_find_node_by_path(DPAUX1_NODE)
		: of_find_node_by_path(DPAUX_NODE);

	if (!np_dp) {
		dev_err(&dc->ndev->dev, "dp node not available\n");
		return;
	}
	tegra_periph_reset_assert(tegra_dpaux_clk_get(np_dp, id));
	mdelay(2);
	tegra_periph_reset_deassert(tegra_dpaux_clk_get(np_dp, id));
	mdelay(1);
}

__maybe_unused
void tegra_set_dpaux_addr(void __iomem *dpaux_base,
			enum tegra_dpaux_instance id)
{
	dpaux_baseaddr[id] = dpaux_base;
}
