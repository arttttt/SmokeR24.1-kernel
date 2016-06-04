/*
 *  ANT+ Driver for Connectivity chip of Broadcom Corporation.
 *
 *  This sub-module of ANT+ driver is common for ant+ RX and TX
 *  functionality. This module is responsible for:
 *  Copyright (C) 2009 Texas Instruments
 *  Copyright (C) 2009-2015 Broadcom Corporation
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
/************************************************************************************
 *
 *  Filename:      antdrv_main.c
 *
 *  Description:   Common sub-module for both ant Rx and Tx
 *
 ***********************************************************************************/

#include <linux/module.h>
#include <linux/delay.h>
#include "antdrv.h"
#include "antdrv_v4l2.h"
#include "antdrv_main.h"
#include "../include/brcm_ldisc_sh.h"
//#include "ant_public.h"
//#include "antdrv_config.h"
#include "../include/v4l2_target.h"
#include "../include/v4l2_logs.h"

#ifndef V4L2_ANT_DEBUG
#define V4L2_ANT_DEBUG TRUE
#endif

/* set this module parameter to enable debug info */
int ant_dbg_param = -1;

/*******************************************************************************
**  Static Variables
*******************************************************************************/


/* Radio Nr */
static int radio_nr = -1;
module_param(radio_nr, int, 1);
MODULE_PARM_DESC(radio_nr, "Radio Nr");

/*******************************************************************************
**  Forward definitions
*******************************************************************************/

int ant_send_user_buf_vsc(struct antdrv_ops* antdrv, const char __user * buf, size_t count)
{
    size_t size = sizeof(struct ant_vsc_hci_cmd_msg_hdr) + count;
    struct sk_buff *skb = alloc_skb(size, GFP_ATOMIC);
    struct ant_vsc_hci_cmd_msg_hdr *cmd_hdr;
    char* p = NULL;
    int ret = 0;
    if (!skb)
    {
        pr_err("(antdrv): No memory to create new SKB");
        return -ENOMEM;
    }
    sh_ldisc_cb(skb)->pkt_type = ANT_PKT;
    cmd_hdr = (struct ant_vsc_hci_cmd_msg_hdr *)skb_put(skb, sizeof(*cmd_hdr));
    cmd_hdr->header = HCI_COMMAND_PKT;    /* 0x01 */
    cmd_hdr->cmd = hci_opcode_pack(HCI_GRP_VENDOR_SPECIFIC, ANT_OP_CODE);
    cmd_hdr->len = count + 1;
    cmd_hdr->prefix = 0xff;
    if (copy_from_user(skb_put(skb, count), buf, count)) {
        kfree_skb(skb);
        _ale("copy_from_user failed");
        return -EFAULT;
    }
    p = (char*)skb->data;
    _al("header:%02x %02x %02x %02x %02x, payload len: %d, skb->len:%d", p[0], p[1], p[2], p[3], p[4], count, skb->len);
    p += 5;
    if(count == 3){
        _al("payload:%02x %02x %02x", p[0], p[1], p[2]);
    }
    else if(count == 4) {
        _al("payload:%02x %02x %02x %02x", p[0], p[1], p[2], p[3]);
    }

    ret = antdrv->bcm_write(skb);
    if(ret < 0) {
        _ale("bcm_write failed");
        kfree_skb(skb);
    }
    return ret;
}
/* Called by LDisc layer when ant packet is available. The pointer to
* this function is registered to LDisc during brcm_sh_ldisc_register() call.*/
static long ant_st_receive(void *arg, struct sk_buff *skb)
{
    struct antdrv_ops *antdev;
    unsigned long flags;
    //__u8 pkt_type = ANT_PKB;
    struct ant_event_hdr *ant_evt_hdr;

    antdev = (struct antdrv_ops *)arg;

    if (skb == NULL) {
        _ale(": Invalid SKB received from LDisp");
        return -EFAULT;
    }
    //set tx_wake_flag for command complete and queue ant event here, no rx work queue needed.
    ant_evt_hdr = (struct ant_event_hdr *)skb->data;
    _al("ant_event_hdr: event:%x, plen:%d", ant_evt_hdr->event, ant_evt_hdr->plen);
    if (ant_evt_hdr->event == HCI_EV_CMD_COMPLETE)
    {
        struct ant_cmd_complete_hdr *cmd_complete_hdr;
        cmd_complete_hdr = (struct ant_cmd_complete_hdr *) (ant_evt_hdr + 1);
        _al("ant_cmd_complete_hdr: ncmd:%d, opcode:%x, status:%x",
                cmd_complete_hdr->hci_cmd_complete.ncmd, cmd_complete_hdr->hci_cmd_complete.opcode,
                cmd_complete_hdr->status);
        antdev->tx_wake_flag = cmd_complete_hdr->status ? 2 : 1;
        wake_up_interruptible(&antdev->tx_wait);
        kfree_skb(skb);
    }
    else if(ant_evt_hdr->event == BRCM_VS_EVENT) {/* Vendor specific Event */
        __u8 sub_evt = *(__u8*)(ant_evt_hdr + 1);
        _al("ant sub event id:%02x, skb->len:%d", sub_evt, skb->len);
        if(sub_evt == 0x2d) {
            skb_pull(skb, sizeof(*ant_evt_hdr) + 2);
            _al("after pull, skb->len:%d: %02x %02x %02x %02x",
                skb->len, skb->data[0], skb->data[1], skb->data[2], skb->data[3]);

             spin_lock_irqsave(&(antdev->rx_q.lock), flags);
            __skb_queue_tail(&antdev->rx_q, skb);
            if(antdev->rx_wake_flag == 0) {
                antdev->rx_wake_flag = 1;
                wake_up_interruptible(&antdev->rx_wait);
            }
            spin_unlock_irqrestore(&antdev->rx_q.lock, flags);
        } else {
            _ale("unknown event id:%02x", sub_evt);
            kfree_skb(skb);
        }
    } else {
        pr_err("Unhandled packet SKB(%p),purging", skb);
        kfree_skb(skb);
    }

    return 0;
}

/*
 * This function will be called from ant V4L2 open function.
 * Register with shared ldisc driver and initialize driver data.
 */
int ant_prepare(struct antdrv_ops *antdev)
{
    static struct sh_proto_s ant_st_proto;
    int ret = 0;

    if (test_bit(ANT_CORE_READY, &antdev->flag)) {
        V4L2_ANT_DBG(V4L2_DBG_OPEN, "ANT Core is already up");
        return ret;
    }
    antdev->bcm_write = NULL;

    memset(&ant_st_proto, 0, sizeof(ant_st_proto));
    ant_st_proto.type = PROTO_SH_ANT;
    ant_st_proto.recv = ant_st_receive;
    ant_st_proto.match_packet = NULL;
    ant_st_proto.write = NULL; /* shared ldisc driver will fill write pointer */
    ant_st_proto.priv_data = antdev;

    /* Register with the shared line discipline */
    ret = brcm_sh_ldisc_register(&ant_st_proto);
    if (ret == -1) {
        pr_err(": brcm_sh_ldisc_register failed %d", ret);
        ret = -EAGAIN;
        return ret;
    }
    else {
        V4L2_ANT_DBG(V4L2_DBG_OPEN,"brcm_sh_ldisc_register sucess %d", ret);
    }
    if (ant_st_proto.write != NULL) {
        antdev->bcm_write = ant_st_proto.write;
    }
    else {
        _ale("Failed to get shared ldisc write func pointer");
        ret = brcm_sh_ldisc_unregister(PROTO_SH_ANT);
        if (ret < 0)
            _ale(": brcm_sh_ldisc_unregister failed %d", ret);
            ret = -EAGAIN;
            return ret;
    }

    /* Initialize RX Queue */
    skb_queue_head_init(&antdev->rx_q);

    antdev->device_info.capabilities = V4L2_CAP_RADIO | V4L2_CAP_MODULATOR | V4L2_CAP_READWRITE;
    antdev->device_info.type = V4L2_TUNER_RADIO;

    antdev->rx_wake_flag = 0;
    init_waitqueue_head(&antdev->rx_wait);

    antdev->tx_wake_flag = 1;
    init_waitqueue_head(&antdev->tx_wait);

    set_bit(ANT_CORE_READY, &antdev->flag);
    _al("out: ret:%d", ret);
    return ret;
}

/* This function will be called from ANT+ V4L2 release function.
 * Unregister from line discipline driver.
 */
int ant_release(struct antdrv_ops *antdev)
{
    int ret;
    unsigned long flags;
    struct sk_buff *skb;
    V4L2_ANT_DBG(V4L2_DBG_CLOSE, " %s", __func__);

    if (!test_bit(ANT_CORE_READY, &antdev->flag)) {
        V4L2_ANT_DBG(V4L2_DBG_CLOSE, ": ANT Core is already down");
        return 0;
    }

    ret = brcm_sh_ldisc_unregister(PROTO_SH_ANT);
    if (ret < 0)
        _ale(": Failed to de-register ant from HCI LDisc - %d", ret);
    else
        V4L2_ANT_DBG(V4L2_DBG_CLOSE, ": Successfully unregistered from  HCI LDisc");

    /* Sevice pending read */
    spin_lock_irqsave(&antdev->rx_q.lock, flags);
    while ((skb = __skb_dequeue(&antdev->rx_q)) != NULL)
        kfree_skb(skb);
    spin_unlock_irqrestore(&antdev->rx_q.lock, flags);

    clear_bit(ANT_CORE_READY, &antdev->flag);
    return ret;
}

/* Module init function. Ask V4L module to register video device.
 * Allocate memory for ant+ driver context
 */
static int __init ant_drv_init(void)
{
    struct antdrv_ops *antdev = NULL;
    int ret = -ENOMEM;

    pr_info("ant driver version %s", ANT_DRV_VERSION);

    antdev = kzalloc(sizeof(struct antdrv_ops), GFP_KERNEL);
    if (NULL == antdev) {
        _ale("Can't allocate operation structure memory");
        return ret;
    }

    ret = ant_v4l2_init_video_device(antdev, radio_nr);
    if (ret < 0)
    {
        kfree(antdev);
        return ret;
    }
    return 0;
}

/* Module exit function. Ask ant V4L module to unregister video device */
static void __exit ant_drv_exit(void)
{
    struct antdrv_ops *antdev = NULL;
    V4L2_ANT_DBG(V4L2_DBG_INIT, "ant_drv_exit");

    antdev = ant_v4l2_deinit_video_device();
    if (antdev != NULL) {
        kfree(antdev);
    }
}

module_init(ant_drv_init);
module_exit(ant_drv_exit);

module_param(ant_dbg_param, int, S_IRUGO);
MODULE_PARM_DESC(ant_dbg_param, \
               "Set to integer value from 1 to 31 for enabling/disabling" \
               " specific categories of logs");


/* ------------- Module Info ------------- */
MODULE_AUTHOR("Zhenye Zhu <zhenye@broadcom.com>");
MODULE_DESCRIPTION("ANT+ Driver for Connectivity chip of Broadcom Corporation");
MODULE_VERSION(VERSION); /* defined in makefile */
MODULE_LICENSE("GPL");
