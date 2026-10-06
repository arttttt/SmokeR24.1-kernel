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
 * RDS alternative frequency (AF) switching for the FM receiver.
 *
 * The policy lives here: the station's PI and AF list as RDS gives them,
 * when the signal has become weak enough to look elsewhere, which AF to
 * try, and what to do when a jump fails or lands somewhere worse. The
 * jump itself is the chip's, reached through fm_af_ops, so nothing here
 * knows a register.
 */

#ifndef _FMDRV_AF_H
#define _FMDRV_AF_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/workqueue.h>

/* RDS method A: up to 25 alternative frequencies per station */
#define FM_AF_LIST_MAX 25

/*
 * What the switching needs of the driver. ctx is the driver's own. All but
 * trylock, receiving and freq_10khz are called with the tuner locked. A
 * jump is jump_prepare, perhaps pause_arm and a wait in fm_af, jump_go;
 * a look at an AF is pause_arm, the wait, probe; jump_restore ends either.
 */
struct fm_af_ops {
    /* the tuner's lock; the switching never blocks on it */
    bool (*trylock)(void *ctx);
    void (*unlock)(void *ctx);
    /* the receiver is on */
    bool (*receiving)(void *ctx);
    /* the signal, as |dBm|, and the frequency tuned, in the chip's units */
    int (*read_signal)(void *ctx, u8 *rssi, u16 *freq);
    /* arm the audio pause detector (fm_af_arm_pause first) */
    int (*pause_arm)(void *ctx);
    /* the signal on freq, as |dBm|, the audio muted while away: a tune
     * there and back, no PI check */
    int (*probe)(void *ctx, u16 freq, u8 *rssi);
    /* set the chip up to jump to freq, the station with this PI, onto a
     * signal of at least -min_dbm (0: the driver's own threshold) */
    int (*jump_prepare)(void *ctx, u16 pi, u8 min_dbm, u16 freq);
    /* jump, the audio muted: 0, or -EAGAIN with the chip's reason, or
     * another error */
    int (*jump_go)(void *ctx, u8 *reason);
    /* put the chip back to plain reception */
    void (*jump_restore)(void *ctx);
    /* a frequency in the chip's units, in 10 kHz, for the log */
    unsigned int (*freq_10khz)(u16 freq);
};

struct fm_af {
    const struct fm_af_ops *ops;
    void *ctx;
    struct workqueue_struct *wq;
    struct delayed_work work;

    /* the station: PI (0 until received) and AF list, in the chip's
     * units; from the RDS parser, under lock */
    spinlock_t lock;
    u16 pi;
    u16 list[FM_AF_LIST_MAX];
    u8 count;

    /* switching on (sysfs); stopping (FM going) */
    bool enabled;
    /* a reading at least this many -dBm is weak (sysfs) */
    u8 weak_dbm;
    bool stop;
    /* bumped by every tune or seek the user asks for */
    unsigned int tune_gen;

    /* the last ten RSSI readings, a bit each, 1 for weak */
    u16 hist;
    /* the AF a round starts from, round robin */
    u8 next;
    /* per AF in list: not to be looked at again before this (the chip
     * refused it: another station there) */
    unsigned long failed_until[FM_AF_LIST_MAX];
    /* no look or jump before this */
    unsigned long quiet_until;

    /* the chip's audio pause interrupt, waited for before a look or a jump */
    bool wait_pause;
    struct completion pause;
};

int fm_af_init(struct fm_af *af, const struct fm_af_ops *ops, void *ctx,
               const char *name);
void fm_af_destroy(struct fm_af *af);

/* the receiver on, and going off (waits for the switching to end) */
void fm_af_start(struct fm_af *af);
void fm_af_stop(struct fm_af *af);
void fm_af_set_enabled(struct fm_af *af, bool enabled);
void fm_af_set_weak(struct fm_af *af, u8 weak_dbm);

/* from the driver: a tune or seek asked for (another station), the
 * station's PI, one of its AFs (chip units) */
void fm_af_tuned(struct fm_af *af);
/* the chip used by someone else (a jump by hand): an attempt waiting for
 * a pause gives up */
void fm_af_preempt(struct fm_af *af);
void fm_af_rds_pi(struct fm_af *af, u16 pi);
void fm_af_rds_add(struct fm_af *af, u16 freq);

/* the pause interrupt: before it is unmasked, and when it comes (true
 * if a jump was waiting for it) */
void fm_af_arm_pause(struct fm_af *af);
bool fm_af_audio_paused(struct fm_af *af);

/* the station as known: PI, AF list */
u16 fm_af_snapshot(struct fm_af *af, u16 *list, u8 *count);

/* one jump by hand, the tuner locked: pause waited for up to pause_ms */
int fm_af_jump(struct fm_af *af, u16 freq, unsigned int pause_ms, u8 *reason);

#endif
