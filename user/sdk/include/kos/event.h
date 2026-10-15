/* KonaOS SDK: device events (`kos/event.h`).
 *
 * A device event is a signal a driver raises for an application that
 * subscribed to it.  `notify` subscribes the calling process to the events
 * of a bound device handle; the nucleus delivers them to `handler` at a
 * point where the program is running its own code again - never while it is
 * inside a syscall, an IPC call or another handler, and only while this
 * process is the one running.  Delivery is invisible otherwise: the runtime
 * entry and the return are libkos code, and a program that never calls
 * `notify` pays nothing for it.
 *
 * Events coalesce: the handler receives how many happened since its last
 * call (`count`) and how many were dropped since then (`missed`); nothing is
 * queued.  A dropped event is one that arrived while a child of this process
 * was running.
 *
 * Return convention matches kos/console.h: a non-negative result, or the
 * negated native error code.  `notify` needs a handle whose endpoint
 * declared events (the driver manifest's `"events": true`): every other
 * handle answers -KOS_ENOTSUP.
 */

#ifndef KOS_SDK_EVENT_H
#define KOS_SDK_EVENT_H

#include "kos/types.h"
#include "kos/errno.h"

/* The handler signature `notify` takes: ordinary application code in the
 * program's primary image (never a far segment - the nucleus returns to
 * the program's own bank).  It runs with interrupts enabled on the
 * program's own stack, may call the SDK and make IPC calls, and must run
 * short: it is entered from wherever the program was interrupted, so it
 * must not touch state the interrupted code may be halfway through - the
 * same rule an interrupt handler obeys - and everything it does delays the
 * code it interrupted.  A `count` above 1 means that many events arrived
 * since the last call; `missed` counts the ones the nucleus dropped. */
typedef void (*kos_event_handler_t)(unsigned int count, unsigned int missed);

/* Subscribe `handler` to the events of the device `handle`, or unsubscribe
 * when `handler` is 0 (a handle that is not subscribed then answers
 * -KOS_ENOENT).  One subscription per handle; a second `notify` on a
 * subscribed handle replaces the handler and costs no slot.  A handle
 * bound from an endpoint without event support is -KOS_ENOTSUP, and a
 * handle the calling process does not hold is -KOS_EINVAL.  The
 * subscription dies with the handle - unbind or process exit - which is
 * also what tells the driver to stop a running event source. */
int notify(int handle, kos_event_handler_t handler);

/* Ask a TIMER handle for its event timer: one event every `period_counts`
 * clock counts (25.6 µs each, so 782 counts is 20 ms) from the first
 * deadline; 0 stops it.  Deadlines are absolute, so successive events do
 * not drift, and the driver refuses a second subscription or an arm while
 * the timer runs with -KOS_EBUSY; an arm with no subscription is
 * -KOS_EINVAL.  The channel is off until the first arm and stays off after
 * a stop. */
int timer_event(int handle, uint32_t period_counts);

#endif /* KOS_SDK_EVENT_H */
