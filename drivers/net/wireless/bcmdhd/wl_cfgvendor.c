/*
 * Linux cfg80211 Vendor Extension Code
 *
 * Copyright (C) 1999-2014, Broadcom Corporation
 * Copyright (C) 2015-2017, NVIDIA CORPORATION. All rights reserved.
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
 * $Id: wl_cfgvendor.c 473890 2014-04-30 01:55:06Z $
*/

/*
 * New vendor interface additon to nl80211/cfg80211 to allow vendors
 * to implement proprietary features over the cfg80211 stack.
*/

#include <typedefs.h>
#include <linuxver.h>
#include <osl.h>
#include <linux/kernel.h>

#include <bcmutils.h>
#include <bcmwifi_channels.h>
#include <bcmendian.h>
#include <proto/ethernet.h>
#include <proto/802.11.h>
#include <linux/if_arp.h>
#include <asm/uaccess.h>


#include <dngl_stats.h>
#include <dhd.h>
#include <dhdioctl.h>
#include <wlioctl.h>
#include <dhd_cfg80211.h>
#ifdef PNO_SUPPORT
#include <dhd_pno.h>
#endif /* PNO_SUPPORT */
#ifdef RTT_SUPPORT
#include <dhd_rtt.h>
#endif /* RTT_SUPPORT */
#include <proto/ethernet.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/netdevice.h>
#include <linux/sched.h>
#include <linux/etherdevice.h>
#include <linux/wireless.h>
#include <linux/ieee80211.h>
#include <linux/wait.h>
#include <linux/vmalloc.h>
#include <net/cfg80211.h>
#include <net/rtnetlink.h>

#include <wlioctl.h>
#include <wldev_common.h>
#include <wl_cfg80211.h>
#include <wl_cfgp2p.h>
#include <wl_android.h>
#include <wl_cfgvendor.h>
#include <dhd_pktmon.h>
#include <dhd_bus.h>
#ifdef PROP_TXSTATUS
#include <dhd_wlfc.h>
#endif

#if (LINUX_VERSION_CODE > KERNEL_VERSION(3, 13, 0)) || defined(WL_VENDOR_EXT_SUPPORT)
/*
 * This API is to be used for asynchronous vendor events. This
 * shouldn't be used in response to a vendor command from its
 * do_it handler context (instead wl_cfgvendor_send_cmd_reply should
 * be used).
 */
int wl_cfgvendor_send_async_event(struct wiphy *wiphy,
	struct net_device *dev, int event_id, const void  *data, int len)
{
	u16 kflags;
	struct sk_buff *skb;

	kflags = in_atomic() ? GFP_ATOMIC : GFP_KERNEL;

	/* Alloc the SKB for vendor_event */
#ifdef VENDOR_NET_SKB_ALLOC
	skb = cfg80211_vendor_event_skb_alloc(dev, wiphy, len, event_id, kflags);
#else
#if defined(CONFIG_ARCH_MSM) && defined(SUPPORT_WDEV_CFG80211_VENDOR_EVENT_ALLOC)
	skb = cfg80211_vendor_event_alloc(wiphy, NULL, len, event_id, kflags);
#else
	skb = cfg80211_vendor_event_alloc(wiphy, len, event_id, kflags);
#endif /* CONFIG_ARCH_MSM && SUPPORT_WDEV_CFG80211_VENDOR_EVENT_ALLOC */
#endif /* VENDOR_NET_SKB_ALLOC */
	if (!skb) {
		WL_ERR(("skb alloc failed"));
		return -ENOMEM;
	}

	/* Push the data to the skb */
	nla_put_nohdr(skb, len, data);

	cfg80211_vendor_event(skb, kflags);

	return 0;
}

static int wl_cfgvendor_send_cmd_reply(struct wiphy *wiphy,
	struct net_device *dev, const void  *data, int len)
{
	struct sk_buff *skb;

	/* Alloc the SKB for vendor_event */
	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, len);
	if (unlikely(!skb)) {
		WL_ERR(("skb alloc failed"));
		return -ENOMEM;
	}

	/* Push the data to the skb */
	nla_put_nohdr(skb, len, data);

	return cfg80211_vendor_cmd_reply(skb);
}

static int wl_cfgvendor_unsupported_feature(struct wiphy *wiphy,
        struct wireless_dev *wdev, const void  *data, int len)
{
	// return unsupported error code
	return WIFI_ERROR_NOT_SUPPORTED;
}

static int wl_cfgvendor_set_country(struct wiphy *wiphy,
        struct wireless_dev *wdev, const void  *data, int len)
{
        struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
        int err = BCME_ERROR, rem, type;
        char country_code[WLC_CNTRY_BUF_SZ] = {0};
        const struct nlattr *iter;

        nla_for_each_attr(iter, data, len, rem) {
                type = nla_type(iter);
                switch (type) {
                        case ANDR_WIFI_ATTRIBUTE_COUNTRY:
                                memcpy(country_code, nla_data(iter),
                                        MIN(nla_len(iter), WLC_CNTRY_BUF_SZ));
                                break;
                        default:
                                WL_ERR(("Unknown type: %d\n", type));
                                return err;
                }
        }

        /*
         * The country is the radio's, whichever interface the HAL names: R
         * sets it on the hotspot's before the hotspot comes up, and there
         * the disassoc that went first failed (BCME_NOTSTA) and took the
         * country with it. Through the primary, as the COUNTRY command and
         * the regulatory notifier do, and like them without the disassoc,
         * which would drop the station each time a hotspot starts beside it.
         */
        err = wldev_set_country(bcmcfg_to_prmry_ndev(cfg), country_code, true, false);
        if (err < 0) {
                WL_ERR(("Set country failed ret:%d\n", err));
        }

        return err;
}

#ifdef GSCAN_SUPPORT
static int wl_cfgvendor_gscan_get_channel_list(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0, type, band;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	uint16 *reply = NULL;
	uint32 reply_len = 0, num_channels, mem_needed;
	struct sk_buff *skb;

	type = nla_type(data);

	if (type == GSCAN_ATTRIBUTE_BAND) {
		band = nla_get_u32(data);
	} else {
		return -1;
	}

	reply = dhd_dev_pno_get_gscan(bcmcfg_to_prmry_ndev(cfg),
	   DHD_PNO_GET_CHANNEL_LIST, &band, &reply_len);

	if (!reply) {
		WL_ERR(("Could not get channel list\n"));
		err = -EINVAL;
		return err;
	}
	num_channels =  reply_len/ sizeof(uint32);
	mem_needed = reply_len + VENDOR_REPLY_OVERHEAD + (ATTRIBUTE_U32_LEN * 2);

	/* Alloc the SKB for vendor_event */
	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, mem_needed);
	if (unlikely(!skb)) {
		WL_ERR(("skb alloc failed"));
		err = -ENOMEM;
		goto exit;
	}

	nla_put_u32(skb, GSCAN_ATTRIBUTE_NUM_CHANNELS, num_channels);
	nla_put(skb, GSCAN_ATTRIBUTE_CHANNEL_LIST, reply_len, reply);

	err =  cfg80211_vendor_cmd_reply(skb);

	if (unlikely(err))
		WL_ERR(("Vendor Command reply failed ret:%d \n", err));
exit:
	kfree(reply);
	return err;
}
#endif /* GSCAN_SUPPORT */

#if defined(KEEP_ALIVE)
static int wl_cfgvendor_start_mkeep_alive(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	/* max size of IP packet for keep alive */
	const int MKEEP_ALIVE_IP_PKT_MAX = 256;

	int ret = BCME_OK, rem, type;
	u8 mkeep_alive_id = 0;
	u8 *ip_pkt = NULL;
	u16 ip_pkt_len = 0;
	u8 src_mac[ETHER_ADDR_LEN];
	u8 dst_mac[ETHER_ADDR_LEN];
	u32 period_msec = 0;
	u16 ether_type = ETHER_TYPE_IP;
	const struct nlattr *iter;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pub_t *dhd_pub = cfg->pub;
	gfp_t kflags = in_atomic() ? GFP_ATOMIC : GFP_KERNEL;
	nla_for_each_attr(iter, data, len, rem) {
		type = nla_type(iter);
		switch (type) {
			case MKEEP_ALIVE_ATTRIBUTE_ID:
				mkeep_alive_id = nla_get_u8(iter);
				break;
			case MKEEP_ALIVE_ATTRIBUTE_IP_PKT_LEN:
				ip_pkt_len = nla_get_u16(iter);
				if (ip_pkt_len > MKEEP_ALIVE_IP_PKT_MAX) {
					ret = BCME_BADARG;
					goto exit;
				}
				break;
			case MKEEP_ALIVE_ATTRIBUTE_IP_PKT:
				if (!ip_pkt_len) {
					ret = BCME_BADARG;
					WL_ERR(("ip packet length is 0\n"));
					goto exit;
				}
				ip_pkt = (u8 *)kzalloc(ip_pkt_len, kflags);
				if (ip_pkt == NULL) {
					ret = BCME_NOMEM;
					WL_ERR(("Failed to allocate mem for ip packet\n"));
					goto exit;
				}
				memcpy(ip_pkt, (u8*)nla_data(iter), ip_pkt_len);
				break;
			case MKEEP_ALIVE_ATTRIBUTE_SRC_MAC_ADDR:
				memcpy(src_mac, nla_data(iter), ETHER_ADDR_LEN);
				break;
			case MKEEP_ALIVE_ATTRIBUTE_DST_MAC_ADDR:
				memcpy(dst_mac, nla_data(iter), ETHER_ADDR_LEN);
				break;
			case MKEEP_ALIVE_ATTRIBUTE_PERIOD_MSEC:
				period_msec = nla_get_u32(iter);
				break;
			case MKEEP_ALIVE_ATTRIBUTE_ETHER_TYPE:
				/* R's HAL always sends it: IPv4 or IPv6 */
				ether_type = nla_get_u16(iter);
				break;
			default:
				WL_ERR(("Unknown type: %d\n", type));
				ret = BCME_BADARG;
				goto exit;
		}
	}

	if (ip_pkt == NULL) {
		ret = BCME_BADARG;
		WL_ERR(("ip packet is NULL\n"));
		goto exit;
	}

	ret = dhd_dev_start_mkeep_alive(dhd_pub, mkeep_alive_id, ip_pkt, ip_pkt_len, src_mac,
		dst_mac, period_msec, ether_type);
	if (ret < 0) {
		WL_ERR(("start_mkeep_alive is failed ret: %d\n", ret));
	}

exit:
	if (ip_pkt) {
		kfree(ip_pkt);
	}

	return ret;
}

static int wl_cfgvendor_stop_mkeep_alive(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	int ret = BCME_OK, rem, type;
	u8 mkeep_alive_id = 0;
	const struct nlattr *iter;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pub_t *dhd_pub = cfg->pub;

	nla_for_each_attr(iter, data, len, rem) {
		type = nla_type(iter);
		switch (type) {
			case MKEEP_ALIVE_ATTRIBUTE_ID:
				mkeep_alive_id = nla_get_u8(iter);
				break;
			default:
				WL_ERR(("Unknown type: %d\n", type));
				ret = BCME_BADARG;
				break;
		}
	}

	ret = dhd_dev_stop_mkeep_alive(dhd_pub, mkeep_alive_id);
	if (ret < 0) {
		WL_ERR(("stop_mkeep_alive is failed ret: %d\n", ret));
	}

	return ret;
}
#endif /* defined(KEEP_ALIVE) */

static int wl_cfgvendor_get_feature_set(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	int reply;

	reply = dhd_dev_get_feature_set(bcmcfg_to_prmry_ndev(cfg));

	err =  wl_cfgvendor_send_cmd_reply(wiphy, bcmcfg_to_prmry_ndev(cfg),
	        &reply, sizeof(int));

	if (unlikely(err))
		WL_ERR(("Vendor Command reply failed ret:%d \n", err));

	return err;
}

static int
wl_cfgvendor_set_pno_mac_oui(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int err = 0;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	int type;
	uint8 pno_random_mac_oui[DOT11_OUI_LEN];

	type = nla_type(data);

	if (type == ANDR_WIFI_ATTRIBUTE_RANDOM_MAC_OUI) {
		memcpy(pno_random_mac_oui, nla_data(data), DOT11_OUI_LEN);

		err = dhd_dev_pno_set_mac_oui(bcmcfg_to_prmry_ndev(cfg), pno_random_mac_oui);

		if (unlikely(err))
			WL_ERR(("Bad OUI, could not set:%d \n", err));


	} else {
		err = -1;
	}

	return err;
}

#ifdef LINKSTAT_SUPPORT
/*
 * Link layer statistics from what this firmware has.
 *
 * Samsung's bcmdhd (LineageOS android_kernel_samsung_universal8890,
 * bcmdhd4358) builds these from the radiostat, wme_counters, counters and
 * ratestat iovars. The BCM4354 firmware here carries only counters, so the
 * radio record goes out empty -- no channels, no on-time -- per-AC figures
 * are totals under best effort, and there are no peer or rate records.
 * What the framework does get is real: beacons from the AP, data frames
 * sent and received, losses and retries, and the AP's RSSI.
 */
static int wl_cfgvendor_lstats_get_info(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	struct net_device *ndev = bcmcfg_to_prmry_ndev(cfg);
	int buflen = sizeof(wifi_radio_stat) + sizeof(wifi_iface_stat);
	wifi_radio_stat *radio;
	wifi_iface_stat *iface;
	wl_cnt_t *cnt = NULL;
	uint32 tot_tx, tot_rx, tot_rxmulti, tot_fail, tot_retry;
	char *buf;
	int err, i;

	buf = kzalloc(buflen, GFP_KERNEL);
	cnt = kzalloc(sizeof(*cnt), GFP_KERNEL);
	if (unlikely(!buf || !cnt)) {
		err = -ENOMEM;
		goto exit;
	}
	radio = (wifi_radio_stat *)buf;
	iface = (wifi_iface_stat *)(buf + sizeof(wifi_radio_stat));

	err = wldev_iovar_getbuf(ndev, "counters", NULL, 0,
		(char *)cnt, sizeof(*cnt), NULL);
	if (unlikely(err)) {
		WL_ERR(("%s: counters failed (%d)\n", __FUNCTION__, err));
		goto exit;
	}
	if (dtoh16(cnt->version) > WL_CNT_T_VERSION) {
		WL_ERR(("%s: wl_cnt_t version %u, expected up to %u\n",
			__FUNCTION__, dtoh16(cnt->version), WL_CNT_T_VERSION));
		err = -EINVAL;
		goto exit;
	}

	tot_tx = dtoh32(cnt->txframe);
	tot_rx = dtoh32(cnt->rxframe);
	tot_rxmulti = dtoh32(cnt->rxmulti);
	tot_fail = dtoh32(cnt->txfail);
	tot_retry = dtoh32(cnt->txretry);

	radio->num_channels = 0;

	iface->info.mode = WIFI_INTERFACE_STA;
	memcpy(iface->info.mac_addr, ndev->dev_addr, ETHER_ADDR_LEN);
	if (wl_get_drv_status(cfg, CONNECTED, ndev)) {
		wlc_ssid_t ssid;
		scb_val_t scbval;

		iface->info.state = WIFI_ASSOCIATED;
		memset(&ssid, 0, sizeof(ssid));
		if (!wldev_ioctl(ndev, WLC_GET_SSID, &ssid, sizeof(ssid), false)) {
			u32 ssid_len = MIN(dtoh32(ssid.SSID_len), DOT11_MAX_SSID_LEN);
			memcpy(iface->info.ssid, ssid.SSID, ssid_len);
		}
		wldev_ioctl(ndev, WLC_GET_BSSID, iface->info.bssid, ETHER_ADDR_LEN, false);
		memset(&scbval, 0, sizeof(scbval));
		if (!wldev_get_rssi(ndev, &scbval))
			iface->rssi_mgmt = dtoh32(scbval.val);
	} else {
		iface->info.state = WIFI_DISCONNECTED;
	}

	iface->beacon_rx = dtoh32(cnt->rxbeaconmbss);	/* before cnt is reused */
	for (i = 0; i < WIFI_AC_MAX; i++)
		iface->ac[i].ac = i;

	/*
	 * Per access category from wme_counters, which this firmware has;
	 * Broadcom counts BE, BK, VI, VO and the HAL VO, VI, BE, BK. Without
	 * it, the totals from counters stand under best effort.
	 */
	memset(cnt, 0, sizeof(*cnt));
	if (sizeof(*cnt) >= sizeof(wl_wme_cnt_t) &&
		!wldev_iovar_getbuf(ndev, "wme_counters", NULL, 0,
			(char *)cnt, sizeof(wl_wme_cnt_t), NULL)) {
		static const int hal_ac[AC_COUNT] = {
			[AC_BE] = WIFI_AC_BE, [AC_BK] = WIFI_AC_BK,
			[AC_VI] = WIFI_AC_VI, [AC_VO] = WIFI_AC_VO,
		};
		wl_wme_cnt_t *wme = (wl_wme_cnt_t *)cnt;

		for (i = 0; i < AC_COUNT; i++) {
			wifi_wmm_ac_stat *ac = &iface->ac[hal_ac[i]];

			ac->tx_mpdu = dtoh32(wme->tx[i].packets);
			ac->rx_mpdu = dtoh32(wme->rx[i].packets);
			ac->mpdu_lost = dtoh32(wme->tx_failed[i].packets);
		}
		/* Retries are counted only as a whole. */
		iface->ac[WIFI_AC_BE].retries = tot_retry;
	} else {
		iface->ac[WIFI_AC_BE].tx_mpdu = tot_tx;
		iface->ac[WIFI_AC_BE].rx_mpdu = tot_rx;
		iface->ac[WIFI_AC_BE].rx_mcast = tot_rxmulti;
		iface->ac[WIFI_AC_BE].mpdu_lost = tot_fail;
		iface->ac[WIFI_AC_BE].retries = tot_retry;
	}
	iface->num_peers = 0;

	err = wl_cfgvendor_send_cmd_reply(wiphy, ndev, buf, buflen);
	if (unlikely(err))
		WL_ERR(("%s: vendor command reply failed (%d)\n", __FUNCTION__, err));

exit:
	kfree(cnt);
	kfree(buf);
	return err;
}
#endif /* LINKSTAT_SUPPORT */

#ifdef DBG_PKT_MON
/* After bcmdhd 1.77's wl_cfgvendor.c, onto dhd_pktmon.c. */
static int wl_cfgvendor_dbg_start_pkt_fate_monitoring(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	int ret;

	ret = dhd_pktmon_start((dhd_pub_t *)cfg->pub);
	if (unlikely(ret))
		WL_ERR(("failed to start pkt fate monitoring, ret=%d\n", ret));

	return ret;
}

typedef int (*dbg_mon_get_pkts_t) (dhd_pub_t *dhdp, void __user *user_buf,
	uint16 req_count, uint16 *resp_count);

static int __wl_cfgvendor_dbg_get_pkt_fates(struct wiphy *wiphy,
	const void *data, int len, dbg_mon_get_pkts_t dbg_mon_get_pkts)
{
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	struct sk_buff *skb;
	const struct nlattr *iter;
	void __user *user_buf = NULL;
	uint16 req_count = 0, resp_count = 0;
	int ret, tmp, type;

	nla_for_each_attr(iter, data, len, tmp) {
		type = nla_type(iter);
		switch (type) {
			case DEBUG_ATTRIBUTE_PKT_FATE_NUM:
				req_count = nla_get_u32(iter);
				break;
			case DEBUG_ATTRIBUTE_PKT_FATE_DATA:
				user_buf = (void __user *)(unsigned long) nla_get_u64(iter);
				break;
			default:
				WL_ERR(("%s: no such attribute %d\n", __FUNCTION__, type));
				return -EINVAL;
		}
	}

	if (!req_count || !user_buf) {
		WL_ERR(("%s: invalid request, user_buf=%p, req_count=%u\n",
			__FUNCTION__, user_buf, req_count));
		return -EINVAL;
	}

	ret = dbg_mon_get_pkts((dhd_pub_t *)cfg->pub, user_buf, req_count, &resp_count);
	if (unlikely(ret)) {
		WL_ERR(("failed to get packets, ret:%d\n", ret));
		return ret;
	}

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy,
		VENDOR_REPLY_OVERHEAD + ATTRIBUTE_U32_LEN);
	if (unlikely(!skb)) {
		WL_ERR(("skb alloc failed"));
		return -ENOMEM;
	}
	nla_put_u32(skb, DEBUG_ATTRIBUTE_PKT_FATE_NUM, resp_count);

	ret = cfg80211_vendor_cmd_reply(skb);
	if (unlikely(ret))
		WL_ERR(("vendor Command reply failed ret:%d\n", ret));

	return ret;
}

static int wl_cfgvendor_dbg_get_tx_pkt_fates(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	return __wl_cfgvendor_dbg_get_pkt_fates(wiphy, data, len, dhd_pktmon_get_tx);
}

static int wl_cfgvendor_dbg_get_rx_pkt_fates(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	return __wl_cfgvendor_dbg_get_pkt_fates(wiphy, data, len, dhd_pktmon_get_rx);
}
#endif /* DBG_PKT_MON */

#ifdef DHD_WAKE_STATUS
/*
 * The counts dhd_linux.c keeps. Unlike bcmdhd 1.77 this sends no per-event
 * array: the HAL copies CMD_COUNT_USED entries into a buffer the framework
 * sizes at 32, and that driver announces WLC_E_LAST of them.
 */
static int wl_cfgvendor_get_wake_reason_stats(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_wake_counts_t *wc = &((dhd_pub_t *)cfg->pub)->wake_counts;
	struct sk_buff *skb;

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy,
		VENDOR_REPLY_OVERHEAD + ATTRIBUTE_U32_LEN * 16);
	if (unlikely(!skb))
		return -ENOMEM;

	nla_put_u32(skb, WAKE_STAT_ATTRIBUTE_TOTAL_CMD_EVENT, wc->rcwake);
	nla_put_u32(skb, WAKE_STAT_ATTRIBUTE_CMD_COUNT_USED, 0);
	nla_put_u32(skb, WAKE_STAT_ATTRIBUTE_TOTAL_RX_DATA_WAKE, wc->rxwake);
	nla_put_u32(skb, WAKE_STAT_ATTRIBUTE_RX_UNICAST_COUNT, wc->rx_ucast);
	nla_put_u32(skb, WAKE_STAT_ATTRIBUTE_RX_MULTICAST_COUNT, wc->rx_mcast);
	nla_put_u32(skb, WAKE_STAT_ATTRIBUTE_RX_BROADCAST_COUNT, wc->rx_bcast);
	nla_put_u32(skb, WAKE_STAT_ATTRIBUTE_RX_ICMP_PKT, wc->rx_arp);
	nla_put_u32(skb, WAKE_STAT_ATTRIBUTE_RX_ICMP6_PKT, wc->rx_icmpv6);
	nla_put_u32(skb, WAKE_STAT_ATTRIBUTE_RX_ICMP6_RA, wc->rx_icmpv6_ra);
	nla_put_u32(skb, WAKE_STAT_ATTRIBUTE_RX_ICMP6_NA, wc->rx_icmpv6_na);
	nla_put_u32(skb, WAKE_STAT_ATTRIBUTE_RX_ICMP6_NS, wc->rx_icmpv6_ns);
	nla_put_u32(skb, WAKE_STAT_ATTRIBUTE_IPV4_RX_MULTICAST_ADD_CNT, wc->rx_multi_ipv4);
	nla_put_u32(skb, WAKE_STAT_ATTRIBUTE_IPV6_RX_MULTICAST_ADD_CNT, wc->rx_multi_ipv6);
	nla_put_u32(skb, WAKE_STAT_ATTRIBUTE_OTHER_RX_MULTICAST_ADD_CNT, wc->rx_multi_other);

	return cfg80211_vendor_cmd_reply(skb);
}
#endif /* DHD_WAKE_STATUS */

/*
 * The HAL tells the driver when it starts and stops and which socket takes
 * its events. Newer bcmdhd holds asynchronous debug events back until then;
 * this driver sends none, so the notice is taken and there is nothing else
 * to do with it.
 */
static int wl_cfgvendor_set_hal_state(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	return 0;
}

#if defined(DHD_DEBUG) && defined(BCMSDIO)
/*
 * The firmware memory dump the HAL puts in bug reports: TRIGGER reads the
 * dongle's RAM into the driver and answers with its length; the HAL then
 * asks GET with a buffer of that length for the driver to fill.
 */
static int wl_cfgvendor_dbg_trigger_mem_dump(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pub_t *dhdp = (dhd_pub_t *)cfg->pub;
	struct sk_buff *skb;
	int ret;

	ret = dhd_bus_socram_dump(dhdp);
	if (ret || !dhdp->soc_ram || !dhdp->soc_ram_length) {
		WL_ERR(("%s: firmware memory dump failed (%d)\n", __FUNCTION__, ret));
		return -EIO;
	}

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy,
		VENDOR_REPLY_OVERHEAD + ATTRIBUTE_U32_LEN);
	if (unlikely(!skb))
		return -ENOMEM;
	nla_put_u32(skb, DEBUG_ATTRIBUTE_FW_DUMP_LEN, dhdp->soc_ram_length);

	return cfg80211_vendor_cmd_reply(skb);
}

static int wl_cfgvendor_dbg_get_mem_dump(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	dhd_pub_t *dhdp = (dhd_pub_t *)cfg->pub;
	const struct nlattr *iter;
	void __user *user_buf = NULL;
	uint32 buf_len = 0, copy_len;
	struct sk_buff *skb;
	int tmp;

	nla_for_each_attr(iter, data, len, tmp) {
		switch (nla_type(iter)) {
			case DEBUG_ATTRIBUTE_FW_DUMP_LEN:
				buf_len = nla_get_u32(iter);
				break;
			case DEBUG_ATTRIBUTE_FW_DUMP_DATA:
				user_buf = (void __user *)(unsigned long) nla_get_u64(iter);
				break;
			default:
				WL_ERR(("%s: no such attribute %d\n", __FUNCTION__, nla_type(iter)));
				return -EINVAL;
		}
	}
	if (!buf_len || !user_buf || !dhdp->soc_ram)
		return -EINVAL;

	copy_len = MIN(buf_len, dhdp->soc_ram_length);
	if (copy_to_user(user_buf, dhdp->soc_ram, copy_len))
		return -EFAULT;

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy,
		VENDOR_REPLY_OVERHEAD + ATTRIBUTE_U32_LEN);
	if (unlikely(!skb))
		return -ENOMEM;
	nla_put_u32(skb, DEBUG_ATTRIBUTE_FW_DUMP_DATA, copy_len);

	return cfg80211_vendor_cmd_reply(skb);
}
#endif /* DHD_DEBUG && BCMSDIO */

/* As hardware_legacy/wifi_logger.h numbers them. */
#define WIFI_LOGGER_MEMORY_DUMP_SUPPORTED	(1 << 0)
#define WIFI_LOGGER_PACKET_FATE_SUPPORTED	(1 << 8)

/* What of the HAL's logger this driver answers; no ring buffers. */
static int wl_cfgvendor_dbg_get_feature(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	uint32 features = 0;

#if defined(DHD_DEBUG) && defined(BCMSDIO)
	features |= WIFI_LOGGER_MEMORY_DUMP_SUPPORTED;
#endif
#ifdef DBG_PKT_MON
	features |= WIFI_LOGGER_PACKET_FATE_SUPPORTED;
#endif
	return wl_cfgvendor_send_cmd_reply(wiphy, wdev->netdev, &features, sizeof(features));
}

/*
 * There are no ring buffers: the firmware carries no event_log to fill
 * them. Saying there are none, rather than refusing the question, leaves
 * the framework nothing to start logging on.
 */
static int wl_cfgvendor_dbg_get_ring_status(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	struct sk_buff *skb;

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy,
		VENDOR_REPLY_OVERHEAD + ATTRIBUTE_U32_LEN);
	if (unlikely(!skb))
		return -ENOMEM;
	nla_put_u32(skb, DEBUG_ATTRIBUTE_RING_NUM, 0);

	return cfg80211_vendor_cmd_reply(skb);
}

/*
 * RSSI monitoring. The firmware has no rssi_monitor, the iovar newer bcmdhd
 * drives this with, but it has the older rssi_event: given a set of levels it
 * posts WLC_E_RSSI each time the RSSI of the AP's frames crosses one. The two
 * thresholds the framework asks about become the two levels; a crossing that
 * leaves the RSSI outside [min, max] is passed to the HAL as its
 * RSSI_MONITOR_EVENT, and the framework then asks for new thresholds.
 */
static struct {
	bool on;
	int8 min_rssi;
	int8 max_rssi;
} rssi_mon;

static int wl_cfgvendor_set_rssi_monitor(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	struct net_device *ndev = bcmcfg_to_prmry_ndev(cfg);
	const struct nlattr *iter;
	wl_rssi_event_t rev;
	int8 max_rssi = 0, min_rssi = 0;
	u32 start = 0;
	int err, tmp;

	nla_for_each_attr(iter, data, len, tmp) {
		switch (nla_type(iter)) {
			case RSSI_MONITOR_ATTRIBUTE_MAX_RSSI:
				max_rssi = (int8)nla_get_u32(iter);
				break;
			case RSSI_MONITOR_ATTRIBUTE_MIN_RSSI:
				min_rssi = (int8)nla_get_u32(iter);
				break;
			case RSSI_MONITOR_ATTRIBUTE_START:
				start = nla_get_u32(iter);
				break;
		}
	}
	if (start && min_rssi > max_rssi)
		return -EINVAL;

	memset(&rev, 0, sizeof(rev));
	if (start) {
		rev.num_rssi_levels = 2;
		rev.rssi_levels[0] = min_rssi;
		rev.rssi_levels[1] = max_rssi;
	}
	err = wldev_iovar_setbuf(ndev, "rssi_event", &rev, sizeof(rev),
		cfg->ioctl_buf, WLC_IOCTL_SMLEN, &cfg->ioctl_buf_sync);
	if (unlikely(err)) {
		WL_ERR(("%s: rssi_event failed (%d)\n", __FUNCTION__, err));
		return err;
	}

	rssi_mon.min_rssi = min_rssi;
	rssi_mon.max_rssi = max_rssi;
	rssi_mon.on = !!start;
	return 0;
}

void wl_cfgvendor_rssi_event(struct bcm_cfg80211 *cfg, struct net_device *ndev, int32 rssi)
{
	struct {
		uint8 version;
		int8 cur_rssi;
		uint8 bssid[ETHER_ADDR_LEN];
	} __attribute__ ((packed)) evt;

	if (!rssi_mon.on || (rssi >= rssi_mon.min_rssi && rssi <= rssi_mon.max_rssi))
		return;

	memset(&evt, 0, sizeof(evt));
	evt.version = 1;	/* RSSI_MONITOR_EVT_VERSION */
	evt.cur_rssi = (int8)rssi;
	wldev_ioctl(ndev, WLC_GET_BSSID, evt.bssid, ETHER_ADDR_LEN, false);

	wl_cfgvendor_send_async_event(bcmcfg_to_wiphy(cfg), ndev,
		GOOGLE_RSSI_MONITOR_EVENT, &evt, sizeof(evt));
}

static int wl_cfgvendor_dbg_get_version(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void *data, int len)
{
	int ret = BCME_OK, rem, type;
	int buf_len = 1024;
	bool dhd_ver = FALSE;
	char *buf_ptr;
	const struct nlattr *iter;
	gfp_t kflags;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	kflags = in_atomic() ? GFP_ATOMIC : GFP_KERNEL;
	buf_ptr = kzalloc(buf_len, kflags);
	if (!buf_ptr) {
		WL_ERR(("failed to allocate the buffer for version n"));
		ret = BCME_NOMEM;
		goto exit;
	}
	nla_for_each_attr(iter, data, len, rem) {
		type = nla_type(iter);
		switch (type) {
			case DEBUG_ATTRIBUTE_GET_DRIVER:
				dhd_ver = TRUE;
				break;
			case DEBUG_ATTRIBUTE_GET_FW:
				dhd_ver = FALSE;
				break;
			default:
				WL_ERR(("Unknown type: %d\n", type));
				ret = BCME_ERROR;
				goto exit;
		}
	}
	ret = dhd_os_get_version(bcmcfg_to_prmry_ndev(cfg), dhd_ver, &buf_ptr, buf_len);
	if (ret < 0) {
		WL_ERR(("failed to get the version %d\n", ret));
		goto exit;
	}
	ret = wl_cfgvendor_send_cmd_reply(wiphy, bcmcfg_to_prmry_ndev(cfg),
	        buf_ptr, strlen(buf_ptr));
exit:
	kfree(buf_ptr);
	return ret;
}

static const struct wiphy_vendor_command wl_vendor_cmds [] = {
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = ANDR_WIFI_SET_COUNTRY
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_country
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = ANDR_WIFI_SUBCMD_GET_FEATURE_SET
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_get_feature_set
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = ANDR_WIFI_RANDOM_MAC_OUI
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_pno_mac_oui
	},
#ifdef LINKSTAT_SUPPORT
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = LSTATS_SUBCMD_GET_INFO
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_lstats_get_info
	},
#endif /* LINKSTAT_SUPPORT */
#ifdef DBG_PKT_MON
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_START_PKT_FATE_MONITORING
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_start_pkt_fate_monitoring
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_TX_PKT_FATES
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_get_tx_pkt_fates
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_RX_PKT_FATES
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_get_rx_pkt_fates
	},
#endif /* DBG_PKT_MON */
#ifdef DHD_WAKE_STATUS
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_WAKE_REASON_STATS
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_get_wake_reason_stats
	},
#endif /* DHD_WAKE_STATUS */
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_SET_HAL_START
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_hal_state
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_SET_HAL_STOP
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_hal_state
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_SET_HAL_PID
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_hal_state
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = WIFI_SUBCMD_SET_RSSI_MONITOR
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_set_rssi_monitor
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_VER
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_get_version
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_START_LOGGING
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_unsupported_feature
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_TRIGGER_MEM_DUMP
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
#if defined(DHD_DEBUG) && defined(BCMSDIO)
		.doit = wl_cfgvendor_dbg_trigger_mem_dump
#else
		.doit = wl_cfgvendor_unsupported_feature
#endif
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_MEM_DUMP
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
#if defined(DHD_DEBUG) && defined(BCMSDIO)
		.doit = wl_cfgvendor_dbg_get_mem_dump
#else
		.doit = wl_cfgvendor_unsupported_feature
#endif
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_RING_STATUS
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_get_ring_status
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_RING_DATA
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_unsupported_feature
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_GET_FEATURE
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_dbg_get_feature
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = DEBUG_RESET_LOGGING
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_unsupported_feature
	},
#ifdef GSCAN_SUPPORT
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = GSCAN_SUBCMD_GET_CHANNEL_LIST
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_gscan_get_channel_list
	},
#endif /* GSCAN_SUPPORT */
#ifdef KEEP_ALIVE
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = WIFI_OFFLOAD_SUBCMD_START_MKEEP_ALIVE
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_start_mkeep_alive
	},
	{
		{
			.vendor_id = OUI_GOOGLE,
			.subcmd = WIFI_OFFLOAD_SUBCMD_STOP_MKEEP_ALIVE
		},
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = wl_cfgvendor_stop_mkeep_alive
	},
#endif /* KEEP_ALIVE */
};

static const struct  nl80211_vendor_cmd_info wl_vendor_events [] = {
		{ OUI_BRCM, BRCM_VENDOR_EVENT_UNSPEC },
		{ OUI_BRCM, BRCM_VENDOR_EVENT_PRIV_STR },
		{ OUI_GOOGLE, GOOGLE_GSCAN_SIGNIFICANT_EVENT },
		{ OUI_GOOGLE, GOOGLE_GSCAN_GEOFENCE_FOUND_EVENT },
		{ OUI_GOOGLE, GOOGLE_GSCAN_BATCH_SCAN_EVENT },
		{ OUI_GOOGLE, GOOGLE_SCAN_FULL_RESULTS_EVENT },
		{ OUI_GOOGLE, GOOGLE_RTT_COMPLETE_EVENT },
		{ OUI_GOOGLE, GOOGLE_SCAN_COMPLETE_EVENT },
		{ OUI_GOOGLE, GOOGLE_GSCAN_GEOFENCE_LOST_EVENT },
		{ OUI_GOOGLE, GOOGLE_SCAN_EPNO_EVENT },
		{ OUI_GOOGLE, GOOGLE_DEBUG_RING_EVENT },
		{ OUI_GOOGLE, GOOGLE_FW_DUMP_EVENT },
		{ OUI_GOOGLE, GOOGLE_PNO_HOTSPOT_FOUND_EVENT },
		{ OUI_GOOGLE, GOOGLE_RSSI_MONITOR_EVENT },
		{ OUI_GOOGLE, GOOGLE_MKEEP_ALIVE_EVENT },
		{ OUI_BRCM, BRCM_VENDOR_EVENT_IDSUP_STATUS },
		{ OUI_BRCM, BRCM_VENDOR_EVENT_DRIVER_HANG }
};

int wl_cfgvendor_attach(struct wiphy *wiphy, dhd_pub_t *dhd)
{

	WL_INFORM(("Vendor: Register BRCM cfg80211 vendor cmd(0x%x) interface \n",
		NL80211_CMD_VENDOR));

	wiphy->vendor_commands	= wl_vendor_cmds;
	wiphy->n_vendor_commands = ARRAY_SIZE(wl_vendor_cmds);
	wiphy->vendor_events	= wl_vendor_events;
	wiphy->n_vendor_events	= ARRAY_SIZE(wl_vendor_events);

	return 0;
}

int wl_cfgvendor_detach(struct wiphy *wiphy)
{
	WL_INFORM(("Vendor: Unregister BRCM cfg80211 vendor interface \n"));

	wiphy->vendor_commands  = NULL;
	wiphy->vendor_events    = NULL;
	wiphy->n_vendor_commands = 0;
	wiphy->n_vendor_events  = 0;

	return 0;
}
#endif /* (LINUX_VERSION_CODE > KERNEL_VERSION(3, 13, 0)) || defined(WL_VENDOR_EXT_SUPPORT) */
