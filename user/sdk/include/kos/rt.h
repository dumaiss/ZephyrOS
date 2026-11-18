/* KonaOS SDK: the real-time interrupt (the native kos/rt.h ABI).
 *
 * One program at a time may own a handler that the IO controller's RT
 * clock runs at a fixed rate through the Z80's NMI - so no masked section
 * anywhere in the system can delay or drop a tick.  The program copies its
 * handler (position-independent assembly ending in RETN) and the data it
 * touches into the RT region of common memory, which every mapping shows,
 * then opens the interrupt:
 *
 *     memcpy((void *)KOS_RT_REGION_BASE, my_handler, my_handler_size);
 *     rt_open((void *)KOS_RT_REGION_BASE, 16000u);
 *     ...
 *     rt_close();
 *
 * The handler contract is the ABI's: preserve every register used, at
 * most KOS_RT_STACK_MAX bytes of stack, no syscall, no mapping change,
 * RETN, and finish well inside one period.  While the interrupt is open
 * the recovery store (`/system`) and reboot answer -KOS_EBUSY.
 *
 * Exit closes it: the registration and the RT clock's hold go with the
 * process, so a program that stops without rt_close leaves no clock
 * running.
 *
 * Return convention: a non-negative result, or the negated native error
 * code.
 */

#ifndef KOS_SDK_RT_H
#define KOS_SDK_RT_H

#include "kos/types.h"
#include "kos/errno.h"
#include "kos/rt.h"

/* Route the NMI to `handler` (an address inside the RT region) and start
 * the RT clock at `rate_hz` (KOS_RT_RATE_MIN..KOS_RT_RATE_MAX).  0, or
 * -KOS_EBUSY (another program holds it, or this one already does),
 * -KOS_EINVAL (a handler outside the region or a rate out of range),
 * -KOS_ENOTSUP (the IO controller's firmware has no RT clock),
 * -KOS_ENOENT/-KOS_ENODEV (no IO controller). */
int rt_open(const void *handler, unsigned int rate_hz);

/* The lane gap the next rt_open asks for, microseconds (0..255; 0, the
 * default, is the IO controller's 16).  The controller stops its serial
 * lanes shortly before each tick and resumes them this long after it, so
 * the bytes it streams never overrun the Z80's receive FIFO while the
 * handler runs: it must cover the handler's whole time from the NMI to its
 * RETN on the board.  0, or -KOS_EINVAL. */
int rt_gap(unsigned int us);

/* Stop the clock and give the interrupt back.  0 when nothing is open. */
int rt_close(void);

/* The region and whether the interrupt is held (struct kos_rt_info). */
int rt_info(struct kos_rt_info *out);

#endif /* KOS_SDK_RT_H */
