/*
 * Packet fate monitoring.
 *
 * The wifi HAL starts this on every connection and reads it back for bug
 * reports and failed connections: the first MAX_FATE_LOG_LEN frames sent and
 * received since the start, each with what became of it. The semantics are
 * those of the packet monitor in Broadcom's bcmdhd 1.77 (as in LineageOS
 * android_kernel_samsung_universal7580, bcmdhd_1_77/dhd_debug.c): frames are
 * logged until the log is full, a sent frame is matched to its status by the
 * packet it travelled in, and the firmware's proptxstatus flags are mapped to
 * fates the same way. That driver only follows transmit fates on PCIe; here
 * they come from the SDIO paths -- the proptxstatus report when the firmware
 * gives one, the bus completion otherwise.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#ifdef DBG_PKT_MON

#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/time.h>
#include <linux/hrtimer.h>
#include <linux/uaccess.h>

#include <typedefs.h>
#include <osl.h>
#include <bcmutils.h>
#include <dngl_stats.h>
#include <dhd.h>
#include <wlfc_proto.h>
#include <dhd_pktmon.h>

/* As hardware_legacy/wifi_logger.h has them. */
#define PKTMON_LOG_LEN		32	/* MAX_FATE_LOG_LEN */
#define PKTMON_FRAME_ETHERNET	1518	/* MAX_FRAME_LEN_ETHERNET */
#define PKTMON_FRAME_80211_MGMT	2352	/* MAX_FRAME_LEN_80211_MGMT */

#define FRAME_TYPE_ETHERNET_II	1

enum {
	TX_PKT_FATE_ACKED,
	TX_PKT_FATE_SENT,
	TX_PKT_FATE_FW_QUEUED,
	TX_PKT_FATE_FW_DROP_INVALID,
	TX_PKT_FATE_FW_DROP_NOBUFS,
	TX_PKT_FATE_FW_DROP_OTHER,
	TX_PKT_FATE_DRV_QUEUED,
	TX_PKT_FATE_DRV_DROP_INVALID,
	TX_PKT_FATE_DRV_DROP_NOBUFS,
	TX_PKT_FATE_DRV_DROP_OTHER
};

enum {
	RX_PKT_FATE_SUCCESS
};

/*
 * wifi_tx_report and wifi_rx_report, which share a layout, as a 32-bit
 * userspace sees them: size_t frame_len is four bytes there, as it is here.
 */
typedef struct {
	char md5_prefix[4];
	int32 fate;
	struct {
		int32 payload_type;
		uint32 frame_len;
		uint32 driver_timestamp_usec;
		uint32 firmware_timestamp_usec;
		char frame_content[PKTMON_FRAME_80211_MGMT];
	} frame_inf;
} pktmon_report_t;

typedef struct {
	void *pkt;		/* identifies a sent frame until its status */
	int32 fate;
	uint32 ts_usec;
	uint16 len;
	uint8 *data;
} pktmon_entry_t;

typedef struct {
	spinlock_t lock;
	bool running;
	uint16 ntx;
	uint16 nrx;
	pktmon_entry_t tx[PKTMON_LOG_LEN];
	pktmon_entry_t rx[PKTMON_LOG_LEN];
} dhd_pktmon_t;

static uint32
pktmon_now_usec(void)
{
	struct timespec ts;

	get_monotonic_boottime(&ts);
	return (uint32)ts.tv_sec * USEC_PER_SEC + ts.tv_nsec / NSEC_PER_USEC;
}

static void
pktmon_clear(dhd_pktmon_t *pm)
{
	int i;

	for (i = 0; i < pm->ntx; i++)
		kfree(pm->tx[i].data);
	for (i = 0; i < pm->nrx; i++)
		kfree(pm->rx[i].data);
	memset(pm->tx, 0, sizeof(pm->tx));
	memset(pm->rx, 0, sizeof(pm->rx));
	pm->ntx = 0;
	pm->nrx = 0;
}

/* Called with the lock held. */
static void
pktmon_log(dhd_pub_t *dhdp, pktmon_entry_t *e, void *pkt, int32 fate)
{
	uint len = MIN(PKTLEN(dhdp->osh, pkt), PKTMON_FRAME_ETHERNET);

	e->data = kmalloc(len, GFP_ATOMIC);
	if (e->data) {
		memcpy(e->data, PKTDATA(dhdp->osh, pkt), len);
		e->len = len;
	}
	e->pkt = pkt;
	e->fate = fate;
	e->ts_usec = pktmon_now_usec();
}

int
dhd_pktmon_start(dhd_pub_t *dhdp)
{
	dhd_pktmon_t *pm = dhdp->pktmon;
	unsigned long flags;

	if (!pm) {
		pm = kzalloc(sizeof(*pm), GFP_KERNEL);
		if (!pm)
			return -ENOMEM;
		spin_lock_init(&pm->lock);
		dhdp->pktmon = pm;
	}

	spin_lock_irqsave(&pm->lock, flags);
	pktmon_clear(pm);
	pm->running = TRUE;
	spin_unlock_irqrestore(&pm->lock, flags);

	return 0;
}

void
dhd_pktmon_free(dhd_pub_t *dhdp)
{
	dhd_pktmon_t *pm = dhdp->pktmon;

	if (!pm)
		return;
	dhdp->pktmon = NULL;
	pktmon_clear(pm);
	kfree(pm);
}

void
dhd_pktmon_tx(dhd_pub_t *dhdp, void *pkt)
{
	dhd_pktmon_t *pm = dhdp->pktmon;
	unsigned long flags;

	if (!pm || !pkt)
		return;

	spin_lock_irqsave(&pm->lock, flags);
	if (pm->running && pm->ntx < PKTMON_LOG_LEN)
		pktmon_log(dhdp, &pm->tx[pm->ntx++], pkt, TX_PKT_FATE_DRV_QUEUED);
	spin_unlock_irqrestore(&pm->lock, flags);
}

/* The latest logged frame still waiting for word of pkt, or NULL. */
static pktmon_entry_t *
pktmon_find_tx(dhd_pktmon_t *pm, void *pkt)
{
	int i;

	for (i = pm->ntx - 1; i >= 0; i--) {
		pktmon_entry_t *e = &pm->tx[i];

		if (e->pkt == pkt &&
			(e->fate == TX_PKT_FATE_DRV_QUEUED || e->fate == TX_PKT_FATE_FW_QUEUED))
			return e;
	}
	return NULL;
}

void
dhd_pktmon_tx_status(dhd_pub_t *dhdp, void *pkt, uint8 wlfc_status)
{
	dhd_pktmon_t *pm = dhdp->pktmon;
	pktmon_entry_t *e;
	unsigned long flags;

	if (!pm || !pkt)
		return;

	spin_lock_irqsave(&pm->lock, flags);
	e = pktmon_find_tx(pm, pkt);
	if (e) {
		switch (wlfc_status) {
		case WLFC_CTL_PKTFLAG_DISCARD:
			e->fate = TX_PKT_FATE_ACKED;
			break;
		case WLFC_CTL_PKTFLAG_D11SUPPRESS:
		case WLFC_CTL_PKTFLAG_WLSUPPRESS:
			/* Suppressed frames are queued again and reported later. */
			e->fate = TX_PKT_FATE_FW_QUEUED;
			break;
		case WLFC_CTL_PKTFLAG_TOSSED_BYWLC:
			e->fate = TX_PKT_FATE_FW_DROP_INVALID;
			break;
		case WLFC_CTL_PKTFLAG_DISCARD_NOACK:
			e->fate = TX_PKT_FATE_SENT;
			break;
		default:
			e->fate = TX_PKT_FATE_FW_DROP_OTHER;
			break;
		}
	}
	spin_unlock_irqrestore(&pm->lock, flags);
}

/*
 * The bus is done with a frame. Without proptxstatus this is the only word
 * there will be: handed to the firmware, or dropped on the way. With it, a
 * status has usually settled the fate already, and this leaves it alone.
 */
void
dhd_pktmon_tx_done(dhd_pub_t *dhdp, void *pkt, bool success)
{
	dhd_pktmon_t *pm = dhdp->pktmon;
	pktmon_entry_t *e;
	unsigned long flags;

	if (!pm || !pkt)
		return;

	spin_lock_irqsave(&pm->lock, flags);
	e = pktmon_find_tx(pm, pkt);
	if (e && e->fate == TX_PKT_FATE_DRV_QUEUED)
		e->fate = success ? TX_PKT_FATE_FW_QUEUED : TX_PKT_FATE_DRV_DROP_OTHER;
	spin_unlock_irqrestore(&pm->lock, flags);
}

void
dhd_pktmon_rx(dhd_pub_t *dhdp, void *pkt)
{
	dhd_pktmon_t *pm = dhdp->pktmon;
	unsigned long flags;

	if (!pm || !pkt)
		return;

	spin_lock_irqsave(&pm->lock, flags);
	if (pm->running && pm->nrx < PKTMON_LOG_LEN)
		pktmon_log(dhdp, &pm->rx[pm->nrx++], pkt, RX_PKT_FATE_SUCCESS);
	spin_unlock_irqrestore(&pm->lock, flags);
}

static int
pktmon_get(dhd_pub_t *dhdp, bool tx, void __user *buf, uint16 req, uint16 *resp)
{
	dhd_pktmon_t *pm = dhdp->pktmon;
	pktmon_report_t *report;
	unsigned long flags;
	uint16 i, count;
	int err = 0;

	*resp = 0;
	if (!pm)
		return 0;

	report = kmalloc(sizeof(*report), GFP_KERNEL);
	if (!report)
		return -ENOMEM;

	for (i = 0; i < req; i++) {
		pktmon_entry_t *e;

		memset(report, 0, sizeof(*report));
		spin_lock_irqsave(&pm->lock, flags);
		count = tx ? pm->ntx : pm->nrx;
		if (i >= count) {
			spin_unlock_irqrestore(&pm->lock, flags);
			break;
		}
		e = tx ? &pm->tx[i] : &pm->rx[i];
		report->fate = e->fate;
		report->frame_inf.payload_type = FRAME_TYPE_ETHERNET_II;
		report->frame_inf.frame_len = e->len;
		report->frame_inf.driver_timestamp_usec = e->ts_usec;
		if (e->data)
			memcpy(report->frame_inf.frame_content, e->data, e->len);
		spin_unlock_irqrestore(&pm->lock, flags);

		if (copy_to_user((char __user *)buf + i * sizeof(*report),
			report, sizeof(*report))) {
			err = -EFAULT;
			break;
		}
		(*resp)++;
	}

	kfree(report);
	return err;
}

int
dhd_pktmon_get_tx(dhd_pub_t *dhdp, void __user *buf, uint16 req, uint16 *resp)
{
	return pktmon_get(dhdp, TRUE, buf, req, resp);
}

int
dhd_pktmon_get_rx(dhd_pub_t *dhdp, void __user *buf, uint16 req, uint16 *resp)
{
	return pktmon_get(dhdp, FALSE, buf, req, resp);
}

#endif /* DBG_PKT_MON */
