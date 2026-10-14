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

static void timer_put_u32(struct kos_ipc_frame *frame, uint8_t at,
			  uint32_t value)
{
	frame->payload[at] = (uint8_t)(value & 0xFFu);
	frame->payload[at + 1u] = (uint8_t)((value >> 8) & 0xFFu);
	frame->payload[at + 2u] = (uint8_t)((value >> 16) & 0xFFu);
	frame->payload[at + 3u] = (uint8_t)((value >> 24) & 0xFFu);
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
	default:
		return (int16_t)-KOS_EINVAL;
	}
}

uint8_t timer_driver_init(const struct driver_boot *boot)
{
	/* One source, one in-image handler: the nucleus keeps the IM2
	 * vectoring, the ISR stack and the final RETI and enters the
	 * handler in this driver's window (architecture 4.5). */
	if (boot->api->irq_claim(KOS_IRQ_CTC2,
				 (uint16_t)timer_irq_handler, 0u) != 0) {
		return 1u;
	}
	return 0u;
}
