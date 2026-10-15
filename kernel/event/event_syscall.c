/* KonaOS device events: the syscall and teardown bodies (kernel/event).
 *
 * Cold-half code like the rest of the syscall dispatcher: SYS_NOTIFY and
 * SYS_EVENT_RETURN are entered with the window on bank 7 (the gate
 * normalizes the page field), so the process table, the namespace resolver
 * and the endpoint flags are reachable, and so is the common event table.
 *
 * SYS_NOTIFY binds one subscription per (process, handle) pair: the row is
 * the id the driver is told (KOS_OP_SUBSCRIBE with the id, 0 for an
 * unsubscribe), and the nucleus keeps the handler and the libkos trampoline
 * that carry the delivery.  SYS_EVENT_RETURN closes a delivery: the
 * trampoline re-enters the nucleus with the frame two bytes above the
 * caller's RST return address, and the body either hands out the next
 * pending event through that same frame or unwinds it to the interrupted
 * code (kernel/event/event_common.asm).
 */

#include "kos/errno.h"
#include "kos/ipc.h"
#include "kos/object.h"
#include "kos/protocol.h"
#include "kos/syscall.h"
#include "kos/types.h"

#include "process.h"

#include "kernel/event/event.h"
#include "kernel/ipc/ipc.h"
#include "kernel/irq/irq_private.h"
#include "kernel/syscall/diag.h"
#include "kernel/vfs/vfs.h"

#include "mod.h"
#include "platform.h"

static void event_fail(int result)
{
	kos_syscall_error((uint8_t)(uint16_t)(0u - (uint16_t)result));
}

/* One KOS_OP_SUBSCRIBE to the endpoint: `sub` is the subscription id, 0 to
 * tell the driver the subscription went away.  The result is the driver's
 * own status; a driver that has no event support never sees the call (its
 * endpoint does not declare MOD_EP_EVENTS). */
static int event_subscribe(uint8_t endpoint, uint8_t sub)
{
	struct kos_ipc_frame frame;

	frame.op = KOS_OP_SUBSCRIBE;
	frame.flags = 0u;
	frame.desc = KOS_IPC_DESC_NONE;
	frame.arg0 = sub;
	frame.arg1 = 0u;
	return ipc_nucleus_call(endpoint, &frame);
}

void kos_event_notify(uint16_t block)
{
	struct kos_notify_args *args = (struct kos_notify_args *)block;
	struct kos_event_row *row = (struct kos_event_row *)0;
	uint8_t endpoint = 0u;
	uint8_t object = 0u;
	uint8_t pid = process_current_pid();
	uint8_t sub = 0u;
	uint8_t i;
	int result;

	if (!kos_syscall_block_ok(block, (uint16_t)sizeof(*args)) ||
	    args->reserved != 0u) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	result = vfs_ns_resolve(pid, args->handle, &endpoint, &object);
	if (result < 0) {
		event_fail(result);
		return;
	}
	for (i = 0u; i < KOS_EVENT_MAX; ++i) {
		if (kos_event_table[i].owner == pid &&
		    kos_event_table[i].handle == args->handle) {
			row = &kos_event_table[i];
			sub = (uint8_t)(i + 1u);
			break;
		}
	}
	if (args->handler == 0u) {
		uint8_t token = irq_arch_lock();

		if (row == (struct kos_event_row *)0) {
			irq_arch_unlock(token);
			kos_syscall_error(KOS_ENOENT);
			return;
		}
		row->owner = 0u;
		row->pending = 0u;
		row->missed = 0u;
		row->handler = 0u;
		row->trampoline = 0u;
		irq_arch_unlock(token);
		(void)event_subscribe(endpoint, 0u);
		kos_syscall_success(0u);
		return;
	}
	if ((ipc_endpoint_flags(endpoint) & MOD_EP_EVENTS) == 0u) {
		kos_syscall_error(KOS_ENOTSUP);
		return;
	}
	/* The handler and the trampoline are application code in the owner's
	 * own image: the window's lower bound up to common memory. */
	if (args->handler < KOS_SYSCALL_WINDOW_BASE ||
	    args->handler >= KOS_COMMON_BASE ||
	    args->trampoline < KOS_SYSCALL_WINDOW_BASE ||
	    args->trampoline >= KOS_COMMON_BASE) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	if (row == (struct kos_event_row *)0) {
		uint8_t bank = process_current_bank();
		uint8_t token = irq_arch_lock();

		for (i = 0u; i < KOS_EVENT_MAX; ++i) {
			if (kos_event_table[i].owner == 0u) {
				row = &kos_event_table[i];
				sub = (uint8_t)(i + 1u);
				break;
			}
		}
		if (row == (struct kos_event_row *)0) {
			irq_arch_unlock(token);
			kos_syscall_error(KOS_ENOSPC);
			return;
		}
		row->owner = pid;
		row->bank = bank;
		row->handle = args->handle;
		row->pending = 0u;
		row->missed = 0u;
		irq_arch_unlock(token);
		result = event_subscribe(endpoint, sub);
		if (result != 0) {
			token = irq_arch_lock();
			row->owner = 0u;
			irq_arch_unlock(token);
			event_fail(result < 0 ? result : -KOS_EBUSY);
			return;
		}
	}
	row->handler = args->handler;
	row->trampoline = args->trampoline;
	kos_syscall_success(0u);
}

void kos_event_return(void)
{
	uint8_t pid = process_current_pid();
	uint16_t frame;

	if (pid >= KOS_EVENT_IN_HANDLER_SIZE ||
	    kos_event_in_handler[pid] == 0u) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	/* Masked from here on: the syscall body runs with the caller's
	 * interrupt state, and every interrupt exit check rewrites
	 * kos_event_latch, so an interrupt between loading the frame's latch
	 * and the unwind would hand the program the nucleus's mapping.  The
	 * enter and the unwind both leave with EI, so nothing restores this. */
	(void)irq_arch_lock();
	kos_event_in_handler[pid] = 0u;
	frame = kos_event_return_frame();
	kos_event_frame[pid] = frame;
	kos_event_latch = kos_event_frame_latch[pid];
	while (kos_event_pick() != 0u) {
		kos_event_enter();
	}
	kos_event_unwind(frame);
}

void kos_event_handle_release(uint8_t pid, uint8_t handle)
{
	uint8_t token = irq_arch_lock();
	uint8_t i;

	for (i = 0u; i < KOS_EVENT_MAX; ++i) {
		if (kos_event_table[i].owner == pid &&
		    kos_event_table[i].handle == handle) {
			kos_event_table[i].owner = 0u;
			kos_event_table[i].pending = 0u;
			kos_event_table[i].missed = 0u;
			kos_event_table[i].handler = 0u;
			kos_event_table[i].trampoline = 0u;
		}
	}
	irq_arch_unlock(token);
}

void kos_event_process_exit(uint8_t pid)
{
	uint8_t token = irq_arch_lock();
	uint8_t i;

	for (i = 0u; i < KOS_EVENT_MAX; ++i) {
		if (kos_event_table[i].owner == pid) {
			kos_event_table[i].owner = 0u;
			kos_event_table[i].pending = 0u;
			kos_event_table[i].missed = 0u;
			kos_event_table[i].handler = 0u;
			kos_event_table[i].trampoline = 0u;
		}
	}
	if (pid < KOS_EVENT_IN_HANDLER_SIZE) {
		kos_event_in_handler[pid] = 0u;
	}
	irq_arch_unlock(token);
}
