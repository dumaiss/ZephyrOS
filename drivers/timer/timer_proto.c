/* KonaOS TIMER resident driver.
 *
 * The monotonic clock as a bank-6 driver image: CTC channel 2 behind the
 * timer protocol of abi/kos/protocol.h (class KOS_CLASS_TIMER, instance
 * 0, version KOS_PROTOCOL_VERSION), published as /dev/timer.
 *
 * Segments (architecture 5.2):
 *   hot  this file and timer.c: the protocol dispatch, the init entry and
 *        the tick handler; the driver owns the interrupt source, the
 *        nucleus the vectoring (the claim runs here, in the mode-11
 *        window where the bootstrap interface is visible);
 *   boot timer_boot.c: the one-time channel programming, which runs
 *        through the mode-10 gate and is never needed again.
 *
 * Both operations are descriptor-less: the reply carries the count (NOW)
 * or the clock constants (INFO) as protocol payload.  A failed claim
 * would leave the clock dead, so the init refuses the image.
 */

#include "kos/errno.h"
#include "driver.h"
#include "kos/protocol.h"

extern uint32_t timer_counts_now(void);
extern void timer_irq_handler(void *context);
extern void timer_event_irq_handler(void *context);
extern int timer_event_arm(uint32_t period);
extern uint8_t timer_event_subscribed(void);
extern void timer_event_subscribe(uint8_t sub);
extern void timer_event_release(void);

/* The bootstrap table the tick and event handlers reach the nucleus
 * through (event_post); the init stores it. */
const struct driver_api *timer_driver_api;

static void timer_put_u32(struct kos_ipc_frame *frame, uint8_t at,
			  uint32_t value)
{
	frame->payload[at] = (uint8_t)(value & 0xFFu);
	frame->payload[at + 1u] = (uint8_t)((value >> 8) & 0xFFu);
	frame->payload[at + 2u] = (uint8_t)((value >> 16) & 0xFFu);
	frame->payload[at + 3u] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t timer_get_u32(struct kos_ipc_frame *frame, uint8_t at)
{
	return (uint32_t)frame->payload[at] |
	       ((uint32_t)frame->payload[at + 1u] << 8) |
	       ((uint32_t)frame->payload[at + 2u] << 16) |
	       ((uint32_t)frame->payload[at + 3u] << 24);
}

int16_t timer_driver_dispatch(struct kos_ipc_frame *frame)
{
	switch (frame->op) {
	case KTIMER_OP_NOW:
		timer_put_u32(frame, 0u, timer_counts_now());
		return 0;
	case KTIMER_OP_INFO:
		timer_put_u32(frame, 0u, KOS_TIMER_COUNTS_PER_10S);
		timer_put_u32(frame, 4u, KOS_TIMER_COUNTS_PER_TICK);
		return 0;
	case KTIMER_OP_EVENT:
		return (int16_t)timer_event_arm(timer_get_u32(frame, 0u));
	case KOS_OP_SUBSCRIBE:
		/* arg0 = the subscription id, 0 when it went away.  One
		 * event source: a second subscriber is refused. */
		if (frame->arg0 != 0u && timer_event_subscribed() != 0u) {
			return (int16_t)-KOS_EBUSY;
		}
		timer_event_subscribe(frame->arg0);
		return 0;
	case KOS_OP_RELEASE:
		/* The handle (and with it the subscription) went away: the
		 * channel stops, whatever object the release named. */
		timer_event_release();
		return 0;
	case KOS_OP_ACQUIRE:
		/* The clock has no exclusive mode: the tick keeps running
		 * for everyone, so an exclusive bind is refused. */
		return (int16_t)-KOS_ENOTSUP;
	default:
		return (int16_t)-KOS_EINVAL;
	}
}

uint8_t timer_driver_init(const struct driver_boot *boot)
{
	/* Two sources, one in-image handler each: the tick's channel and the
	 * event timer's.  The nucleus keeps the IM2 vectoring, the ISR stack
	 * and the final RETI and enters each handler in this driver's window
	 * (architecture 4.5); event_post is reached through the same table. */
	timer_driver_api = boot->api;
	if (boot->api->irq_claim(KOS_IRQ_CTC2,
				 (uint16_t)timer_irq_handler, 0u) != 0) {
		return 1u;
	}
	if (boot->api->irq_claim(KOS_IRQ_CTC3,
				 (uint16_t)timer_event_irq_handler, 0u) != 0) {
		return 1u;
	}
	return 0u;
}
