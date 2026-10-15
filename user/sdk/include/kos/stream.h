/* KonaOS SDK: stream measurement and refill planning.
 *
 * A stream never needs the clock; `fmeasure` opts one in.  The stream
 * then times every refill (the read that fills its buffer) with the
 * TIMER driver and `fstats` reports the counts: the fastest, mean and
 * slowest refill in 25.6 us units.  `stream_plan` is pure arithmetic on
 * those figures (or on a figure the caller brings, e.g. a card's
 * measured block time) and answers how a consumer should read a file:
 *
 *   B   the consumer's rate, bytes per second
 *   T   the period it wants attention at, in TIMER counts
 *   C   the refill chunk, bytes (KOS_STREAM_BUF_SIZE)
 *   t   the mean and worst refill times, in TIMER counts
 *
 * and produces
 *
 *   b = B*T              bytes the consumer eats per tick;
 *   n = floor(C / b)     ticks between two refills (a refill right
 *                        after a tick keeps every tick on time: one
 *                        longer than the period makes the tick late,
 *                        never lost);
 *   P = safety * B*t_max + C, rounded up to a whole C
 *                        bytes to pre-fetch, so the worst refill never
 *                        starves the consumer;
 *   P / R                start-up delay in milliseconds, with
 *                        R = C / t_mean (the medium's rate);
 *   U = B / R            utilisation, in percent, and `feasible`,
 *                        which is U < 50 %.
 *
 * A plan that is not feasible is not a failure: it says the medium
 * cannot serve this consumer with the safety asked for.  Short names:
 * this section is not ISO C.
 */

#ifndef KOS_SDK_STREAM_H
#define KOS_SDK_STREAM_H

#include "kos/types.h"
#include "kos/errno.h"
#include "klibc/stdio.h"

/* One measured refill set: counts are TIMER counts (25.6 us). */
struct kos_stream_stats {
	unsigned int refills;   /* refills measured */
	uint32_t min;           /* fastest refill */
	uint32_t max;           /* slowest refill */
	uint32_t mean;          /* sum / refills */
};

/* What the consumer asks for:
 *   rate    bytes per second the consumer needs;
 *   period  the period it must be served within, TIMER counts;
 *   chunk   the refill size, bytes (KOS_STREAM_BUF_SIZE);
 *   mean    mean refill time, TIMER counts;
 *   worst   worst-case refill time, TIMER counts;
 *   safety  worst-case factor for the pre-fetch, 0 selects 2. */
struct kos_stream_need {
	unsigned long rate;
	unsigned int period;
	unsigned int chunk;
	unsigned int mean;
	unsigned int worst;
	unsigned int safety;
};

/* What the numbers say.  `bytes_per_tick_millis` is b * 1000: a byte
 * per tick is a milli-byte per tick's worth of precision the consumer
 * would lose as an integer. */
struct kos_stream_plan {
	unsigned long bytes_per_tick_millis;
	unsigned int ticks_per_refill;
	unsigned long prefetch;
	unsigned long startup_ms;
	unsigned int utilisation_pct;
	unsigned char feasible;
};

/* Time every refill of `stream` with the TIMER handle `timer`; 0 turns
 * measurement off.  Returns 0 or the negated error. */
int fmeasure(FILE *stream, int timer);

/* The measurement so far: 0, or -KOS_ENOENT when no refill has been
 * measured yet. */
int fstats(FILE *stream, struct kos_stream_stats *stats);

/* Fill `plan` from `need`: 0, or -KOS_EINVAL for a need that names no
 * bytes (a zero rate, period, chunk or refill time). */
int stream_plan(const struct kos_stream_need *need,
		struct kos_stream_plan *plan);

#endif /* KOS_SDK_STREAM_H */
