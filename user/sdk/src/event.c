/* KonaOS libkos: device events.
 *
 * `notify` marshals the subscription block through the shared low-window
 * scratch (kos_internal.h) and the raw gate stub; the trampoline it hands
 * the nucleus is `kos_event_entry` (event_entry.asm), which is the libkos
 * code the nucleus returns to and the only place SYS_EVENT_RETURN is
 * issued.
 *
 * `timer_event` is an ordinary TIMER protocol call: an event-timer period
 * is four bytes of request payload and needs no descriptor.
 *
 * Return convention matches kos/console.h: a non-negative result, or the
 * negated native error code.
 */

#include "kos.h"

#include "kos/event.h"
#include "kos/protocol.h"
#include "kos/syscall.h"

#include "kos_internal.h"

/* The libkos trampoline the nucleus returns to for a delivery.  Its
 * address is only ever passed as the subscription block's `trampoline`; a
 * program never calls it. */
extern void kos_event_entry(unsigned int count, unsigned int missed);

int notify(int handle, kos_event_handler_t handler)
{
	kos_scratch.notify.handle = (unsigned char)handle;
	kos_scratch.notify.reserved = 0u;
	kos_scratch.notify.handler = (unsigned int)handler;
	kos_scratch.notify.trampoline = (unsigned int)kos_event_entry;
	return kos_ipc_raw(KOS_SYSCALL_NOTIFY, &kos_scratch.notify);
}

int timer_event(int handle, uint32_t period_counts)
{
	struct kos_ipc_request req;

	req.frame.op = KTIMER_OP_EVENT;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.frame.payload[0] = (unsigned char)(period_counts & 0xFFu);
	req.frame.payload[1] = (unsigned char)((period_counts >> 8) & 0xFFu);
	req.frame.payload[2] = (unsigned char)((period_counts >> 16) & 0xFFu);
	req.frame.payload[3] = (unsigned char)((period_counts >> 24) & 0xFFu);
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	return call((unsigned char)handle, &req);
}
