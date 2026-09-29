/*
 * Common stats definitions for clients of dongle
 * ports
 *
 * Copyright (C) 1999-2015, Broadcom Corporation
 * 
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 * 
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 * 
 *      Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a license
 * other than the GPL, without Broadcom's express prior written consent.
 *
 * $Id: dngl_stats.h 464743 2014-03-25 21:04:32Z $
 */

#ifndef _dngl_stats_h_
#define _dngl_stats_h_

typedef struct {
	unsigned long	rx_packets;		/* total packets received */
	unsigned long	tx_packets;		/* total packets transmitted */
	unsigned long	rx_bytes;		/* total bytes received */
	unsigned long	tx_bytes;		/* total bytes transmitted */
	unsigned long	rx_errors;		/* bad packets received */
	unsigned long	tx_errors;		/* packet transmit problems */
	unsigned long	rx_dropped;		/* packets dropped by dongle */
	unsigned long	tx_dropped;		/* packets dropped by dongle */
	unsigned long   multicast;      /* multicast packets received */
} dngl_stats_t;

#ifdef LINKSTAT_SUPPORT
/*
 * Link layer statistics as the wifi HAL of Android 11 reads them: a radio
 * record, its channel records, then an interface record laid out as
 * hardware/libhardware_legacy link_layer_stats.h has it on R. The radio
 * record is the HAL's wifi_radio_stat_internal (no tx power levels); the
 * interface record carries average_tsf_offset and the leaky-AP fields that
 * the M-era copy of these structures in other bcmdhd trees lacks, which
 * would shift everything after beacon_rx.
 */
typedef int wifi_radio;
typedef int wifi_channel;
typedef int wifi_rssi;

typedef enum {
	WIFI_DISCONNECTED = 0,
	WIFI_AUTHENTICATING = 1,
	WIFI_ASSOCIATING = 2,
	WIFI_ASSOCIATED = 3,
	WIFI_EAPOL_STARTED = 4,
	WIFI_EAPOL_COMPLETED = 5
} wifi_connection_state;

typedef enum {
	WIFI_ROAMING_IDLE = 0,
	WIFI_ROAMING_ACTIVE = 1
} wifi_roam_state;

typedef enum {
	WIFI_INTERFACE_STA = 0,
	WIFI_INTERFACE_SOFTAP = 1,
	WIFI_INTERFACE_IBSS = 2,
	WIFI_INTERFACE_P2P_CLIENT = 3,
	WIFI_INTERFACE_P2P_GO = 4,
	WIFI_INTERFACE_NAN = 5,
	WIFI_INTERFACE_MESH = 6
} wifi_interface_mode;

typedef struct {
	wifi_interface_mode mode;
	uint8 mac_addr[6];
	wifi_connection_state state;
	wifi_roam_state roaming;
	uint32 capabilities;
	uint8 ssid[33];
	uint8 bssid[6];
	uint8 ap_country_str[3];
	uint8 country_str[3];
} wifi_interface_link_layer_info;

typedef struct {
	int width;
	wifi_channel center_freq;
	wifi_channel center_freq0;
	wifi_channel center_freq1;
} wifi_channel_info;

typedef struct {
	wifi_channel_info channel;
	uint32 on_time;
	uint32 cca_busy_time;
} wifi_channel_stat;

typedef struct {
	wifi_radio radio;
	uint32 on_time;
	uint32 tx_time;
	uint32 rx_time;
	uint32 on_time_scan;
	uint32 on_time_nbd;
	uint32 on_time_gscan;
	uint32 on_time_roam_scan;
	uint32 on_time_pno_scan;
	uint32 on_time_hs20;
	uint32 num_channels;
	wifi_channel_stat channels[];
} wifi_radio_stat;

typedef enum {
	WIFI_AC_VO  = 0,
	WIFI_AC_VI  = 1,
	WIFI_AC_BE  = 2,
	WIFI_AC_BK  = 3,
	WIFI_AC_MAX = 4
} wifi_traffic_ac;

typedef struct {
	wifi_traffic_ac ac;
	uint32 tx_mpdu;
	uint32 rx_mpdu;
	uint32 tx_mcast;
	uint32 rx_mcast;
	uint32 rx_ampdu;
	uint32 tx_ampdu;
	uint32 mpdu_lost;
	uint32 retries;
	uint32 retries_short;
	uint32 retries_long;
	uint32 contention_time_min;
	uint32 contention_time_max;
	uint32 contention_time_avg;
	uint32 contention_num_samples;
} wifi_wmm_ac_stat;

typedef struct {
	uint32 iface;			/* a HAL pointer; userspace is 32-bit here */
	wifi_interface_link_layer_info info;
	uint32 beacon_rx;
	uint64 average_tsf_offset;
	uint32 leaky_ap_detected;
	uint32 leaky_ap_avg_num_frames_leaked;
	uint32 leaky_ap_guard_time;
	uint32 mgmt_rx;
	uint32 mgmt_action_rx;
	uint32 mgmt_action_tx;
	wifi_rssi rssi_mgmt;
	wifi_rssi rssi_data;
	wifi_rssi rssi_ack;
	wifi_wmm_ac_stat ac[WIFI_AC_MAX];
	uint32 num_peers;
} wifi_iface_stat;
#endif /* LINKSTAT_SUPPORT */

#endif /* _dngl_stats_h_ */
