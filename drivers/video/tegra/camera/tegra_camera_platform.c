/*
 * drivers/video/tegra/camera/tegra_camera_platform.c
 *
 * Copyright (c) 2015-2016, NVIDIA CORPORATION.  All rights reserved.
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
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/clk.h>
#include <linux/platform/tegra/mc.h>

#include "vi/vi.h"
#include "tegra_camera_dev_mfi.h"
#include <media/tegra_camera_platform.h>
#include "camera_priv_defs.h"

#define CAMDEV_NAME "tegra_camera_ctrl"

/* Peak BPP for any of the YUV/Bayer formats */
#define CAMERA_PEAK_BPP 5

#define LANE_SPEED_1_GBPS 1000000000
#define LANE_SPEED_1_5_GBPS 1500000000

struct tegra_camera_info {
	char devname[64];
	atomic_t in_use;
	struct device *dev;
	struct clk *emc;
	struct clk *iso_emc;
	tegra_isomgr_handle isomgr_handle;
	u64 max_bw;
	struct mutex update_bw_lock;
	u32 vi_mode_isobw;
	u64 bypass_mode_isobw;
};

static const struct of_device_id tegra_camera_of_ids[] = {
	{ .compatible = "nvidia, tegra-camera-platform" },
	{ },
};

static struct miscdevice tegra_camera_misc;

static int tegra_camera_isomgr_register(struct tegra_camera_info *info)
{
#if defined(CONFIG_TEGRA_ISOMGR)
	u32 num_csi_lanes;
	u32 max_num_streams;
	u64 max_lane_speed;
	u32 min_bits_per_pixel;

	dev_dbg(info->dev, "%s++\n", __func__);

	/* TODO: Extract these values from DT */
#if defined(CONFIG_ARCH_TEGRA_21x_SOC)
	num_csi_lanes = 2;
	max_num_streams = 6;
	max_lane_speed = LANE_SPEED_1_5_GBPS;
	min_bits_per_pixel = 10;
#elif defined(CONFIG_ARCH_TEGRA_12x_SOC) || defined(CONFIG_ARCH_TEGRA_13x_SOC)
	num_csi_lanes = 2;
	max_num_streams = 2;
	max_lane_speed = LANE_SPEED_1_GBPS;
	min_bits_per_pixel = 10;
#else
	dev_err(info->dev, "%s Invalid chip-id\n", __func__);
	return -EINVAL;
#endif
	/*
	 * Let's go with simple registering max dedicated BW
	 * approach for now.
	 *
	 * The formula is:
	 * Camera's max total ISO BW =
	 * ((max_num_streams *
	 * num_csi_lanes * max_lane_speed) /
	 * min_bits_per_pixel) * max_peak_BPP
	 *
	 * Above considered cap is CSI link cap, but we need to
	 * consider real sensor-on-board cap also, DT based approach
	 * should handle it.
	 *
	 * Only VI out is considered, because in case of
	 * max # of cameras running, only VI is in ISO mode.
	 *
	 * TODO: Try renegotiate approach later.
	 */
	info->max_bw = (((num_csi_lanes * max_lane_speed * max_num_streams) /
				min_bits_per_pixel) * CAMERA_PEAK_BPP) / 1000;
	if (!info->max_bw) {
		dev_err(info->dev, "%s: BW must be non-zero\n", __func__);
		return -EINVAL;
	}

	dev_info(info->dev, "%s camera's max_iso_bw %llu\n",
				__func__, info->max_bw);

	/* Register with max possible BW for CAMERA usecases.*/
	info->isomgr_handle = tegra_isomgr_register(
					TEGRA_ISO_CLIENT_TEGRA_CAMERA,
					info->max_bw,
					NULL,	/* tegra_isomgr_renegotiate */
					NULL);	/* *priv */

	if (IS_ERR(info->isomgr_handle)) {
		dev_err(info->dev,
			"%s: unable to register to isomgr\n",
				__func__);
		return -ENOMEM;
	}
#endif

	return 0;
}

static int tegra_camera_isomgr_unregister(struct tegra_camera_info *info)
{
#if defined(CONFIG_TEGRA_ISOMGR)
	tegra_isomgr_unregister(info->isomgr_handle);
	info->isomgr_handle = NULL;
#endif

	return 0;
}

static int tegra_camera_isomgr_request(
		struct tegra_camera_info *info, uint iso_bw, uint lt)
{
#if defined(CONFIG_TEGRA_ISOMGR)
	int ret = 0;

	dev_dbg(info->dev,
		"%s++ bw=%u, lt=%u\n", __func__, iso_bw, lt);

	if (!info->isomgr_handle) {
		dev_err(info->dev,
		"%s: isomgr_handle is NULL\n",
		__func__);
		return -EINVAL;
	}

	/* return value of tegra_isomgr_reserve is dvfs latency in usec */
	ret = tegra_isomgr_reserve(info->isomgr_handle,
				iso_bw,	/* KB/sec */
				lt);	/* usec */
	if (!ret) {
		dev_err(info->dev,
		"%s: failed to reserve %u KBps\n", __func__, iso_bw);
		return -ENOMEM;
	}

	/* return value of tegra_isomgr_realize is dvfs latency in usec */
	ret = tegra_isomgr_realize(info->isomgr_handle);
	if (ret)
		dev_dbg(info->dev,
		"%s: tegra_camera isomgr latency is %d usec",
		__func__, ret);
	else {
		dev_err(info->dev,
		"%s: failed to realize %u KBps\n", __func__, iso_bw);
			return -ENOMEM;
	}
#endif

	return 0;
}

static int tegra_camera_isomgr_release(struct tegra_camera_info *info)
{
#if defined(CONFIG_TEGRA_ISOMGR)
	int ret = 0;
	dev_dbg(info->dev, "%s++\n", __func__);

	/* deallocate isomgr bw */
	ret = tegra_camera_isomgr_request(info, 0, 4);
	if (ret) {
		dev_err(info->dev,
		"%s: failed to deallocate memory in isomgr\n",
		__func__);
		return -ENOMEM;
	}
#endif

	return 0;
}
int tegra_camera_emc_clk_enable(void)
{
	struct tegra_camera_info *info;
	int ret = 0;

	info = dev_get_drvdata(tegra_camera_misc.parent);
	if (!info)
		return -EINVAL;
	ret = clk_prepare_enable(info->emc);
	if (ret) {
		dev_err(info->dev, "Cannot enable camera.emc\n");
		return ret;
	}

	ret = clk_prepare_enable(info->iso_emc);
	if (ret) {
		dev_err(info->dev, "Cannot enable camera_iso.emc\n");
		goto err_iso_emc;
	}

	return 0;
err_iso_emc:
	clk_disable_unprepare(info->emc);
	return ret;
}
EXPORT_SYMBOL(tegra_camera_emc_clk_enable);

int tegra_camera_emc_clk_disable(void)
{
	struct tegra_camera_info *info;

	info = dev_get_drvdata(tegra_camera_misc.parent);
	if (!info)
		return -EINVAL;
	clk_disable_unprepare(info->emc);
	clk_disable_unprepare(info->iso_emc);
	return 0;
}
EXPORT_SYMBOL(tegra_camera_emc_clk_disable);

static int tegra_camera_open(struct inode *inode, struct file *file)
{
	struct tegra_camera_info *info;
	struct miscdevice *mdev;

	mdev = file->private_data;
	info = dev_get_drvdata(mdev->parent);

	file->private_data = info;

	return tegra_camera_emc_clk_enable();

}

static int tegra_camera_release(struct inode *inode, struct file *file)
{

	struct tegra_camera_info *info;
	int ret = 0;

	info = file->private_data;
	tegra_camera_emc_clk_disable();

	/* nullify isomgr request */
	if (info->isomgr_handle) {
		ret = tegra_camera_isomgr_release(info);
	}

	return ret;
}

#ifdef CONFIG_DEBUG_FS
static u64 vi_mode_d;
static u64 bypass_mode_d;

static int dbgfs_tegra_camera_init(void)
{
	struct dentry *dir;
	struct dentry *val;

	dir = debugfs_create_dir("tegra_camera_platform", NULL);
	if (!dir)
		return -ENOMEM;

	val = debugfs_create_u64("vi", S_IRUGO, dir, &vi_mode_d);
	if (!val)
		return -ENOMEM;
	val = debugfs_create_u64("scf", S_IRUGO, dir, &bypass_mode_d);
	if (!val)
		return -ENOMEM;
	return 0;
}
#endif

/*
 * vi_kbyteps: Iso bw request from V4L2 vi driver.
 * is_ioctl: Whether this function is called by userspace or kernel.
 * This function aggregates V4L2 vi driver's request with userspace (SCF)
 * iso bw request and submit total to isomgr.
 */
int vi_v4l2_update_isobw(u32 vi_kbyteps, u32 is_ioctl)
{
	struct tegra_camera_info *info;
	unsigned long total_khz;
	unsigned long bw;
	int ret = 0;

	info = dev_get_drvdata(tegra_camera_misc.parent);
	mutex_lock(&info->update_bw_lock);
	if (!is_ioctl)
		info->vi_mode_isobw = vi_kbyteps;
	bw = info->bypass_mode_isobw + info->vi_mode_isobw;

	/* Use Khz to prevent overflow */
	total_khz = tegra_emc_bw_to_freq_req(bw);
	total_khz = min(ULONG_MAX / 1000, total_khz);

	dev_dbg(info->dev, "%s:Set iso bw %lu kbyteps at %lu KHz\n",
		__func__, bw, total_khz);
	ret = clk_set_rate(info->iso_emc, total_khz * 1000);
	if (ret)
		dev_err(info->dev, "%s:Failed to set iso bw\n",
			__func__);

	/*
	 * Request to ISOMGR.
	 * 3 usec is minimum time to switch PLL source.
	 * Let's put 4 usec as latency for now.
	 */
	ret = tegra_camera_isomgr_request(info, bw, 4);
	if (ret) {
		dev_err(info->dev,
		"%s: failed to reserve %lu KBps with isomgr\n",
		__func__, bw);
		mutex_unlock(&info->update_bw_lock);
		return -ENOMEM;
	}

#ifdef CONFIG_DEBUG_FS
	vi_mode_d = info->vi_mode_isobw;
	bypass_mode_d = info->bypass_mode_isobw;
#endif
	mutex_unlock(&info->update_bw_lock);
	return ret;
}
EXPORT_SYMBOL(vi_v4l2_update_isobw);

static long tegra_camera_ioctl(struct file *file,
	unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct tegra_camera_info *info;
	info = file->private_data;

	switch (_IOC_NR(cmd)) {
	case _IOC_NR(TEGRA_CAMERA_IOCTL_SET_BW):
	{
		struct bw_info kcopy;
		unsigned long mc_khz = 0;

		memset(&kcopy, 0, sizeof(kcopy));

		if (copy_from_user(&kcopy, (const void __user *)arg,
			sizeof(struct bw_info))) {
			dev_err(info->dev, "%s:Failed to get data from user\n",
				__func__);
			return -EFAULT;
		}
		/* Use Khz to prevent overflow */
		mc_khz = tegra_emc_bw_to_freq_req(kcopy.bw);
		mc_khz = min(ULONG_MAX / 1000, mc_khz);

		if (kcopy.is_iso) {
			info->bypass_mode_isobw = kcopy.bw;
			ret = vi_v4l2_update_isobw(0, 1);
		} else {
			dev_dbg(info->dev, "%s:Set bw %llu at %lu KHz\n",
				__func__, kcopy.bw, mc_khz);
			ret = clk_set_rate(info->emc, mc_khz * 1000);
			if (ret)
				dev_err(info->dev, "%s:Failed to set bw\n",
					__func__);
		}
		break;
	}
	default:
		break;
	}
	return ret;
}

static const struct file_operations tegra_camera_ops = {
	.owner = THIS_MODULE,
	.open = tegra_camera_open,
	.unlocked_ioctl = tegra_camera_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = tegra_camera_ioctl,
#endif
	.release = tegra_camera_release,
};



static int tegra_camera_probe(struct platform_device *pdev)
{
	int ret;
	struct tegra_camera_info *info;

	dev_info(&pdev->dev, "%s:camera_platform_driver probe\n", __func__);

	tegra_camera_misc.minor = MISC_DYNAMIC_MINOR;
	tegra_camera_misc.name = CAMDEV_NAME;
	tegra_camera_misc.fops = &tegra_camera_ops;
	tegra_camera_misc.parent = &pdev->dev;

	ret = misc_register(&tegra_camera_misc);
	if (ret) {
		dev_err(tegra_camera_misc.this_device,
			"register failed for %s\n",
			tegra_camera_misc.name);
		return ret;
	}
	info = devm_kzalloc(tegra_camera_misc.this_device,
		sizeof(struct tegra_camera_info), GFP_KERNEL);
	if (!info) {
		dev_err(tegra_camera_misc.this_device,
			"Can't allocate memory for %s\n",
			tegra_camera_misc.name);
		return -ENOMEM;
	}

	strcpy(info->devname, tegra_camera_misc.name);
	info->dev = tegra_camera_misc.this_device;

	info->emc = devm_clk_get(info->dev, "emc");
	if (IS_ERR(info->emc)) {
		dev_err(info->dev, "Failed to get camera.emc\n");
		return -EINVAL;
	}
	clk_set_rate(info->emc, 0);
	info->iso_emc = devm_clk_get(info->dev, "iso.emc");
	if (IS_ERR(info->iso_emc)) {
		dev_err(info->dev, "Failed to get camera_iso.emc\n");
		return -EINVAL;
	}
	clk_set_rate(info->iso_emc, 0);
	mutex_init(&info->update_bw_lock);
	/* Register Camera as isomgr client. */
	ret = tegra_camera_isomgr_register(info);
	if (ret) {
		dev_err(info->dev,
		"%s: failed to register CAMERA as isomgr client\n",
		__func__);
		return -ENOMEM;
	}

	tegra_camera_dev_mfi_init();
	tegra_vi_register_mfi_cb(tegra_camera_dev_mfi_cb, NULL);

	platform_set_drvdata(pdev, info);
#ifdef CONFIG_DEBUG_FS
	ret = dbgfs_tegra_camera_init();
	if (ret)
		dev_err(info->dev, "Fail to create debugfs");
#endif
	return 0;

}
static int tegra_camera_remove(struct platform_device *pdev)
{
	struct tegra_camera_info *info = platform_get_drvdata(pdev);
	dev_info(&pdev->dev, "%s:camera_platform_driver remove\n", __func__);

	tegra_vi_unregister_mfi_cb();

	if (info->isomgr_handle)
		tegra_camera_isomgr_unregister(info);

	return misc_deregister(&tegra_camera_misc);
}

static struct platform_driver tegra_camera_driver = {
	.probe = tegra_camera_probe,
	.remove = tegra_camera_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "tegra_camera_platform",
		.of_match_table = tegra_camera_of_ids
	}
};
static int __init tegra_camera_init(void)
{
	return platform_driver_register(&tegra_camera_driver);
}
static void __exit tegra_camera_exit(void)
{
	platform_driver_unregister(&tegra_camera_driver);
}

module_init(tegra_camera_init);
module_exit(tegra_camera_exit);

