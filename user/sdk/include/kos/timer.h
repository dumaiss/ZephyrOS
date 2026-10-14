/* KonaOS SDK: the monotonic clock (the TIMER driver protocol).
 *
 * `/dev/timer` is the resident TIMER driver (the TIMER protocol class,
 * KOS_CLASS_TIMER): a free-running count of 25.6 µs units that wraps
 * after about 30.5 hours.  Elapsed times are unsigned differences, so
 * the wrap needs no special case for intervals shorter than the wrap.
 *
 * Return convention: a non-negative result, or the negated native error
 * code.
 */

#ifndef KOS_SDK_TIMER_H
#define KOS_SDK_TIMER_H

#include "kos/types.h"
#include "kos/errno.h"

/* Bind `/dev/timer` and return the handle, or the negated error
 * (-KOS_ENOENT when the namespace has no such name, -KOS_ENODEV when
 * the build publishes no timer driver). */
int timer_open(void);

/* Read the current count into `*now`: 0 or a negative error. */
int timer_now(int handle, uint32_t *now);

/* `counts` scaled to microseconds: counts * 25 + counts * 3 / 5, without
 * overflow while the elapsed time stays under about 71 minutes. */
uint32_t timer_us(uint32_t counts);

/* Wait for at least `us` microseconds by polling the clock: 0 when the
 * wait was served, the negated error when a reading failed, and
 * -KOS_ETIMEDOUT when the clock stops - a reading must move by one tick
 * (250 counts) within 100,000 consecutive readings, so a dead or frozen
 * clock never hangs the caller. */
int timer_wait_us(int handle, uint32_t us);

#endif /* KOS_SDK_TIMER_H */
