/*
 * Packet fate monitoring: the frames the wifi HAL's startPktFateMonitoring,
 * getTxPktFates and getRxPktFates ask about.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#ifndef _dhd_pktmon_h_
#define _dhd_pktmon_h_

#ifdef DBG_PKT_MON

#include <linux/compiler.h>
#include <typedefs.h>
#include <dhd.h>

int dhd_pktmon_start(dhd_pub_t *dhdp);
void dhd_pktmon_free(dhd_pub_t *dhdp);

void dhd_pktmon_tx(dhd_pub_t *dhdp, void *pkt);
void dhd_pktmon_tx_status(dhd_pub_t *dhdp, void *pkt, uint8 wlfc_status);
void dhd_pktmon_tx_done(dhd_pub_t *dhdp, void *pkt, bool success);
void dhd_pktmon_rx(dhd_pub_t *dhdp, void *pkt);

int dhd_pktmon_get_tx(dhd_pub_t *dhdp, void __user *buf, uint16 req, uint16 *resp);
int dhd_pktmon_get_rx(dhd_pub_t *dhdp, void __user *buf, uint16 req, uint16 *resp);

#endif /* DBG_PKT_MON */

#endif /* _dhd_pktmon_h_ */
