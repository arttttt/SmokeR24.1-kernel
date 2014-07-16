/*
 * You SHOULD NOT be including this unless you're vsyscall
 * handling code or timekeeping internal code!
 */

#ifndef _LINUX_TIMEKEEPER_INTERNAL_H
#define _LINUX_TIMEKEEPER_INTERNAL_H

#include <linux/clocksource.h>
#include <linux/jiffies.h>
#include <linux/time.h>

/**
 * struct tk_read_base - base structure for timekeeping readout
 * @clock:	Current clocksource used for timekeeping.
 * @read:	Read function of @clock
 * @mask:	Bitmask for two's complement subtraction of non 64bit clocks
 * @cycle_last: @clock cycle value at last update
 * @mult:	NTP adjusted multiplier for scaled math conversion
 * @shift:	Shift value for scaled math conversion
 * @xtime_nsec: Shifted (fractional) nano seconds offset for readout
 * @base_mono:  ktime_t (nanoseconds) base time for readout
 *
 * This struct has size 56 byte on 64 bit. Together with a seqcount it
 * occupies a single 64byte cache line.
 *
 * The struct is separate from struct timekeeper as it is also used
 * for a fast NMI safe accessor to clock monotonic.
 */
struct tk_read_base {
	struct clocksource	*clock;
	cycle_t			(*read)(struct clocksource *cs);
	cycle_t			mask;
	cycle_t			cycle_last;
	u32			mult;
	u32			shift;
	u64			xtime_nsec;
	ktime_t			base_mono;
};

struct timekeeper {
	struct tk_read_base	tkr;
	/* Current clocksource used for timekeeping. */
	struct clocksource	*clock;
	/* Read function of @clock */
	cycle_t			(*read)(struct clocksource *cs);
	/* Bitmask for two's complement subtraction of non 64bit counters */
	cycle_t			mask;
	/* Last cycle value */
	cycle_t			cycle_last;
	/* NTP adjusted clock multiplier */
	u32			mult;
	/* The shift value of the current clocksource. */
	u32			shift;
	/* Clock shifted nano seconds */
	u64			xtime_nsec;

	/* Current CLOCK_REALTIME time in seconds */
	u64			xtime_sec;
	/* Difference between accumulated time and NTP time in ntp
	 * shifted nano seconds. */
	s64			ntp_error;
	/* CLOCK_REALTIME to CLOCK_MONOTONIC offset */
	struct timespec 	wall_to_monotonic;

	/* Offset clock monotonic -> clock realtime */
	ktime_t			offs_real;
	ktime_t			offs_boot;
	ktime_t			offs_tai;

	/* time spent in suspend */
	struct timespec	        total_sleep_time;
	/* The current UTC to TAI offset in seconds */
	s32			tai_offset;

	/* The raw monotonic time for the CLOCK_MONOTONIC_RAW posix clock. */
	struct timespec 	raw_time;

	/* The following members are for timekeeping internal use */
	cycle_t			cycle_interval;
	u64			xtime_interval;
	s64			xtime_remainder;
	u32			raw_interval;

	/*
	 * Difference between accumulated time and NTP time in ntp
	 * shifted nano seconds.
	 */
	u32			ntp_error_shift;
};

static inline struct timespec tk_xtime(struct timekeeper *tk)
{
	struct timespec ts;

	ts.tv_sec = tk->xtime_sec;
	ts.tv_nsec = (long)(tk->xtime_nsec >> tk->shift);
	return ts;
}


#ifdef CONFIG_GENERIC_TIME_VSYSCALL

extern void update_vsyscall(struct timekeeper *tk);
extern void update_vsyscall_tz(void);

#elif defined(CONFIG_GENERIC_TIME_VSYSCALL_OLD)

extern void update_vsyscall_old(struct timespec *ts, struct timespec *wtm,
				struct clocksource *c, u32 mult,
				cycles_t cycle_last);
extern void update_vsyscall_tz(void);

static inline void update_vsyscall(struct timekeeper *tk)
{
	struct timespec xt;

	xt = tk_xtime(tk);
	update_vsyscall_old(&xt, &tk->wall_to_monotonic, tk->clock, tk->mult);
}

#else

static inline void update_vsyscall(struct timekeeper *tk)
{
}
static inline void update_vsyscall_tz(void)
{
}
#endif

#endif /* _LINUX_TIMEKEEPER_INTERNAL_H */
