/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.


 *  Copyright (C) 2009-2014 Broadcom Corporation
 */

/************************************************************************************
 *
 *  Filename:      fmdrv_v4l2.c
 *
 *  Description:   FM Driver for Connectivity chip of Broadcom Corporation.
*  This file provides interfaces to V4L2 subsystem.
*
*  This module registers with V4L2 subsystem as Radio
*  data system interface (/dev/radio). During the registration,
*  it will expose three set of function pointers to V4L2 subsystem.
*
*    1) File operation related API (open, close, read, write, poll...etc).
*    2) Set of V4L2 IOCTL complaint API.
*
************************************************************************************/

#include "fmdrv.h"
#include "fmdrv_v4l2.h"
#include "fmdrv_main.h"
#include "fmdrv_rx.h"
#include "fm_public.h"
#include "fmdrv_config.h"
#include "../include/v4l2_target.h"
#include "../include/v4l2_logs.h"
#include <linux/ioctl.h>


/* Required to set "THIS_MODULE" during driver registration to linux system. Not required in Maguro */
#ifndef V4L2_THIS_MODULE_SUPPORT
#include <linux/export.h>
#endif

/************************************************************************************
**  Constants & Macros
************************************************************************************/
#ifndef V4L2_FM_DEBUG
#define V4L2_FM_DEBUG TRUE
#endif

/* The major device number. We can't rely on dynamic
 * registration any more, because ioctls need to know
 * it.  There is no logic behind chosing 100, it's just a random
 * number*/
#define MAJOR_NUM 100
/* Set the message of the device driver */
#define IOCTL_GET_PI_CODE _IOR(MAJOR_NUM, 0, char *)
#define IOCTL_GET_TP_CODE _IOR(MAJOR_NUM, 1, void *)
#define IOCTL_GET_PTY_CODE _IOR(MAJOR_NUM, 2, char *)
#define IOCTL_GET_TA_CODE _IOR(MAJOR_NUM, 3, char *)
#define IOCTL_GET_MS_CODE _IOR(MAJOR_NUM, 4, char *)
#define IOCTL_GET_PS_CODE _IOR(MAJOR_NUM, 5, char *)
#define IOCTL_GET_RT_MSG _IOR(MAJOR_NUM, 6, char *)
#define IOCTL_GET_CT_DATA _IOR(MAJOR_NUM, 7, char *)
#define IOCTL_GET_TMC_CHANNEL _IOR(MAJOR_NUM, 8, char *)


/*These values are set and has to be sent together.*/
/*Keep them as a set always, never try to further seperate them*/
/*These are arguments to BRCM vsc HCI command to switch the FM-I2S pins */
/*over PCM pins*/
/*I2S works in slave mode. Host side has to be master*/
unsigned char i2s_slave_on_pcm_pins[5] = {0x07, 0x19, 0x18, 0x19, 0x19};
/*I2S works in master mode. Host side has to be slave*/
unsigned char i2s_master_on_pcm_pins[5] = {0x05, 0x19, 0x18, 0x18, 0x18};

/*PCM works in slave mode. Host side has to be master*/
unsigned char bt_slave_on_pcm_pins[5] = {0x01, 0x19, 0x18, 0x19, 0x19};
/*PCM works in master mode. Host side has to be slave*/
unsigned char bt_master_on_pcm_pins[5] = {0x01, 0x19, 0x18, 0x18, 0x18 };

/* set this module parameter to enable debug info */
extern int fm_dbg_param;

extern struct region_info region_configs[];



#if V4L2_FM_DEBUG
#define V4L2_FM_DRV_DBG(flag, fmt, arg...) \
        do { \
            if (fm_dbg_param & flag) \
                printk(KERN_DEBUG "(v4l2fmdrv):%s  "fmt"\n" , \
                                           __func__,## arg); \
        } while(0)
#else
#define V4L2_FM_DRV_DBG(flag, fmt, arg...)
#endif
#define V4L2_FM_DRV_ERR(fmt, arg...)  printk(KERN_ERR "(v4l2fmdrv):%s  "fmt"\n" , \
                                           __func__,## arg)



/************************************************************************************
**  Static variables
************************************************************************************/

static struct video_device *gradio_dev;
static unsigned char radio_disconnected;

static atomic_t v4l2_device_available = ATOMIC_INIT(1);

/************************************************************************************
**  Forward function declarations
************************************************************************************/

static int fm_v4l2_vidioc_s_hw_freq_seek(struct file *, void *,
                    const struct v4l2_hw_freq_seek *);

/************************************************************************************
**  Functions
************************************************************************************/
/*****************************************************************************
**   V4L2 RADIO (/dev/radioX) device file operation interfaces
*****************************************************************************/

/* Read RX RDS data */
static ssize_t fm_v4l2_fops_read(struct file *file, char __user * buf,
                    size_t count, loff_t *ppos)
{
    int ret, bytes_read;
    struct fmdrv_ops *fmdev;

    fmdev = video_drvdata(file);

    if (!radio_disconnected) {
        V4L2_FM_DRV_ERR("(fmdrv): FM device is already disconnected\n");
        ret = -EIO;
        return ret;
    }

    /* Copy RDS data from the cicular buffer to userspace */
    bytes_read =
        fmc_transfer_rds_from_cbuff(fmdev, file, buf, count);
    return bytes_read;
}

/* Write RDS data. Since FM TX is not supported, return EINVAL
 */
static ssize_t fm_v4l2_fops_write(struct file *file, const char __user * buf,
                    size_t count, loff_t *ppos)
{
    return -EINVAL;
}

/* Handle Poll event for "/dev/radioX" device.*/
static unsigned int fm_v4l2_fops_poll(struct file *file,
                      struct poll_table_struct *pts)
{
    int ret;
    struct fmdrv_ops *fmdev;

    V4L2_FM_DRV_DBG(V4L2_DBG_RX, "(rds) %s", __func__ );

    fmdev = video_drvdata(file);
    /* Check if RDS data is available */
    ret = fm_rx_is_rds_data_available(fmdev, file, pts);
    if (!ret)
        return POLLIN | POLLRDNORM;
    return 0;
}

static ssize_t show_fmrx_comp_scan(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct fmdrv_ops *fmdev = dev_get_drvdata(dev);

    /* Chip doesn't support complete scan for weather band */
    if (fmdev->rx.region.fm_band == FM_BAND_WEATHER)
        return -EINVAL;

    return sprintf(buf, "%d\n", fmdev->rx.no_of_chans);
}

static ssize_t fmrx_comp_scan_locked(struct fmdrv_ops *fmdev,
        const char *buf, size_t size)
{
    int ret;
    unsigned long comp_scan;

    /* Chip doesn't support complete scan for weather band */
    if (fmdev->rx.region.fm_band == FM_BAND_WEATHER)
        return -EINVAL;

    if (kstrtoul(buf, 0, &comp_scan))
        return -EINVAL;

    ret = fm_rx_seek_station(fmdev, 1, 0);// FM_CHANNEL_SPACING_200KHZ, comp_scan);
    if (ret < 0)
        V4L2_FM_DRV_ERR("RX complete scan failed - %d\n", ret);

    /* the channel count is read through show; a store returns the
     * bytes it consumed, and anything less makes the caller write again */
    return size;
}

static ssize_t show_fmrx_deemphasis(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct fmdrv_ops *fmdev = dev_get_drvdata(dev);

    return sprintf(buf, "%d\n", (fmdev->rx.region.deemphasis==
                FM_RX_EMPHASIS_FILTER_50_USEC) ? 50 : 75);
}

static ssize_t fmrx_deemphasis_locked(struct fmdrv_ops *fmdev,
        const char *buf, size_t size)
{
    int ret;
    unsigned long deemph_mode;

    if (kstrtoul(buf, 0, &deemph_mode))
        return -EINVAL;

    /* through the control, so it and the chip agree; accepts 50 or 75 */
    if (deemph_mode != 50 && deemph_mode != 75)
        return -EINVAL;
    ret = v4l2_ctrl_s_ctrl(fmdev->deemph_ctrl, deemph_mode == 75 ?
                           V4L2_DEEMPHASIS_75_uS : V4L2_DEEMPHASIS_50_uS);
    if (ret < 0) {
        V4L2_FM_DRV_ERR("Failed to set De-emphasis Mode\n");
        return ret;
    }

    return size;
}

static ssize_t show_fmrx_af(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct fmdrv_ops *fmdev = dev_get_drvdata(dev);

    return sprintf(buf, "%d\n", fmdev->rx.af_mode);
}

static ssize_t fmrx_af_locked(struct fmdrv_ops *fmdev,
        const char *buf, size_t size)
{
    int ret;
    unsigned long af_mode;

    if (kstrtoul(buf, 0, &af_mode))
        return -EINVAL;

    if (af_mode < 0 || af_mode > 1)
        return -EINVAL;

    ret = fm_rx_set_af_switch(fmdev, af_mode);
    if (ret < 0) {
        V4L2_FM_DRV_ERR("Failed to set AF Switch\n");
        return ret;
    }

    return size;
}

/*
 * The station's RDS as the AF jump uses it: frequency, PI, RSSI and the
 * alternative frequencies, in 10 kHz units
 */
static ssize_t show_fmrx_af_info(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct fmdrv_ops *fmdev = dev_get_drvdata(dev);
    u16 af[FM_AF_LIST_MAX], pi;
    u8 n;
    int i, len;

    if (mutex_lock_interruptible(&fmdev->mutex))
        return -ERESTARTSYS;
    if (!test_bit(FM_CORE_READY, &fmdev->flag) ||
        fmdev->curr_fmmode != FM_MODE_RX) {
        mutex_unlock(&fmdev->mutex);
        return -EPERM;
    }
    fm_rx_read_curr_rssi_freq(fmdev, FALSE);
    pi = fm_af_snapshot(&fmdev->af, af, &n);

    len = sprintf(buf, "freq %u pi %04x rssi -%u af %d:",
                  FM_SET_FREQ(fmdev->rx.curr_freq), pi, fmdev->rx.curr_rssi, n);
    for (i = 0; i < n; i++)
        len += sprintf(buf + len, " %u", FM_SET_FREQ(af[i]));
    len += sprintf(buf + len, "\n");
    mutex_unlock(&fmdev->mutex);
    return len;
}

/* The RSSI, in -dBm, from which a reading counts as weak for AF switching */
static ssize_t show_fmrx_af_weak(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct fmdrv_ops *fmdev = dev_get_drvdata(dev);

    return sprintf(buf, "%u\n", fmdev->af.weak_dbm);
}

static ssize_t fmrx_af_weak_locked(struct fmdrv_ops *fmdev,
        const char *buf, size_t size)
{
    unsigned int dbm;

    if (kstrtouint(buf, 0, &dbm) || dbm < 20 || dbm > 127)
        return -EINVAL;
    fm_af_set_weak(&fmdev->af, dbm);
    return size;
}

/* "freq [pause_ms]": one AF jump by hand, freq in 10 kHz units */
static ssize_t fmrx_af_jump_locked(struct fmdrv_ops *fmdev,
        const char *buf, size_t size)
{
    unsigned int freq, pause_ms = 3000;
    unsigned short af_freq;
    u8 reason;
    int ret;

    if (sscanf(buf, "%u %u", &freq, &pause_ms) < 1)
        return -EINVAL;
    if (fmdev->curr_fmmode != FM_MODE_RX)
        return -EPERM;
    af_freq = FM_GET_FREQ(freq);
    if (!check_if_valid_freq(fmdev, af_freq))
        return -EINVAL;

    /* the switching's own attempt, if waiting for a pause, is off */
    fm_af_preempt(&fmdev->af);
    ret = fm_af_jump(&fmdev->af, af_freq, pause_ms, &reason);
    return (ret < 0 && ret != -EAGAIN) ? ret : size;
}

static ssize_t show_fmrx_band(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct fmdrv_ops *fmdev = dev_get_drvdata(dev);

    return sprintf(buf, "%d\n", fmdev->rx.region.fm_band);
}

static ssize_t fmrx_band_locked(struct fmdrv_ops *fmdev,
        const char *buf, size_t size)
{
    int ret;
    unsigned long fm_band;
    if (kstrtoul(buf, 0, &fm_band))
        return -EINVAL;
    pr_info("store_fmrx_band In  fm_band %ld",fm_band);

    if (fm_band < FM_BAND_EUROPE || fm_band >= FM_BAND_WEATHER)
        return -EINVAL;

    ret = fm_rx_set_region(fmdev, fm_band);
    if (ret < 0) {
        V4L2_FM_DRV_ERR("Failed to set FM Band\n");
        return ret;
    }

    return size;
}

static ssize_t show_fmrx_fm_audio_pins(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct fmdrv_ops *fmdev = dev_get_drvdata(dev);

    return sprintf(buf, "%s\n", fmdev->rx.current_pins);
}

static ssize_t fmrx_fm_audio_pins_locked(struct fmdrv_ops *fmdev,
        const char *buf, size_t size)
{
    /* used only by the ROUTE_* pin switches, off in this build */
    int ret __maybe_unused = 0;
    if(strncmp(buf, fmdev->rx.current_pins, 3) == 0 &&
       fmdev->rx.current_pins[0]) /*I2S or PCM*/
    {
        return size;
    }

    else if(strncmp(buf, "PCM", 3) == 0) /*use PCM pins*/
    {
        //send VSC to switch I2S to PCM pins
 #if ROUTE_FM_I2S_MASTER_TO_PCM_PINS
        V4L2_FM_DRV_DBG(V4L2_DBG_OPEN, "Routing I2S audio over PCM pins in master mode");
        ret = fmc_send_cmd(fmdev, 0, i2s_master_on_pcm_pins, 5, VSC_HCI_CMD, &fmdev->maintask_completion, NULL, NULL);
        if (ret < 0)
        {
            V4L2_FM_DRV_ERR("(fmdrv): Error setting switch I2s path to PCM pins as a master");
            return ret;
        }
#endif

#if ROUTE_FM_I2S_SLAVE_TO_PCM_PINS
    V4L2_FM_DRV_DBG(V4L2_DBG_OPEN, "Routing I2S audio over PCM pins in slave mode");
    ret = fmc_send_cmd(fmdev, 0, i2s_slave_on_pcm_pins, 5, VSC_HCI_CMD, &fmdev->maintask_completion, NULL, NULL);
    if (ret < 0)
    {
        V4L2_FM_DRV_ERR("(fmdrv): Error setting switch I2s path to PCM pins as a slave");
        return ret;
    }
#endif
    strlcpy(fmdev->rx.current_pins, "PCM", sizeof(fmdev->rx.current_pins));
    return size;
    }
    else if(strncmp(buf, "I2S", 3) == 0) /*use I2S pins and release PCM pins for BT SCO*/
    {
    /*send VSC to release PCM pins*/
#if ROUTE_BT_I2S_MASTER_TO_PCM_PINS
        V4L2_FM_DRV_DBG(V4L2_DBG_OPEN, "Routing I2S audio over PCM pins in master mode");
        ret = fmc_send_cmd(fmdev, 0, bt_master_on_pcm_pins, 5, VSC_HCI_CMD, &fmdev->maintask_completion, NULL, NULL);
        if (ret < 0)
        {
            V4L2_FM_DRV_ERR("(fmdrv): Error setting switch I2s path to PCM pins as a master");
            return ret;
        }
#endif

#if ROUTE_FM_I2S_SLAVE_TO_PCM_PINS
        V4L2_FM_DRV_DBG(V4L2_DBG_OPEN, "Routing I2S audio over PCM pins in slave mode");
        ret = fmc_send_cmd(fmdev, 0, bt_slave_on_pcm_pins, 5, VSC_HCI_CMD, &fmdev->maintask_completion, NULL, NULL);
        if (ret < 0)
        {
            V4L2_FM_DRV_ERR("(fmdrv): Error setting switch I2s path to PCM pins as a slave");
            return ret;
        }
#endif
        strlcpy(fmdev->rx.current_pins, "I2S", sizeof(fmdev->rx.current_pins));
        return size;
    }
    else
    {
        V4L2_FM_DRV_ERR("Wrong value: either PCM or I2S\n");
        return -EINVAL;
    }
    return size;
}


static ssize_t show_fmrx_rssi_lvl(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct fmdrv_ops *fmdev = dev_get_drvdata(dev);

    return sprintf(buf, "%d\n", fmdev->rx.curr_rssi_threshold);
}

static ssize_t fmrx_rssi_lvl_locked(struct fmdrv_ops *fmdev,
        const char *buf, size_t size)
{
    int ret;
    unsigned long rssi_lvl;

    if (kstrtoul(buf, 0, &rssi_lvl))
        return -EINVAL;

    ret = fm_rx_set_rssi_threshold(fmdev, rssi_lvl);
    if (ret < 0) {
        V4L2_FM_DRV_ERR("Failed to set RSSI level\n");
        return ret;
    }

    return size;
}

static ssize_t show_fmrx_snr_lvl(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct fmdrv_ops *fmdev = dev_get_drvdata(dev);

    return sprintf(buf, "%d\n", fmdev->rx.curr_snr_threshold);
}

static ssize_t fmrx_snr_lvl_locked(struct fmdrv_ops *fmdev,
        const char *buf, size_t size)
{
    int ret;
    unsigned long snr_lvl;

    if (kstrtoul(buf, 0, &snr_lvl))
        return -EINVAL;

    ret = fm_rx_set_snr_threshold(fmdev, snr_lvl);
    if (ret < 0) {
        V4L2_FM_DRV_ERR("Failed to set SNR level\n");
        return ret;
    }

    return size;
}

static ssize_t show_fmrx_channel_space(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct fmdrv_ops *fmdev = dev_get_drvdata(dev);

    return sprintf(buf, "%d\n", fmdev->rx.sch_step);
}

static ssize_t fmrx_channel_space_locked(struct fmdrv_ops *fmdev,
        const char *buf, size_t size)
{
    int ret;
    unsigned long chl_spacing,chl_step;

    if (kstrtoul(buf, 0, &chl_spacing))
        return -EINVAL;
    switch( chl_spacing){
        case CHL_SPACE_ONE:
            chl_step= FM_STEP_50KHZ;
            break;
        case CHL_SPACE_TWO:
            chl_step= FM_STEP_100KHZ;
            break;
        case CHL_SPACE_FOUR:
            chl_step= FM_STEP_200KHZ;
            break;
        default:
            chl_step= FM_STEP_100KHZ;
    };
    /* the step index, not the spacing: fm_sch_step_size[] has three
     * entries and the spacing was used to index it */
    ret = fmc_set_scan_step(fmdev, chl_step);
    if (ret < 0) {
        V4L2_FM_DRV_ERR("Failed to set channel spacing\n");
        return ret;
    }

    return size;
}

/*
 * The stores drive the chip, so they take the driver mutex like the ioctls;
 * the shows only read cached state. These used to be kobj_attributes whose
 * functions were written for struct device and cast through void *: they
 * were handed the kobject and dev_get_drvdata() read garbage. They were also
 * world-writable. Nothing in userspace uses them -- the FM HAL and app go
 * through /dev/radio0 -- so they are root-writable now.
 */
#define FM_SYSFS_STORE(name)                                              \
static ssize_t store_##name(struct device *dev,                           \
        struct device_attribute *attr, const char *buf, size_t size)      \
{                                                                         \
    struct fmdrv_ops *fmdev = dev_get_drvdata(dev);                       \
    ssize_t ret;                                                          \
                                                                          \
    if (mutex_lock_interruptible(&fmdev->mutex))                          \
        return -ERESTARTSYS;                                              \
    /* the group lives as long as the device; FM may be closed */         \
    if (!test_bit(FM_CORE_READY, &fmdev->flag))                           \
        ret = -EPERM;                                                     \
    else                                                                  \
        ret = name##_locked(fmdev, buf, size);                            \
    mutex_unlock(&fmdev->mutex);                                          \
    return ret;                                                           \
}

FM_SYSFS_STORE(fmrx_comp_scan)
FM_SYSFS_STORE(fmrx_deemphasis)
FM_SYSFS_STORE(fmrx_af)
FM_SYSFS_STORE(fmrx_band)
FM_SYSFS_STORE(fmrx_fm_audio_pins)
FM_SYSFS_STORE(fmrx_rssi_lvl)
FM_SYSFS_STORE(fmrx_snr_lvl)
FM_SYSFS_STORE(fmrx_channel_space)
FM_SYSFS_STORE(fmrx_af_jump)
FM_SYSFS_STORE(fmrx_af_weak)

/* To start FM RX complete scan*/
static struct device_attribute v4l2_fmrx_comp_scan =
__ATTR(fmrx_comp_scan, 0644, show_fmrx_comp_scan, store_fmrx_comp_scan);

/* To Set De-Emphasis filter mode */
static struct device_attribute v4l2_fmrx_deemph_mode =
__ATTR(fmrx_deemph_mode, 0644, show_fmrx_deemphasis, store_fmrx_deemphasis);

/* To Enable/Disable FM RX RDS AF feature */
static struct device_attribute v4l2_fmrx_rds_af =
__ATTR(fmrx_rds_af, 0644, show_fmrx_af, store_fmrx_af);

/* To switch between Japan/US bands */
static struct device_attribute v4l2_fmrx_band =
__ATTR(fmrx_band, 0644, show_fmrx_band, store_fmrx_band);

/* To set the desired FM reception RSSI level */
static struct device_attribute v4l2_fmrx_rssi_lvl =
__ATTR(fmrx_rssi_lvl, 0644, show_fmrx_rssi_lvl, store_fmrx_rssi_lvl);

/* To set the desired FM reception SNR level */
static struct device_attribute v4l2_fmrx_snr_lvl =
__ATTR(fmrx_snr_lvl, 0644, show_fmrx_snr_lvl, store_fmrx_snr_lvl);

/* To set the desired channel spacing */
static struct device_attribute v4l2_fmrx_channel_space =
__ATTR(fmrx_chl_lvl, 0644, show_fmrx_channel_space, store_fmrx_channel_space);

/* To switch between PCM / I2S pins*/
static struct device_attribute v4l2_fmrx_fm_audio_pins =
__ATTR(fmrx_fm_audio_pins, 0644, show_fmrx_fm_audio_pins, store_fmrx_fm_audio_pins);

/* AF jump, by hand: RDS state, and one jump */
static struct device_attribute v4l2_fmrx_af_info =
__ATTR(fmrx_af_info, 0444, show_fmrx_af_info, NULL);
static struct device_attribute v4l2_fmrx_af_jump =
__ATTR(fmrx_af_jump, 0200, NULL, store_fmrx_af_jump);
static struct device_attribute v4l2_fmrx_af_weak =
__ATTR(fmrx_af_weak, 0644, show_fmrx_af_weak, store_fmrx_af_weak);

static struct attribute *v4l2_fm_attrs[] = {
    &v4l2_fmrx_af_info.attr,
    &v4l2_fmrx_af_jump.attr,
    &v4l2_fmrx_af_weak.attr,
    &v4l2_fmrx_comp_scan.attr,
    &v4l2_fmrx_deemph_mode.attr,
    &v4l2_fmrx_rds_af.attr,
    &v4l2_fmrx_band.attr,
    &v4l2_fmrx_rssi_lvl.attr,
    &v4l2_fmrx_snr_lvl.attr,
    &v4l2_fmrx_channel_space.attr,
    &v4l2_fmrx_fm_audio_pins.attr,
    NULL,
};
static struct attribute_group v4l2_fm_attr_grp = {
    .attrs = v4l2_fm_attrs,
};

/* Handle open request for "/dev/radioX" device.
 * Start with FM RX mode as default.
 */
static int fm_v4l2_fops_open_locked(struct file *file);

static int fm_v4l2_fops_open(struct file *file)
{
    struct fmdrv_ops *fmdev = video_drvdata(file);
    int ret;

    if (mutex_lock_interruptible(&fmdev->mutex))
        return -ERESTARTSYS;
    ret = fm_v4l2_fops_open_locked(file);
    mutex_unlock(&fmdev->mutex);
    return ret;
}

static int fm_v4l2_fops_open_locked(struct file *file)
{
    int ret = -EINVAL;
    bool fm_on = false;
    unsigned char option;
    struct fmdrv_ops *fmdev = NULL;
    V4L2_FM_DRV_DBG(V4L2_DBG_OPEN, "(fmdrv): fm_v4l2_fops_open");
    /* Don't allow multiple open */
    if(!atomic_dec_and_test(&v4l2_device_available))
    {
        atomic_inc(&v4l2_device_available);
        V4L2_FM_DRV_ERR("(fmdrv): FM device is already opened\n");
        return -EBUSY;
    }

    if (radio_disconnected) {
        V4L2_FM_DRV_ERR("(fmdrv): FM device is already opened\n");
        ret = -EBUSY;
        goto err_inc_avail;
    }

    fmdev = video_drvdata(file);
    /* initialize the driver */
    ret = fmc_prepare(fmdev);
    if (ret < 0) {
        V4L2_FM_DRV_ERR("(fmdrv): Unable to prepare FM CORE");
        goto err_inc_avail;
    }

    radio_disconnected = 1;

    ret = fmc_set_mode(fmdev, FM_MODE_RX); /* As of now, support only Rx */

#if(defined(DEF_V4L2_FM_WORLD_REGION) && DEF_V4L2_FM_WORLD_REGION == FM_REGION_NA)
    option = FM_REGION_NA | FM_RBDS_BIT;
#elif(defined(DEF_V4L2_FM_WORLD_REGION) && DEF_V4L2_FM_WORLD_REGION == FM_REGION_EUR)
    option = FM_REGION_EUR | FM_RDS_BIT;
#elif(defined(DEF_V4L2_FM_WORLD_REGION) && DEF_V4L2_FM_WORLD_REGION == FM_REGION_JP)
    option = FM_REGION_JP | FM_RDS_BIT;
#else
    option = 0;
#endif

    /* Enable FM */
    V4L2_FM_DRV_DBG(V4L2_DBG_OPEN,"(fmdrv): FM Enable INIT option : %d", option);
    ret = fmc_enable(fmdev, option);
    if (ret < 0) {
        V4L2_FM_DRV_ERR("(fmdrv): Unable to enable FM");
        goto err_release;
    }
    fm_on = true;

    /* Set Audio mode */
    V4L2_FM_DRV_DBG(V4L2_DBG_OPEN,"(fmdrv): FM Set Audio mode option : %d", DEF_V4L2_FM_AUDIO_MODE);
    ret = fmc_set_audio_mode(fmdev, DEF_V4L2_FM_AUDIO_MODE);
    if (ret < 0) {
        V4L2_FM_DRV_ERR("(fmdrv): Error setting Audio mode during FM enable operation");
        goto err_release;
    }

    /* The controls -- de-emphasis, volume, mute -- as userspace last set
     * them, over what enabling the chip and its region set up */
    ret = v4l2_ctrl_handler_setup(&fmdev->ctrl_handler);
    if (ret < 0) {
        V4L2_FM_DRV_ERR("(fmdrv): Error applying controls during FM enable operation");
        goto err_release;
    }

    /* Set Audio path */
    V4L2_FM_DRV_DBG(V4L2_DBG_OPEN,"(fmdrv): FM Set Audio path option : %d", DEF_V4L2_FM_AUDIO_PATH);
    ret = fm_rx_config_audio_path(fmdev, DEF_V4L2_FM_AUDIO_PATH);
    if (ret < 0) {
        V4L2_FM_DRV_ERR("(fmdrv): Error setting Audio path during FM enable operation");
        goto err_release;
    }

#if ROUTE_FM_I2S_MASTER_TO_PCM_PINS
    V4L2_FM_DRV_DBG(V4L2_DBG_OPEN, "Routing I2S audio over PCM pins in master mode");
    ret = fmc_send_cmd(fmdev, 0, i2s_master_on_pcm_pins, 5, VSC_HCI_CMD, &fmdev->maintask_completion, NULL, NULL);
    if (ret < 0) {
        V4L2_FM_DRV_ERR("(fmdrv): Error setting switch I2s path to PCM pins as a master");
        goto err_release;
    }
#endif

#if ROUTE_FM_I2S_SLAVE_TO_PCM_PINS
    V4L2_FM_DRV_DBG(V4L2_DBG_OPEN, "Routing I2S audio over PCM pins in slave mode");
    ret = fmc_send_cmd(fmdev, 0, i2s_slave_on_pcm_pins, 5, VSC_HCI_CMD, &fmdev->maintask_completion, NULL, NULL);
    if (ret < 0)
    {
        V4L2_FM_DRV_ERR("(fmdrv): Error setting switch I2s path to PCM pins as a slave");
        goto err_release;
    }
#endif

    return 0;

err_release:
    /* The core is prepared and registered with the line discipline; leave
     * it up and the next open finds it "already up" on a half-set chip.
     * FM, once turned on, is turned off first -- with BT up the chip stays
     * powered and would keep FM running. */
    if (fm_on)
        fmc_turn_fm_off(fmdev);
    fmc_release(fmdev);
    radio_disconnected = 0;
err_inc_avail:
    atomic_inc(&v4l2_device_available);
    return ret;
}

/* Handle close request for "/dev/radioX" device.
 */
static int fm_v4l2_fops_release_locked(struct file *file);

static int fm_v4l2_fops_release(struct file *file)
{
    struct fmdrv_ops *fmdev = video_drvdata(file);
    int ret;

    /* release cannot be interrupted: the chip has to be turned off */
    mutex_lock(&fmdev->mutex);
    ret = fm_v4l2_fops_release_locked(file);
    mutex_unlock(&fmdev->mutex);
    return ret;
}

static int fm_v4l2_fops_release_locked(struct file *file)
{
    int ret =  -EINVAL;
    struct fmdrv_ops *fmdev;
    V4L2_FM_DRV_DBG(V4L2_DBG_CLOSE, "(fmdrv): fm_v4l2_fops_release");

    fmdev = video_drvdata(file);

    if (!radio_disconnected) {
        V4L2_FM_DRV_DBG(V4L2_DBG_CLOSE, "(fmdrv):FM dev already closed, close called again?");
        return ret;
    }
    /* First set audio path to NONE */
    ret = fm_rx_config_audio_path(fmdev, FM_AUDIO_NONE);
    if (ret < 0) {
        V4L2_FM_DRV_ERR("(fmdrv): Failed to set audio path to FM_AUDIO_NONE");
        /*ret = 0;*/
    }
#if ROUTE_FM_I2S_MASTER_TO_PCM_PINS
    V4L2_FM_DRV_DBG(V4L2_DBG_CLOSE, "Routing I2S audio over PCM pins in master mode");
    ret = fmc_send_cmd(fmdev, 0, bt_master_on_pcm_pins, 5, VSC_HCI_CMD, &fmdev->maintask_completion, NULL, NULL);
    if (ret < 0)
    {
        /* carry on: the file is being closed whatever the chip says */
        V4L2_FM_DRV_ERR("(fmdrv): Error setting switch I2s path to PCM pins as a master");
    }
#endif

#if ROUTE_FM_I2S_SLAVE_TO_PCM_PINS
    V4L2_FM_DRV_DBG(V4L2_DBG_CLOSE, "Routing I2S audio over PCM pins in slave mode");
    ret = fmc_send_cmd(fmdev, 0, bt_slave_on_pcm_pins, 5, VSC_HCI_CMD, &fmdev->maintask_completion, NULL, NULL);
    if (ret < 0) {
        /* carry on, as above */
        V4L2_FM_DRV_ERR("(fmdrv): Error setting switch I2s path to PCM pins as a slave");
    }
#endif

    /* Now disable FM */
    ret = fmc_turn_fm_off(fmdev);
    if(ret < 0)
    {
        V4L2_FM_DRV_ERR("(fmdrv): Error disabling FM. Continuing to release FM core..");
        ret = 0;
    }
    ret = fmc_release(fmdev);
    if (ret < 0)
        V4L2_FM_DRV_ERR("(fmdrv): FM CORE release failed");

    /* Whatever failed above, the file is closed: returning early here left
     * the device marked open for good, and every later open got -EBUSY. */
    radio_disconnected = 0;
    atomic_inc(&v4l2_device_available);

    return ret < 0 ? ret : 0;
}

/*****************************************************************************
**   V4L2 RADIO (/dev/radioX) device IOCTL interfaces
*****************************************************************************/

/*
* Function to query the driver capabilities
*/
static int fm_v4l2_vidioc_querycap(struct file *file, void *priv,
                    struct v4l2_capability *capability)
{
    struct fmdrv_ops *fmdev;

    fmdev = video_drvdata(file);

    strlcpy(capability->driver, FM_DRV_NAME, sizeof(capability->driver));
    strlcpy(capability->card, FM_DRV_CARD_SHORT_NAME,
                                    sizeof(capability->card));
    sprintf(capability->bus_info, "UART");
    capability->version = FM_DRV_RADIO_VERSION;
    capability->capabilities = fmdev->device_info.capabilities;
    return 0;
}

/*
 * Controls, through the V4L2 control framework. The core calls s_ctrl with
 * the driver mutex held: from an ioctl (the video device's lock) or from
 * open's v4l2_ctrl_handler_setup(). With FM closed a value is only kept,
 * and open pushes it to the chip.
 */
static int fm_v4l2_s_ctrl(struct v4l2_ctrl *ctrl)
{
    struct fmdrv_ops *fmdev = container_of(ctrl->handler, struct fmdrv_ops,
                                           ctrl_handler);

    if (!test_bit(FM_CORE_READY, &fmdev->flag) ||
        fmdev->curr_fmmode != FM_MODE_RX)
        return 0;

    switch (ctrl->id) {
    case V4L2_CID_AUDIO_MUTE:
        return fm_rx_set_mute_mode(fmdev, ctrl->val ? FM_MUTE_ON : FM_MUTE_OFF);

    case V4L2_CID_AUDIO_VOLUME:
        return fm_rx_set_volume(fmdev, (unsigned short)ctrl->val);

    case V4L2_CID_TUNE_DEEMPHASIS:
        return fm_rx_config_deemphasis(fmdev,
                ctrl->val == V4L2_DEEMPHASIS_75_uS ? FM_DEEMPHA_75U
                                                   : FM_DEEMPHA_50U);
    }
    return -EINVAL;
}

static const struct v4l2_ctrl_ops fm_ctrl_ops = {
    .s_ctrl = fm_v4l2_s_ctrl,
};

/* The chip's tuning range, in 62.5 Hz units (V4L2_TUNER_CAP_LOW) */
#define FM_BAND_LOW_62_5HZ(f10khz)   ((f10khz) * 160)

/*
 * One band: the receiver's current one, which tuning is limited to. Seeks
 * may narrow it per request (VIDIOC_S_HW_FREQ_SEEK rangelow/rangehigh).
 */
static int fm_v4l2_vidioc_enum_freq_bands(struct file *file, void *priv,
                    struct v4l2_frequency_band *band)
{
    struct fmdrv_ops *fmdev = video_drvdata(file);

    if (band->tuner != 0 || band->index != 0)
        return -EINVAL;

    memset(band->reserved, 0, sizeof(band->reserved));
    band->type = V4L2_TUNER_RADIO;
    band->capability = V4L2_TUNER_CAP_LOW | V4L2_TUNER_CAP_STEREO |
                       V4L2_TUNER_CAP_RDS | V4L2_TUNER_CAP_FREQ_BANDS |
                       V4L2_TUNER_CAP_HWSEEK_BOUNDED |
                       V4L2_TUNER_CAP_HWSEEK_WRAP |
                       V4L2_TUNER_CAP_HWSEEK_PROG_LIM;
    /* the region's whole band, not what the last seek narrowed it to */
    band->rangelow = FM_BAND_LOW_62_5HZ(
                FM_SET_FREQ(region_configs[fmdev->rx.curr_region].low_bound));
    band->rangehigh = FM_BAND_LOW_62_5HZ(
                FM_SET_FREQ(region_configs[fmdev->rx.curr_region].high_bound));
    band->modulation = V4L2_BAND_MODULATION_FM;
    return 0;
}

/*
* Function to get the driver audio params. Called
* by user-space via IOCTL call
*/
static int fm_v4l2_vidioc_g_audio(struct file *file, void *priv,
                    struct v4l2_audio *audio)
{
    memset(audio, 0, sizeof(*audio));
    audio->index = 0;
    strcpy(audio->name, "Radio");
    /* For FM Radio device, the audio capability should always return
   V4L2_AUDCAP_STEREO */
    audio->capability = V4L2_AUDCAP_STEREO;
    return 0;
}

/*
* Function to set the driver audio params. Called
* by user-space via IOCTL call
*/
static int fm_v4l2_vidioc_s_audio(struct file *file, void *priv,
                    const struct v4l2_audio *audio)
{
    int ret = 0;
    if (audio->index != 0)
        ret = -EINVAL;
    return ret;
}

/* Get tuner attributes. This IOCTL call will return attributes like tuner type,
   upper/lower frequency, audio mode, RSSI value and AF channel */
static int fm_v4l2_vidioc_g_tuner(struct file *file, void *priv,
                    struct v4l2_tuner *tuner)
{
    unsigned short curr_rssi;
    unsigned int high = 0, low = 0;
    int ret = -EINVAL;
    struct fmdrv_ops *fmdev;
    unsigned char mode = 0;

    if (tuner->index != 0)
        return ret;

    fmdev = video_drvdata(file);
    strcpy(tuner->name, "FM");
    tuner->type = fmdev->device_info.type;
    /* The tuner's whole range (62.5 Hz units), not a seek's narrowing */
    low = FM_SET_FREQ(region_configs[fmdev->rx.curr_region].low_bound);
    high = FM_SET_FREQ(region_configs[fmdev->rx.curr_region].high_bound);
    tuner->rangelow = FM_BAND_LOW_62_5HZ(low);
    tuner->rangehigh = FM_BAND_LOW_62_5HZ(high);

    /*
     * audmode is the mode asked for (stereo means "when the signal allows",
     * i.e. auto-blend); what is being received now goes in rxsubchans. It
     * used to be the other way round: audmode followed the SNR.
     */
    tuner->audmode = (fmdev->rx.audio_mode == FM_MONO_MODE) ?
                    V4L2_TUNER_MODE_MONO : V4L2_TUNER_MODE_STEREO;
    ret = fmc_get_audio_mode(fmdev, &mode);
    tuner->rxsubchans = (ret == 0 && mode == FM_STEREO_MODE) ?
                    V4L2_TUNER_SUB_STEREO : V4L2_TUNER_SUB_MONO;
    if (fmdev->device_info.rxsubchans & V4L2_TUNER_SUB_RDS)
        tuner->rxsubchans |= V4L2_TUNER_SUB_RDS;
    V4L2_FM_DRV_DBG(V4L2_DBG_TX, "(fmdrv) tuner->audmode:%d", tuner->audmode);
    tuner->capability = fmdev->device_info.tuner_capability |
                    V4L2_TUNER_CAP_FREQ_BANDS | V4L2_TUNER_CAP_HWSEEK_BOUNDED |
                    V4L2_TUNER_CAP_HWSEEK_WRAP | V4L2_TUNER_CAP_HWSEEK_PROG_LIM;

    ret = fm_rx_read_curr_rssi_freq(fmdev, TRUE);
    curr_rssi = fmdev->rx.curr_rssi;
    /* RSSI from controller will be in range of -128 to +127.
     But V4L2 API defines the range of 0 to 65535. So convert this value
     FM rssi is cannot be 1~128dbm normally, although range is -128 to +127 */

    tuner->signal = (128-curr_rssi) * (65535 / 128);
    ret = 0;
    return ret;
}

/* Set tuner attributes. This IOCTL call will set attributes like
   upper/lower frequency, audio mode.
 */
static int fm_v4l2_vidioc_s_tuner(struct file *file, void *priv,
                    const struct v4l2_tuner *tuner)
{
    int ret = -EINVAL;
    struct fmdrv_ops *fmdev;
    unsigned short mode;
    if (tuner->index != 0)
        return ret;

    fmdev = video_drvdata(file);

    /* Only audmode is writable (V4L2 spec); the band is reported by
     * VIDIOC_ENUM_FREQ_BANDS and a seek's limits come with the seek. */

    /* Map V4L2 stereo/mono macro to Broadcom controller equivalent audio mode */
    mode = (tuner->audmode == V4L2_TUNER_MODE_STEREO) ?
        FM_AUTO_MODE : FM_MONO_MODE;

    ret = fmc_set_audio_mode(fmdev, mode);
    if (ret < 0)
        return ret;
    return 0;
}

/* Get tuner or modulator radio frequency */
static int fm_v4l2_vidioc_g_frequency(struct file *file, void *priv,
                    struct v4l2_frequency *freq)
{
    int ret;
    struct fmdrv_ops *fmdev;

    fmdev = video_drvdata(file);
    if (fmdev->curr_fmmode != FM_MODE_RX)
        return -EINVAL;
    /*
     * The frequency as the driver last read it from the chip: after every
     * tune, seek and AF jump. Asking the chip each time, as this did, is a
     * register read per call; the FM library calls it twice a second to
     * notice an AF jump, and RDS stopped coming while it did.
     */
    ret = 0;
    freq->frequency = FM_SET_FREQ(fmdev->rx.curr_freq);
    /* Translate the controller frequency to V4L2 specific frequency
        (frequencies in unit of 62.5 Hz):
        x = (y * 100) * 1000/62.5  = y * 160 */
    freq->frequency = (freq->frequency * 160);
    return ret;
}

/* Set tuner or modulator radio frequency, this is tune channel */
static int fm_v4l2_vidioc_s_frequency(struct file *file, void *priv,
                    const struct v4l2_frequency *freq)
{
    int ret = 0;
    struct fmdrv_ops *fmdev;
    struct v4l2_frequency fq;

    fmdev = video_drvdata(file);
    /* Translate the incoming tuner band frequencies
    (frequencies in unit of 62.5 Hz to controller
    recognized values. x = y * (62.5/1000000) * 100 = y / 160 */
    fq.frequency = (freq->frequency/160);
    ret = fmc_set_frequency(fmdev, fq.frequency);
    if (ret < 0)
        return ret;
    return 0;
}

/* Set hardware frequency seek. This is scanning radio stations. */
static int fm_v4l2_vidioc_s_hw_freq_seek(struct file *file, void *priv,
                    const struct v4l2_hw_freq_seek *seek)
{
    int ret = -EINVAL;
    struct fmdrv_ops *fmdev;

    fmdev = video_drvdata(file);

    V4L2_FM_DRV_DBG(V4L2_DBG_TX, "(fmdrv) direction:%d wrap:%d spacing:%u range:%u-%u", \
        seek->seek_upward, seek->wrap_around, seek->spacing,
        seek->rangelow, seek->rangehigh);

    if (seek->tuner != 0 || seek->type != V4L2_TUNER_RADIO)
        return -EINVAL;

    /* spacing in Hz; 0 keeps the current step */
    if (seek->spacing) {
        unsigned char step;

        switch (seek->spacing) {
        case 50000:  step = FM_STEP_50KHZ;  break;
        case 100000: step = FM_STEP_100KHZ; break;
        case 200000: step = FM_STEP_200KHZ; break;
        default:     return -EINVAL;
        }
        ret = fmc_set_scan_step(fmdev, step);
        if (ret < 0)
            return ret;
    }

    /* limits in 62.5 Hz units; both 0 means the whole band, so a seek
     * without limits undoes the previous one's */
    if (seek->rangelow || seek->rangehigh)
        ret = fm_rx_set_seek_range(fmdev, seek->rangelow / 160,
                                   seek->rangehigh / 160);
    else
        ret = fm_rx_set_seek_range(fmdev,
                FM_SET_FREQ(fmdev->rx.region.low_bound),
                FM_SET_FREQ(fmdev->rx.region.high_bound));
    if (ret < 0)
        return ret;

    ret = fmc_seek_station(fmdev, seek->seek_upward, seek->wrap_around);
    /* the V4L2 answer for "no station found" */
    if (ret == -EAGAIN)
        ret = -ENODATA;

    if (ret < 0)
        return ret;
    return 0;
}


/* This function is called whenever a process tries to
 * do an ioctl on this radio device. We get two extra
 * parameters (additional to the inode and file
 * structures, which all device functions get): the number
 * of the ioctl called and the parameter given to the
 * ioctl function.
 *
 * If the ioctl is write or read/write (meaning output
 * is returned to the calling process), the ioctl call
 * returns the output of this function.
 * Very important is that this is the Private IOCTL function to handle  RDA psrser
 * IOCTL commands. V4L2 framework also issues some IOCTLs which we have to route
 * them to video_ioctl2 function. So all the IOCTL other than Private ones are
 * passed to video_ioctl2 function.
 * And another important thing is that, it's good to return from this function as soon as
* we handle Private IOCTL so return statement is used insteadof break.
 */
long rds_info_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    /* Switch according to the ioctl called */
    long ret = 0;
/* Process if it is private IOCTL*/
    switch (cmd) {
        case IOCTL_GET_PI_CODE:
           V4L2_FM_DRV_DBG(V4L2_DBG_RX, "IOCTL_GET_PI_CODE");
           get_rds_element_value(GET_PI_CODE, (char *)arg);
           return 1;
        case IOCTL_GET_TP_CODE:
           V4L2_FM_DRV_DBG(V4L2_DBG_RX, "IOCTL_GET_TP_CODE");
           get_rds_element_value(GET_TP_CODE, (char *)arg);
           return 1;
        case IOCTL_GET_PTY_CODE:
           V4L2_FM_DRV_DBG(V4L2_DBG_RX, "IOCTL_GET_PTY_CODE");
           get_rds_element_value(GET_PTY_CODE, (char *)arg);
           return 1;
        case IOCTL_GET_TA_CODE:
           V4L2_FM_DRV_DBG(V4L2_DBG_RX, "IOCTL_GET_TA_CODE");
           get_rds_element_value(GET_TA_CODE, (char *)arg);
           return 1;
        case IOCTL_GET_MS_CODE:
           V4L2_FM_DRV_DBG(V4L2_DBG_RX, "IOCTL_GET_MS_CODE");
            get_rds_element_value(GET_MS_CODE, (char *)arg);
           return 1;
        case IOCTL_GET_PS_CODE:
           V4L2_FM_DRV_DBG(V4L2_DBG_RX, "IOCTL_GET_PS_CODE");
           get_rds_element_value(GET_PS_CODE, (char *)arg);
           return 1;
        case IOCTL_GET_RT_MSG:
           V4L2_FM_DRV_DBG(V4L2_DBG_RX, "IOCTL_GET_RT_MSG");
           get_rds_element_value(GET_RT_MSG, (char *)arg);
           return 1;
        case IOCTL_GET_CT_DATA:
           V4L2_FM_DRV_DBG(V4L2_DBG_RX, "IOCTL_GET_CT_DATA");
           get_rds_element_value(GET_CT_DATA, (char *)arg);
           return 1;
        case IOCTL_GET_TMC_CHANNEL:
           V4L2_FM_DRV_DBG(V4L2_DBG_RX, "IOCTL_GET_TMC_CHANNEL");
           get_rds_element_value(GET_TMC_CHANNEL, (char *)arg);
           return 1;
        default:
           V4L2_FM_DRV_DBG(V4L2_DBG_RX, "Invalid IOCTL");
           break;
    }
/* If anything other than private will be passed to V4L2 framework */
    ret = video_ioctl2(file, cmd, arg);
    return ret;
}

static const struct v4l2_file_operations fm_drv_fops = {
    .owner = THIS_MODULE,
    .read = fm_v4l2_fops_read,
    .write = fm_v4l2_fops_write,
    .poll = fm_v4l2_fops_poll,
/*This is the private IOCTL to handle RDS parser related IOCTLs*/
    .unlocked_ioctl = rds_info_ioctl,
    .open = fm_v4l2_fops_open,
    .release = fm_v4l2_fops_release,
};

static const struct v4l2_ioctl_ops fm_drv_ioctl_ops = {
    .vidioc_querycap = fm_v4l2_vidioc_querycap,
    .vidioc_enum_freq_bands = fm_v4l2_vidioc_enum_freq_bands,
    .vidioc_g_audio = fm_v4l2_vidioc_g_audio,
    .vidioc_s_audio = fm_v4l2_vidioc_s_audio,
    .vidioc_g_tuner = fm_v4l2_vidioc_g_tuner,
    .vidioc_s_tuner = fm_v4l2_vidioc_s_tuner,
    .vidioc_g_frequency = fm_v4l2_vidioc_g_frequency,
    .vidioc_s_frequency = fm_v4l2_vidioc_s_frequency,
    .vidioc_s_hw_freq_seek = fm_v4l2_vidioc_s_hw_freq_seek
};

/* V4L2 RADIO device parent structure */
static struct video_device fm_viddev_template = {
    .fops = &fm_drv_fops,
    .ioctl_ops = &fm_drv_ioctl_ops,
    .name = FM_DRV_NAME,
    .release = video_device_release,
    .vfl_type = VFL_TYPE_RADIO,
};

int fm_v4l2_init_video_device(struct fmdrv_ops *fmdev, int radio_nr)
{
    int ret = -ENOMEM;

    gradio_dev = NULL;
    /* Allocate new video device */
    gradio_dev = video_device_alloc();
    if (NULL == gradio_dev) {
        pr_err("(fmdrv): Can't allocate video device");
        return -ENOMEM;
    }

    /* Setup FM driver's V4L2 properties */
    memcpy(gradio_dev, &fm_viddev_template, sizeof(fm_viddev_template));

    video_set_drvdata(gradio_dev, fmdev);
    /* the V4L2 core holds it around every ioctl */
    gradio_dev->lock = &fmdev->mutex;

    strlcpy(fmdev->v4l2_dev.name, FM_DRV_NAME, sizeof(fmdev->v4l2_dev.name));
    ret = v4l2_device_register(NULL, &fmdev->v4l2_dev);
    if (ret < 0) {
        video_device_release(gradio_dev);
        V4L2_FM_DRV_ERR("(fmdrv): Could not register v4l2 device");
        return ret;
    }

    /*
     * Volume is the chip's digital level. FM's listening volume is set on
     * the codec (DAC1, the HAL's fm_volume), so the chip defaults to full
     * scale: the previous default of 150 would have made FM quieter as soon
     * as open applied it.
     */
    v4l2_ctrl_handler_init(&fmdev->ctrl_handler, 3);
    v4l2_ctrl_new_std(&fmdev->ctrl_handler, &fm_ctrl_ops,
                      V4L2_CID_AUDIO_VOLUME, FM_RX_VOLUME_MIN,
                      FM_RX_VOLUME_MAX, 1, FM_RX_VOLUME_MAX);
    v4l2_ctrl_new_std(&fmdev->ctrl_handler, &fm_ctrl_ops,
                      V4L2_CID_AUDIO_MUTE, 0, 1, 1, 0);
    /* 50 or 75 us; the chip cannot turn it off */
    fmdev->deemph_ctrl = v4l2_ctrl_new_std_menu(&fmdev->ctrl_handler,
                           &fm_ctrl_ops,
                           V4L2_CID_TUNE_DEEMPHASIS, V4L2_DEEMPHASIS_75_uS,
                           1 << V4L2_DEEMPHASIS_DISABLED,
                           region_configs[DEF_V4L2_FM_WORLD_REGION].deemphasis
                               == FM_DEEMPHA_75U ?
                           V4L2_DEEMPHASIS_75_uS : V4L2_DEEMPHASIS_50_uS);
    ret = fmdev->ctrl_handler.error;
    if (ret) {
        V4L2_FM_DRV_ERR("(fmdrv): Could not create controls (%d)", ret);
        goto err_ctrls;
    }
    fmdev->v4l2_dev.ctrl_handler = &fmdev->ctrl_handler;
    gradio_dev->v4l2_dev = &fmdev->v4l2_dev;
    gradio_dev->ctrl_handler = &fmdev->ctrl_handler;

    /* Register with V4L2 subsystem as RADIO device */
    if (video_register_device(gradio_dev, VFL_TYPE_RADIO, radio_nr)) {
        V4L2_FM_DRV_ERR("(fmdrv): Could not register video device");
        ret = -EINVAL;
        goto err_ctrls;
    }

    fmdev->radio_dev = gradio_dev;

    /*
     * The sysfs entries live as long as the device, not per open: release
     * holds the driver mutex, and removing the group there would wait for a
     * store that is itself waiting for the mutex. A store with FM closed
     * returns -EPERM.
     */
    if (sysfs_create_group(&gradio_dev->dev.kobj, &v4l2_fm_attr_grp))
        V4L2_FM_DRV_ERR("(fmdrv): failed to create sysfs entries");
    V4L2_FM_DRV_DBG(V4L2_DBG_INIT,"(fmdrv) registered with video device");
    return 0;

err_ctrls:
    v4l2_ctrl_handler_free(&fmdev->ctrl_handler);
    v4l2_device_unregister(&fmdev->v4l2_dev);
    video_device_release(gradio_dev);
    return ret;
}

void *fm_v4l2_deinit_video_device(void)
{
    struct fmdrv_ops *fmdev;

    fmdev = video_get_drvdata(gradio_dev);
    sysfs_remove_group(&gradio_dev->dev.kobj, &v4l2_fm_attr_grp);
    /* Unregister RADIO device from V4L2 subsystem */
    video_unregister_device(gradio_dev);
    v4l2_ctrl_handler_free(&fmdev->ctrl_handler);
    v4l2_device_unregister(&fmdev->v4l2_dev);

    return fmdev;
}
