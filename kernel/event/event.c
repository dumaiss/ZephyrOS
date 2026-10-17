/* KonaOS device events: the post path (kernel/event, fixed core).
 *
 * `event_post` is the one entry a resident driver's own code calls from its
 * interrupt handler (drivers/include/driver.h): it credits the posted event
 * to a subscription.  It runs in fixed-core code because the driver
 * interrupt that calls it runs with the driver's window mapped, where the
 * cold half of bank 7 - the syscall bodies and the process table - is not
 * visible; everything it touches is permanent common memory.
 *
 * The event counts as pending exactly while its subscriber is the process
 * the interrupted context runs for (kernel/event/event_common.asm
 * kos_event_post_bank): the owner's own code and the kernel work it asked
 * for keep it pending, a run of any other process - a child it started -
 * drops it into `missed`.  Both counters saturate at FFFFh.
 */

#include "kos/types.h"

#include "kernel/event/event.h"

/* Common data is not zeroed at boot (only _DATA and _WINDOW_DATA are), so
 * the table and the per-process flags are initialized explicitly before any
 * driver can post: a garbage `owner` would look like a bound row and a
 * garbage `in_handler` would refuse every delivery. */
void kos_event_init(void)
{
	uint8_t i;

	for (i = 0u; i < KOS_EVENT_MAX; ++i) {
		struct kos_event_row *row = &kos_event_table[i];

		row->owner = 0u;
		row->bank = 0u;
		row->pending = 0u;
		row->missed = 0u;
		row->trampoline = 0u;
		row->handler = 0u;
		row->handle = 0u;
		row->reserved = 0u;
	}
	for (i = 0u; i < KOS_EVENT_IN_HANDLER_SIZE; ++i) {
		kos_event_in_handler[i] = 0u;
		kos_event_frame_latch[i] = 0u;
		kos_event_frame[i] = 0u;
	}
}

void kos_event_post(uint16_t sub, uint8_t bank)
{
	struct kos_event_row *row;

	if (sub == 0u || sub > KOS_EVENT_MAX) {
		return;
	}
	row = &kos_event_table[sub - 1u];
	if (row->owner == 0u) {
		return; /* unsubscribed since the driver last heard */
	}
	if (row->bank == bank) {
		if (row->pending != 0xFFFFu) {
			++row->pending;
		}
		kos_event_maybe = 1u;
	} else if (row->missed != 0xFFFFu) {
		++row->missed;
	}
}
