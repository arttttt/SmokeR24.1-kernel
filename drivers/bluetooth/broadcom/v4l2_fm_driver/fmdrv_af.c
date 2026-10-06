/*
 * Copyright (C) 2026 Artem Bambalov
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * RDS alternative frequency (AF) switching for the FM receiver: the
 * policy. See fmdrv_af.h.
 *
 * Every FM_AF_POLL_MS, while the station has given its PI and an AF list,
 * the signal is read; when FM_AF_WEAK_OF_10 of the last ten readings are
 * weaker than the weak level (FM_AF_WEAK_DBM by default), the AFs are
 * looked at, then perhaps jumped to, the way receivers with one tuner do
 * it (Silicon Labs AN243):
 *
 *  - a look at each AF, up to FM_AF_LOOKS of them: a tune there, the
 *    signal read, a tune back, the audio muted -- about 0.11 s off air;
 *  - if the best of them is FM_AF_BETTER_DB stronger than the station
 *    here, now, the chip's own jump to it, which checks the PI there and
 *    the signal again and comes back by itself if either fails -- about
 *    0.15 s, muted.
 *
 * Every look and the jump waits for a pause in the audio, so as to be
 * hidden in it: with no pause within FM_AF_PAUSE_MS, nothing is done and
 * it is tried again in FM_AF_RETRY_MS. The levels are those of the moment:
 * nothing is kept from one round to the next, a receiver on the move
 * seeing them change. Should a jump still land FM_AF_BACK_DB weaker (the
 * signal moves), it goes back.
 *
 * After a jump the switching rests for FM_AF_REST_MS; after a round with
 * nothing better, for FM_AF_NONE_MS, so a station weak everywhere is not
 * looked about all the time. An AF the chip refuses (another station
 * there, most likely) is not looked at for FM_AF_FAILED_MS.
 *
 * Broadcom's own stack (bta_fm in the stock bluetooth.default.so) decides
 * when the same way -- a reading every 3 s, 5 weak ones -- but jumps
 * blind, unmuted: the chip then fades the audio out and back in, and is
 * away most of a second. The chip's RSSI-low interrupt is not used.
 *
 * The tuner is the user's first: the work never blocks on the tuner's
 * lock (it tries, and comes back later), and lets go of it while it waits
 * for a pause; a tune or seek meanwhile (tune_gen) ends the round. The
 * work runs on its own ordered, power efficient queue, its timer
 * deferrable: the polling wakes no idle CPU.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include "fmdrv_af.h"

#define FM_AF_POLL_MS       3000
#define FM_AF_WEAK_DBM      80      /* weaker than -80 dBm, by default */
#define FM_AF_WEAK_OF_10    5
#define FM_AF_LOOKS         8       /* AFs looked at in a round */
#define FM_AF_RETRY_MS      10000   /* no pause came: again in */
#define FM_AF_REST_MS       60000   /* after a jump */
#define FM_AF_NONE_MS       30000   /* after a round with nothing better */
#define FM_AF_PAUSE_MS      5000    /* longest wait for a pause (stock 3 s) */
#define FM_AF_BETTER_DB     3       /* the AF must be this much stronger */
#define FM_AF_BACK_DB       3       /* this much weaker after a jump: back */
#define FM_AF_FAILED_MS     120000  /* a refused AF rests this long */

static unsigned int af_10khz(struct fm_af *af, u16 freq)
{
    return af->ops->freq_10khz(freq);
}

/* every AF may be looked at: "now", not 0, which jiffies' start puts ahead */
static void fm_af_clear_failed(struct fm_af *af)
{
    int i;

    for (i = 0; i < FM_AF_LIST_MAX; i++)
        af->failed_until[i] = jiffies;
}

static void fm_af_requeue(struct fm_af *af, unsigned int ms)
{
    if (!af->stop)
        queue_delayed_work(af->wq, &af->work, msecs_to_jiffies(ms));
}

/*
 * Wait for a pause in the audio, the tuner let go meanwhile. Returns with
 * it held again: 0 paused, -ETIME none came, -ECANCELED the user tuned or
 * FM is going, or the driver's error; -ESHUTDOWN, stopping, with it not
 * held.
 */
static int fm_af_wait_pause(struct fm_af *af, unsigned int gen)
{
    bool paused = false;
    int ret;

    ret = af->ops->pause_arm(af->ctx);
    af->ops->unlock(af->ctx);
    if (ret == 0)
        paused = wait_for_completion_timeout(&af->pause,
                                msecs_to_jiffies(FM_AF_PAUSE_MS)) != 0;
    while (!af->ops->trylock(af->ctx)) {
        if (af->stop)
            return -ESHUTDOWN;
        msleep(20);
    }
    af->wait_pause = false;
    if (af->stop || !af->enabled || af->tune_gen != gen ||
        !af->ops->receiving(af->ctx))
        return -ECANCELED;
    if (ret < 0)
        return ret;
    return paused ? 0 : -ETIME;
}

/* One jump, the tuner held throughout; for jumping back, and by hand */
static int fm_af_jump_locked(struct fm_af *af, u16 pi, u16 freq,
                             unsigned int pause_ms, u8 *reason, bool *paused)
{
    int ret;

    *reason = 0;
    *paused = false;
    ret = af->ops->jump_prepare(af->ctx, pi, 0, freq);
    if (ret == 0 && pause_ms) {
        ret = af->ops->pause_arm(af->ctx);
        if (ret == 0)
            *paused = wait_for_completion_timeout(&af->pause,
                                    msecs_to_jiffies(pause_ms)) != 0;
        af->wait_pause = false;
    }
    if (ret == 0)
        ret = af->ops->jump_go(af->ctx, reason);
    af->ops->jump_restore(af->ctx);
    return ret;
}

static void fm_af_work(struct work_struct *w)
{
    struct fm_af *af = container_of(to_delayed_work(w), struct fm_af, work);
    u16 list[FM_AF_LIST_MAX], pi, orig, now, best = 0;
    u8 rssi, rssi_here, rssi_af, best_rssi = 0, reason = 0, back_reason;
    char seen[FM_AF_LOOKS * 13 + 1] = "";
    unsigned long flags;
    unsigned int gen;
    int n, i, idx, first, best_idx = 0, looks = 0, len = 0, ret;
    bool paused;

    if (af->stop)
        return;
    if (!af->ops->trylock(af->ctx)) {
        fm_af_requeue(af, 200);
        return;
    }

    if (!af->enabled || !af->ops->receiving(af->ctx))
        goto idle;

    spin_lock_irqsave(&af->lock, flags);
    pi = af->pi;
    n = af->count;
    memcpy(list, af->list, sizeof(list));
    spin_unlock_irqrestore(&af->lock, flags);
    if (!pi || !n)
        goto idle;

    if (af->ops->read_signal(af->ctx, &rssi, &orig) < 0)
        goto out;
    af->hist = ((af->hist << 1) | (rssi >= af->weak_dbm)) & 0x3ff;
    if (hweight16(af->hist) < FM_AF_WEAK_OF_10 ||
        time_before(jiffies, af->quiet_until))
        goto out;
    af->hist = 0;
    gen = af->tune_gen;

    /* a look at each AF other than where the tuner is and than one
     * refused lately, each in its own pause; round robin when there are
     * more than a round looks at */
    first = af->next % n;
    for (i = 0; i < n && looks < FM_AF_LOOKS; i++) {
        idx = (first + i) % n;
        if (list[idx] == orig ||
            time_before(jiffies, af->failed_until[idx]))
            continue;

        ret = fm_af_wait_pause(af, gen);
        if (ret == -ESHUTDOWN)
            return;
        if (ret == -ECANCELED)
            goto restore;
        if (ret < 0) {
            /* no pause to hide it in: not now */
            af->quiet_until = jiffies + msecs_to_jiffies(FM_AF_RETRY_MS);
            goto restore;
        }

        looks++;
        af->next = (idx + 1) % n;
        ret = af->ops->probe(af->ctx, list[idx], &rssi_af);
        if (ret == -ENOLINK) {
            /* the tuner did not come back: the round is over */
            af->quiet_until = jiffies + msecs_to_jiffies(FM_AF_RETRY_MS);
            goto restore;
        }
        if (ret < 0) {
            len += scnprintf(seen + len, sizeof(seen) - len, " %u e%d",
                             af_10khz(af, list[idx]), ret);
            continue;
        }
        len += scnprintf(seen + len, sizeof(seen) - len, " %u -%u",
                         af_10khz(af, list[idx]), rssi_af);
        if (!best || rssi_af < best_rssi) {
            best = list[idx];
            best_rssi = rssi_af;
            best_idx = idx;
        }
    }
    if (!looks) {
        /* nowhere to go */
        af->quiet_until = jiffies + msecs_to_jiffies(FM_AF_REST_MS);
        goto out;
    }

    /* the signal here now, against the best there */
    if (af->ops->read_signal(af->ctx, &rssi_here, &now) < 0)
        goto restore;
    if (!best || best_rssi + FM_AF_BETTER_DB > rssi_here) {
        pr_debug("fm_af: %u -%u dBm, AFs%s: none better\n",
                af_10khz(af, orig), rssi_here, seen);
        af->quiet_until = jiffies + msecs_to_jiffies(FM_AF_NONE_MS);
        goto restore;
    }

    /* the jump, set up before the pause; the chip measures again there */
    ret = af->ops->jump_prepare(af->ctx, pi,
            rssi_here > FM_AF_BETTER_DB ? rssi_here - FM_AF_BETTER_DB : 1,
            best);
    if (ret == 0) {
        ret = fm_af_wait_pause(af, gen);
        if (ret == -ESHUTDOWN)
            return;
        if (ret == -ECANCELED)
            goto restore;
        if (ret == -ETIME) {
            af->quiet_until = jiffies + msecs_to_jiffies(FM_AF_RETRY_MS);
            goto restore;
        }
    }
    if (ret == 0)
        ret = af->ops->jump_go(af->ctx, &reason);
    af->ops->jump_restore(af->ctx);
    if (ret == -EAGAIN)
        af->failed_until[best_idx] =
            jiffies + msecs_to_jiffies(FM_AF_FAILED_MS);
    if (af->ops->read_signal(af->ctx, &rssi, &now) < 0)
        rssi = rssi_here;   /* not known: no going back on it */

    if (ret == 0 && rssi >= rssi_here + FM_AF_BACK_DB) {
        /* landed on a weaker signal than it left: back */
        u8 rssi_landed = rssi;

        fm_af_jump_locked(af, pi, orig, 0, &back_reason, &paused);
        af->ops->read_signal(af->ctx, &rssi, &now);
        pr_debug("fm_af: %u -%u dBm, AFs%s: -> %u, landed -%u dBm, back: "
                "now %u\n", af_10khz(af, orig), rssi_here, seen,
                af_10khz(af, best), rssi_landed, af_10khz(af, now));
        af->quiet_until = jiffies + msecs_to_jiffies(FM_AF_REST_MS);
    } else {
        pr_debug("fm_af: %u -%u dBm, AFs%s: -> %u: %d (reason 0x%02x), "
                "now %u -%u dBm\n", af_10khz(af, orig), rssi_here, seen,
                af_10khz(af, best), ret, ret ? reason : 0,
                af_10khz(af, now), rssi);
        af->quiet_until = jiffies +
            msecs_to_jiffies(ret == 0 ? FM_AF_REST_MS : FM_AF_RETRY_MS);
    }
    goto out;

restore:
    af->ops->jump_restore(af->ctx);
    goto out;
idle:
    af->hist = 0;
out:
    af->ops->unlock(af->ctx);
    fm_af_requeue(af, FM_AF_POLL_MS);
}

int fm_af_init(struct fm_af *af, const struct fm_af_ops *ops, void *ctx,
               const char *name)
{
    memset(af, 0, sizeof(*af));
    af->ops = ops;
    af->ctx = ctx;
    af->enabled = true;
    af->weak_dbm = FM_AF_WEAK_DBM;
    af->stop = true;
    spin_lock_init(&af->lock);
    init_completion(&af->pause);
    INIT_DEFERRABLE_WORK(&af->work, fm_af_work);
    af->wq = alloc_ordered_workqueue("%s", WQ_POWER_EFFICIENT, name);
    return af->wq ? 0 : -ENOMEM;
}

void fm_af_destroy(struct fm_af *af)
{
    if (af->wq)
        destroy_workqueue(af->wq);
    af->wq = NULL;
}

void fm_af_start(struct fm_af *af)
{
    af->stop = false;
    af->hist = 0;
    af->next = 0;
    af->quiet_until = jiffies;
    fm_af_clear_failed(af);
    queue_delayed_work(af->wq, &af->work, msecs_to_jiffies(FM_AF_POLL_MS));
}

/* The tuner's lock may be held: the work never waits for it, so it ends */
void fm_af_stop(struct fm_af *af)
{
    af->stop = true;
    complete(&af->pause);
    cancel_delayed_work_sync(&af->work);
    af->wait_pause = false;
}

void fm_af_set_enabled(struct fm_af *af, bool enabled)
{
    af->enabled = enabled;
    af->hist = 0;
}

void fm_af_set_weak(struct fm_af *af, u8 weak_dbm)
{
    af->weak_dbm = weak_dbm;
    af->hist = 0;
}

void fm_af_preempt(struct fm_af *af)
{
    af->tune_gen++;
}

void fm_af_tuned(struct fm_af *af)
{
    unsigned long flags;

    af->tune_gen++;
    spin_lock_irqsave(&af->lock, flags);
    af->pi = 0;
    af->count = 0;
    spin_unlock_irqrestore(&af->lock, flags);
    af->next = 0;
    af->hist = 0;
    fm_af_clear_failed(af);
}

void fm_af_rds_pi(struct fm_af *af, u16 pi)
{
    af->pi = pi;
}

/* Called with af->lock taken by the caller's RDS path or not: takes it */
void fm_af_rds_add(struct fm_af *af, u16 freq)
{
    unsigned long flags;
    int i;

    spin_lock_irqsave(&af->lock, flags);
    for (i = 0; i < af->count; i++)
        if (af->list[i] == freq)
            goto out;
    if (af->count < FM_AF_LIST_MAX)
        af->list[af->count++] = freq;
out:
    spin_unlock_irqrestore(&af->lock, flags);
}

void fm_af_arm_pause(struct fm_af *af)
{
    reinit_completion(&af->pause);
    af->wait_pause = true;
}

bool fm_af_audio_paused(struct fm_af *af)
{
    if (!af->wait_pause)
        return false;
    af->wait_pause = false;
    complete(&af->pause);
    return true;
}

u16 fm_af_snapshot(struct fm_af *af, u16 *list, u8 *count)
{
    unsigned long flags;
    u16 pi;

    spin_lock_irqsave(&af->lock, flags);
    pi = af->pi;
    *count = af->count;
    memcpy(list, af->list, sizeof(af->list));
    spin_unlock_irqrestore(&af->lock, flags);
    return pi;
}

int fm_af_jump(struct fm_af *af, u16 freq, unsigned int pause_ms, u8 *reason)
{
    unsigned long start = jiffies;
    bool paused;
    u16 pi, now;
    u8 rssi;
    int ret;

    pi = af->pi;
    if (!pi)
        return -ENODATA;

    ret = fm_af_jump_locked(af, pi, freq, pause_ms, reason, &paused);
    af->ops->read_signal(af->ctx, &rssi, &now);
    pr_debug("fm_af: jump to %u by hand: %d (reason 0x%02x, pause %s) in %u ms, "
            "now %u, -%u dBm\n", af_10khz(af, freq), ret, *reason,
            pause_ms ? (paused ? "yes" : "timed out") : "not waited",
            jiffies_to_msecs(jiffies - start), af_10khz(af, now), rssi);
    return ret;
}
