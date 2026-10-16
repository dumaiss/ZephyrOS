/* Read-path phase instrument (drivers, `KOS_IOC_PHASES=1` only).
 *
 * A measurement build option, off in every shipped image: it records the
 * timer clock at each step of one `/fs` block read, into a `struct
 * kfile_phases` in the stamping driver's own DRIVER_IO
 * (abi/kos/protocol.h owns the phase ids, the table and its one reader
 * operation, KFILE_OP_PHASES).  Nothing here runs, costs a byte or is
 * even compiled without the option; the `phase-image` make target is the
 * only one that defines it.
 *
 * The clock, programmed by the boot segment (ioc_phases_clock.h): the
 * two free-running CTC channels that no shipped image programs plus the
 * timer's own channel - channel 1, which the instrument programs once
 * (timer mode, 256 prescaler, interrupts off, 256-count cycle), and
 * channel 2, which the timer driver keeps running at 156.25 Hz.  Both
 * count the same 10 MHz / 256 clock as the tick counter, so one
 * count is the documented 25.6 us, and reading a counter pair is two port
 * reads: no IPC, no kernel or timer-driver involvement, and - the reason
 * for this clock - it keeps counting inside the interrupt-masked bulk
 * window, where a tick count cannot be read at all and loses ticks the
 * instrument must see (the timer's CTC latches one pending request, so
 * every masked expiry past the first is lost).  Each stamp stores one
 * byte per channel that increases with time modulo that channel's cycle
 * length; the difference between two stamps is resolved to one count
 * difference through the two cycle lengths (250 and 256, LCM 32,000
 * counts = 0.8192 s) by the fetching program, so a stamped pair carries
 * no hidden epoch.
 *
 * One stamp costs two port reads plus a retry while the pair is not
 * resolvable: channel 1 must not move between them, and the pair must
 * carry the parity the last accepted one did.  Both channels advance one
 * count per 256 T-states, so a clean pair's two cycle differences are
 * congruent modulo 2; a second read that landed across a channel-2 reload
 * (its 250-count cycle reloads out of step with channel 1) breaks that,
 * and no congruence answers the pair.  The stamp retakes such a pair
 * (bounded), and the fetching program verifies the same parity before it
 * resolves a difference, so a pair the retries could not fix is reported
 * as torn rather than answered with a wrong count.
 *
 * The instrument is read-only and side-effect free from the system's
 * point of view: it writes its own table and touches two CTC channels no
 * other image programs, and a warm reboot stops them (the programming
 * runs in a driver boot segment, which every boot re-runs).
 */

#ifndef KONAOS_DRIVERS_IOC_PHASES_H
#define KONAOS_DRIVERS_IOC_PHASES_H

#include "hardware.h"
#include "kos/protocol.h"

/* The clock half lives apart: a boot segment starts channel 1 without the
 * stamping code or its reference byte (ioc_phases_clock.h). */
#include "ioc_phases_clock.h"

#if KOS_IOC_PHASES

/* The parity the last accepted pair carried: both channels advance one
 * count per 256 T-states, so a clean pair's two cycle differences are
 * congruent modulo 2, and that parity is a constant of the two clocks.
 * It is the reference a retaken pair is held to; `known` is clear until
 * the image's first stamp, which has nothing to compare against. */
static uint8_t ioc_phases_parity;
static uint8_t ioc_phases_parity_known;

/* One stamp: the two counters' elapsed bytes, taken as close together as
 * the code allows, into `table`.  A CTC counter is free-running and
 * readable at any time, masked or not.  Channel 1 is read either side of
 * channel 2, and a pair is retaken while channel 1 moved inside it (a
 * 6.5536 ms cycle makes a missed retry rare) or while its parity differs
 * from the last accepted pair's - see the file note.  The retries are
 * bounded and the last pair is stored either way, and it becomes the new
 * reference: a pair that could not be matched only ever costs the
 * fetching program a sample, it can never be answered with a wrong
 * count. */
static void ioc_phases_stamp(struct kfile_phases *table, uint8_t phase)
{
	uint8_t first = 0u;
	uint8_t second = 0u;
	uint8_t parity = 0u;
	uint8_t tries;

	for (tries = 4u; tries != 0u; --tries) {
		first = ioc_phases_ctc1;
		second = ioc_phases_ctc2;
		parity = (uint8_t)((first ^ second) & 1u);
		if (first != ioc_phases_ctc1) {
			continue; /* channel 1 moved inside the pair */
		}
		if (ioc_phases_parity_known == 0u ||
		    parity == ioc_phases_parity) {
			break;
		}
	}
	table->elapsed_1[phase] = (uint8_t)(0u - first);
	table->elapsed_2[phase] = (uint8_t)(0u - second);
	table->valid |= (uint16_t)(1u << phase);
	ioc_phases_parity = parity;
	ioc_phases_parity_known = 1u;
}

/* Drop every stamp: the delimiter a read uses to tell its own phases from
 * whatever the previous operation left. */
static void ioc_phases_clear(struct kfile_phases *table)
{
	table->valid = 0u;
	table->flags = 0u;
	table->path = KFILE_PHASE_PATH_UNKNOWN;
}

#endif /* KOS_IOC_PHASES */

#endif /* KONAOS_DRIVERS_IOC_PHASES_H */
