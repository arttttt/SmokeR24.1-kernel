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
*  Filename:      antdrv_main.h
*
*  Description:   Header for ANT+ V4L2 driver
*
***********************************************************************************/

#ifndef _ANTDRV_MAIN_H
#define _ANTDRV_MAIN_H
#include <linux/module.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

/*******************************************************************************
**  Constants & Macros
*******************************************************************************/


#define REG_RD       0x1
#define REG_WR       0x0
#define VSC_HCI_CMD  0x2

#define HCI_COMMAND  0x01
#define ANT_OP_CODE         0x00ec
#define HCI_GRP_VENDOR_SPECIFIC 0x3F
#define BRCM_VS_EVENT 0xff


/*******************************************************************************
**  Type definitions
*******************************************************************************/

/* SKB helpers */
struct ant_skb_cb {
    __u8 ant_opcode;
    struct completion *completion;
};

#define ant_cb(skb) ((struct ant_skb_cb *)(skb->cb))

struct ant_cmd_msg_hdr {
    __u16 cmd;          /* vendor specific command */
    __u8 len;           /* Number of bytes follows */
} __attribute__ ((packed));

struct ant_vsc_hci_cmd_msg_hdr {
    __u8 header;        /* type, HCI_COMMAND */
    __u16 cmd;          /* vendor specific command */
    __u8 len;           /* Number of bytes follows */
    __u8 prefix;
} __attribute__ ((packed));



#define ANT_CMD_MSG_HDR_SIZE    6    /* sizeof(struct ant_cmd_msg_hdr) */
//#define ANT_VSC_HCI_CMD_MSG_HDR_SIZE   sizeof(ant_vsc_hci_cmd_msg_hdr)



//#define ANT_EVT_MSG_HDR_SIZE     2 /* sizeof(struct ant_event_hdr) */
struct ant_cmd_complete_hdr {
    struct hci_ev_cmd_complete hci_cmd_complete;
    __u8 status;
} __attribute__ ((packed));

/* Function forward declaration */
int ant_send_user_buf_vsc(struct antdrv_ops* antdrv,
    const char __user * buf, size_t count);

int ant_prepare(struct antdrv_ops *antdev);

int ant_release(struct antdrv_ops *antdev);

#endif

