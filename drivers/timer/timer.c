/* KonaOS TIMER driver implementation (drivers/timer/).
 *
 * The monotonic clock: CTC channel 2 free-runs at 156.25 Hz (250 counts
 * of 25.6 µs on the 10 MHz system clock) and the interrupt handler counts
 * the ticks.  The current count is
 *
 *     now = tick count * 250 + (250 - the channel's down-counter)
 *
 * read as ticks, channel counter, ticks again, retrying when the tick
 * count moved in between, with the caller's interrupt state saved and
 * interrupts opened across the read so a rollover and its handler cannot
 * separate the counter from the tick count: a reading is never torn
 * across a tick, successive readings are monotonic, and the caller's
 * state is put back when the read is done.  The count wraps after about
 * 30.5 hours; elapsed values use unsigned subtraction.
 *
 * The handler is the clock's whole interrupt-side cost: it increments the
 * tick count and returns.  This source, timer_proto.c and timer_io.asm
 * (the interrupt-token helpers) are hot code (the mode-11 window reaches
 * them while the driver's window is mapped); timer_boot.c programs the
 * channel once through the mode-10 boot gate.
 *
 * This source is compiled into the `timer` driver image's hot segment.
 * The image links no multiply or divide runtime, so every scaling here is
 * a shift or an add.
 */

#include "kos/types.h"

#include "hardware.h"

/* The channel's counter port: physical channel 2 answers 41h on this
 * board (KOS_CTC2_PORT; the address bits are crossed), and a read
 * returns the running channel's current count. */
static __sfr __at KOS_CTC2_PORT timer_ctc2_port;

/* Ticks since boot; the handler is the only writer.  The tick/counter/tick
 * sequence in `timer_counts_now` guards a reader against a rollover between
 * the two tick reads.  The image's only run-time data, so a replay reads the
 * live count through the packed symbol offset. */
volatile uint32_t timer_ticks;

/* The interrupt-token helpers (timer_io.asm).  The read opens interrupts
 * for its critical section and restores the state the caller had. */
extern uint8_t timer_irq_save_enable(void);
extern void timer_irq_restore(uint8_t token);

void timer_irq_handler(void *context)
{
	(void)context;
	timer_ticks = timer_ticks + 1u;
}

/* The tick count scaled to counts, without a multiply runtime:
 * 250 * t = 256 * t - 4 * t - 2 * t. */
static uint32_t timer_tick_counts(uint32_t ticks)
{
	return (ticks << 8) - (ticks << 2) - (ticks << 1);
}

uint32_t timer_counts_now(void)
{
	uint32_t before;
	uint32_t after;
	uint16_t down;
	uint8_t state;

	/* A driver call runs with the caller's interrupt state: the syscall
	 * gate hands the body the state it was entered with (correction
	 * 01), so neither open nor masked can be assumed.  A masked read
	 * could pair a stale tick count with a counter the handler has not
	 * observed yet - `now` would go backwards - and the retry cannot
	 * see a rollover that never interrupts.  Save the state, open for
	 * the read (the handler then lands between the two tick reads,
	 * which the retry covers) and restore it: same discipline as the
	 * IOC driver's critical segments. */
	state = timer_irq_save_enable();

	for (;;) {
		before = timer_ticks;
		down = (uint16_t)timer_ctc2_port;
		after = timer_ticks;
		if (before == after) {
			break;
		}
	}

	timer_irq_restore(state);

	return timer_tick_counts(before) +
	       (uint16_t)(KOS_CTC2_TIME_CONSTANT - down);
}
