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
 *  Filename:      antdrv_v4l2.c
 *
 *  Description:   ANT+ Driver for Connectivity chip of Broadcom Corporation.
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

#include "antdrv.h"
#include "antdrv_v4l2.h"
#include "antdrv_main.h"
//#include "ant_public.h"
//#include "antdrv_config.h"
#include "../include/v4l2_target.h"
#include "../include/v4l2_logs.h"


/* Required to set "THIS_MODULE" during driver registration to linux system. Not required in Maguro */
#ifndef V4L2_THIS_MODULE_SUPPORT
#include <linux/export.h>
#endif

/************************************************************************************
**  Constants & Macros
************************************************************************************/
#ifndef V4L2_ANT_DEBUG
#define V4L2_ANT_DEBUG TRUE
#endif

extern int ant_dbg_param;



/************************************************************************************
**  Static variables
************************************************************************************/

static struct video_device *gradio_dev;
static int ant_ready;

static atomic_t v4l2_device_available = ATOMIC_INIT(1);

/************************************************************************************
**  Forward function declarations
************************************************************************************/


/************************************************************************************
**  Functions
************************************************************************************/
/*****************************************************************************
**   V4L2 RADIO (/dev/radioX) device file operation interfaces
*****************************************************************************/

/* Read RX RDS data */

static ssize_t ant_v4l2_fops_read(struct file *file, char __user * buf,
                    size_t count, loff_t *ppos)
{
    struct antdrv_ops *antdev;
    unsigned long flags;
    struct sk_buff *head = NULL;
    int ret = -EIO;
    _al("in, count:%d", count);
    antdev = video_drvdata(file);

    if (!ant_ready) {
        _ale("ANT device is already disconnected\n");
        return -EIO;
    }
    if(wait_event_interruptible(antdev->rx_wait, (antdev->rx_wake_flag != 0))) {
        _ale("ANT device is interrupted\n");
        return -ERESTARTSYS;
    }
    if (!ant_ready) {
        _ale("ANT device is already disconnected\n");
        return -EIO;
    }

    spin_lock_irqsave(&antdev->rx_q.lock, flags);
    if((head = skb_peek(&antdev->rx_q)) != NULL) {
        _al("head->len:%d", head->len);
        if(head->len <= count) {
            head = __skb_dequeue(&antdev->rx_q);
            if(copy_to_user(buf, head->data, head->len))
                ret = -EFAULT;
            else ret = head->len;
            kfree_skb(head);
            if(skb_queue_empty(&antdev->rx_q))
                antdev->rx_wake_flag = 0;
        }
        else ret = -EINVAL;
    }
    spin_unlock_irqrestore(&antdev->rx_q.lock, flags);
    _al("out, count:%d, ret:%d", count, ret);
    return ret;
}

static ssize_t ant_v4l2_fops_write(struct file *file, const char __user * buf,
                    size_t count, loff_t *ppos)
{
    struct antdrv_ops *antdev;
    int ret;
    _al("in, count:%d", count);
    antdev = video_drvdata(file);
    if (!ant_ready) {
        _ale("ANT device is already disconnected\n");
        return -EIO;
    }

    for(;;)
    {
        antdev->tx_wake_flag = 0;
        ret = ant_send_user_buf_vsc(antdev, buf, count);
        if(ret > 0)
        {
            ret = count;
            if(wait_event_interruptible(antdev->tx_wait, (antdev->tx_wake_flag != 0))) {
                _ale("ANT device is interrupted\n");
                return -ERESTARTSYS;
            }
        } else break;
        if(antdev->tx_wake_flag == 2) {
            msleep(100);
        } else break;
    } 
    _al("out, ret:%d", ret);
    return ret;
}

static unsigned int ant_v4l2_fops_poll(struct file *file,
                      struct poll_table_struct *pts)
{
    int ret = 0;
    struct antdrv_ops *antdev;

    V4L2_ANT_DBG(V4L2_DBG_RX, "in");

    antdev = video_drvdata(file);
    if (!ant_ready)
		return POLLHUP;
	ret = 0;
	if (antdev->rx_wake_flag)
		ret |= POLLIN | POLLRDNORM;
	if (antdev->tx_wake_flag)
		ret |= POLLOUT;
    return ret;
}


static int ant_v4l2_fops_open(struct file *file)
{
    int ret = -EINVAL;
    struct antdrv_ops *antdev = NULL;
    V4L2_ANT_DBG(V4L2_DBG_OPEN, "in");
    /* Don't allow multiple open */
    if(!atomic_dec_and_test(&v4l2_device_available))
    {
        atomic_inc(&v4l2_device_available);
        _ale("ant device is already opened\n");
        return -EBUSY;
    }

    if (ant_ready) {
        _ale("ant device is already opened\n");
        return  -EBUSY;
    }

    antdev = video_drvdata(file);
    /* initialize the driver */
    ret = ant_prepare(antdev);
    if (ret < 0) {
        _ale("Unable to prepare ANT CORE");
        return ret;
    }

    ant_ready = TRUE;

    return 0;
}
static int ant_v4l2_fops_release(struct file *file)
{
    int ret =  -EINVAL;
    struct antdrv_ops *antdev;
    V4L2_ANT_DBG(V4L2_DBG_CLOSE, "in");

    antdev = video_drvdata(file);

    if (!ant_ready) {
        V4L2_ANT_DBG(V4L2_DBG_CLOSE, "(ant):ant dev already closed, close called again?");
        return ret;
    }
    ant_ready = FALSE;
    wake_up_interruptible(&antdev->tx_wait);
    wake_up_interruptible(&antdev->rx_wait);
    ret = ant_release(antdev);
    if (ret < 0)
    {
        _ale("ANT CORE release failed");
        return ret;
    }
    atomic_inc(&v4l2_device_available);
    return 0;
}

/*****************************************************************************
**   V4L2 RADIO (/dev/radioX) device IOCTL interfaces
*****************************************************************************/

static const struct v4l2_file_operations ant_drv_fops = {
    .owner = THIS_MODULE,
    .read = ant_v4l2_fops_read,
    .write = ant_v4l2_fops_write,
    .poll = ant_v4l2_fops_poll,
    /* Since no private IOCTLs are supported currently,
    direct all calls to video_ioctl2() */
    .ioctl = video_ioctl2,
    .open = ant_v4l2_fops_open,
    .release = ant_v4l2_fops_release,
};
/* V4L2 RADIO device parent structure */
static struct video_device ant_viddev_template = {
    .fops = &ant_drv_fops,
    .ioctl_ops = NULL, //&v4l2_ioctl_ops,
    .name = ANT_DRV_NAME,
    .release = video_device_release,
    .vfl_type = VFL_TYPE_RADIO,
};

int ant_v4l2_init_video_device(struct antdrv_ops *antdev, int radio_nr)
{
    int ret = -ENOMEM;

    /* Allocate new video device */
    gradio_dev = video_device_alloc();
    if (NULL == gradio_dev) {
        pr_err("antdrv can't allocate video device");
        return -ENOMEM;
    }

    memcpy(gradio_dev, &ant_viddev_template, sizeof(ant_viddev_template));

    video_set_drvdata(gradio_dev, antdev);

    /* Register with V4L2 subsystem as RADIO device */
    if (video_register_device(gradio_dev, VFL_TYPE_RADIO, radio_nr)) {
        video_device_release(gradio_dev);
        _ale("(antdrv): Could not register video device");
        return -EINVAL;
    }

    antdev->radio_dev = gradio_dev;
    V4L2_ANT_DBG(V4L2_DBG_INIT,"(antdrv) registered with video device");
    ret = 0;

    return ret;
}

void *ant_v4l2_deinit_video_device(void)
{
    struct antdrv_ops *antdev;

    antdev = video_get_drvdata(gradio_dev);
    /* Unregister RADIO device from V4L2 subsystem */
    video_unregister_device(gradio_dev);

    return antdev;
}
