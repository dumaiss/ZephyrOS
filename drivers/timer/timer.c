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
 * The tick handler increments the tick count and, while an event period
 * is armed, checks the next deadline (below).  This source, timer_proto.c and timer_io.asm
 * (the interrupt-token helpers) are hot code (the mode-11 window reaches
 * them while the driver's window is mapped); timer_boot.c programs the
 * channel once through the mode-10 boot gate.
 *
 * This source is compiled into the `timer` driver image's hot segment.
 * The image links no multiply or divide runtime, so every scaling here is
 * a shift or an add.
 */

#include "kos/errno.h"
#include "kos/types.h"

#include "driver.h"
#include "hardware.h"

/* The channel's counter port: physical channel 2 answers 41h on this
 * board (KOS_CTC2_PORT; the address bits are crossed), and a read
 * returns the running channel's current count. */
static __sfr __at KOS_CTC2_PORT timer_ctc2_port;

/* Ticks since boot; the handler is the only writer.  The tick/counter/tick
 * sequence in `timer_counts_now` guards a reader against a rollover between
 * the two tick reads.  A replay reads the live count through the packed
 * symbol offset. */
volatile uint32_t timer_ticks;

/* The interrupt-token helpers (timer_io.asm).  The read opens interrupts
 * for its critical section and restores the state the caller had. */
extern uint8_t timer_irq_save_enable(void);
extern uint8_t timer_irq_save_disable(void);
extern void timer_irq_restore(uint8_t token);

uint32_t timer_counts_now(void);
uint32_t timer_counts_now_locked(void);
static void timer_event_service(uint32_t now);

/* --- the event timer (CTC channel 3) -------------------------------------
 *
 * One subscription at a time (the nucleus issues the id through
 * KOS_OP_SUBSCRIBE).  The clock above is the event timer's only time
 * reference: the driver keeps an absolute deadline in clock counts and
 * adds the period at each one, so deadlines never drift.  Channel 3 only
 * covers the final stretch: the tick handler, which reads the clock
 * exactly (the tick count was just incremented and the counter has just
 * reloaded), loads channel 3 once with the counts left when the next
 * deadline is within KOS_CTC3_REACH, and channel 3's handler stops the
 * channel and posts.  Nothing is counted on channel 3, so an interrupt
 * held off by a masked stretch makes a post late, never lost, doubled or
 * early, and the deadline after it is still exact.
 */

static __sfr __at KOS_CTC3_PORT timer_ctc3_port;

extern const struct driver_api *timer_driver_api;

static uint8_t timer_event_sub;      /* 0 = no subscription          */
static uint8_t timer_event_armed;    /* a period is set              */
static uint8_t timer_event_loaded;   /* channel 3 holds the stretch  */
volatile uint32_t timer_event_deadline;
volatile uint32_t timer_event_period;

void timer_irq_handler(void *context)
{
	(void)context;
	timer_ticks = timer_ticks + 1u;
	if (timer_event_armed != 0u && timer_event_loaded == 0u) {
		timer_event_service(timer_counts_now_locked());
	}
}

/* Load channel 3 for `counts` (1..KOS_CTC3_REACH; 256 is written as 0):
 * reset, control word and constant, so the load starts the channel from
 * any state, the power-on one included. */
static void timer_event_load(uint16_t counts)
{
	timer_event_loaded = 1u;
	timer_ctc3_port = KOS_CTC3_CONTROL_LOAD;
	timer_ctc3_port = (uint8_t)counts;
}

static void timer_event_stop_channel(void)
{
	timer_event_loaded = 0u;
	timer_ctc3_port = KOS_CTC_RESET;
}

/* Post every deadline `now` has reached, then load channel 3 when the next
 * one is within reach; otherwise a later tick does it.  Runs masked (an
 * interrupt handler, or the arm's critical section). */
static void timer_event_service(uint32_t now)
{
	uint32_t left;

	while (((now - timer_event_deadline) & 0x80000000u) == 0u) {
		timer_driver_api->event_post(timer_event_sub);
		timer_event_deadline += timer_event_period;
	}
	left = timer_event_deadline - now;
	if (left <= KOS_CTC3_REACH) {
		timer_event_load((uint16_t)left);
	}
}

void timer_event_irq_handler(void *context)
{
	uint32_t now;

	(void)context;
	if (timer_event_armed == 0u || timer_event_loaded == 0u) {
		/* A stop raced the expiry: the request is stale. */
		timer_event_stop_channel();
		return;
	}
	/* The clock reading is one tick low when a tick expired while this
	 * handler was being entered and its own handler has not run yet.
	 * The channel only expires at or after the deadline, so a reading
	 * below the deadline is that stale one. */
	now = timer_counts_now_locked();
	if (((now - timer_event_deadline) & 0x80000000u) != 0u) {
		now += KOS_CTC2_TIME_CONSTANT;
	}
	timer_event_stop_channel();
	timer_event_service(now);
}

/* The driver body's KTIMER_OP_EVENT.  A period of 0 stops the channel. */
int timer_event_arm(uint32_t period)
{
	uint32_t before;
	uint32_t now;
	uint8_t token;

	if (period == 0u) {
		/* Stopping is always allowed and always idempotent: the
		 * unsubscribe and the release both drop the subscription
		 * first and stop the channel after, so this cannot require
		 * one to still be in place. */
		token = timer_irq_save_disable();
		timer_event_armed = 0u;
		timer_event_stop_channel();
		timer_irq_restore(token);
		return 0;
	}
	if (timer_event_sub == 0u) {
		return -KOS_EINVAL;
	}
	/* The masked reading below can stand one tick low when a tick expired
	 * after the mask and its handler has not run; the open reading just
	 * before is a few counts earlier and never low, so a masked reading
	 * below it is the stale one. */
	before = timer_counts_now();
	token = timer_irq_save_disable();
	if (timer_event_armed != 0u) {
		timer_irq_restore(token);
		return -KOS_EBUSY;
	}
	now = timer_counts_now_locked();
	if (((now - before) & 0x80000000u) != 0u) {
		now += KOS_CTC2_TIME_CONSTANT;
	}
	timer_event_period = period;
	timer_event_deadline = now + period;
	timer_event_armed = 1u;
	timer_event_loaded = 0u;
	timer_event_service(now);
	timer_irq_restore(token);
	return 0;
}

uint8_t timer_event_subscribed(void)
{
	return timer_event_sub;
}

void timer_event_subscribe(uint8_t sub)
{
	if (sub == 0u) {
		timer_event_sub = 0u;
		(void)timer_event_arm(0u); /* stop the channel with it */
		return;
	}
	if (timer_event_sub != 0u) {
		return; /* the nucleus asks one subscription at a time */
	}
	timer_event_sub = sub;
}

void timer_event_release(void)
{
	timer_event_sub = 0u;
	(void)timer_event_arm(0u);
}

/* The tick count scaled to counts, without a multiply runtime:
 * 250 * t = 256 * t - 4 * t - 2 * t. */
static uint32_t timer_tick_counts(uint32_t ticks)
{
	return (ticks << 8) - (ticks << 2) - (ticks << 1);
}

/* The same reading with interrupts already masked: the caller must hold
 * them (the event arm), and the tick count and the channel counter are
 * then consistent because no tick can land in between. */
uint32_t timer_counts_now_locked(void)
{
	return timer_tick_counts(timer_ticks) +
	       (uint16_t)(KOS_CTC2_TIME_CONSTANT - (uint16_t)timer_ctc2_port);
}

uint32_t timer_counts_now(void)
{
	uint32_t before;
	uint32_t after;
	uint16_t down;
	uint8_t state;

	/* A driver call runs with the caller's interrupt state: the syscall
	 * gate hands the body the state it was entered with, so neither
	 * open nor masked can be assumed.  A masked read
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
