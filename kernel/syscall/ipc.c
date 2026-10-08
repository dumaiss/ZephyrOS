/* ZephyrOS IPC syscall bodies (kernel/syscall, cold half).
 *
 * The P-normalizing syscall gate (kernel/syscall/entry.asm) saves the
 * caller's whole latch and dispatches with P = 0, so this code runs with
 * the window showing bank 7 and can touch the caller window and the
 * cold half directly.  It validates the argument block, then hands the
 * request to the fixed-core substrate (kernel/ipc), which captures the
 * descriptor from the gate's saved latch and opens the service domain.
 *
 * Result conventions (docs/abi.md section 12): the IPC status is the
 * primary result published in HL with carry clear (negative values are
 * -ZOS_E*); a structural problem with the request itself is reported
 * with carry set and A = ZOS_EINVAL before the substrate is entered.
 */

#include "zos/ipc.h"
#include "zephyros/errno.h"

#include "kernel/ipc/ipc.h"
#include "kernel/syscall/diag.h"
#include "kernel/syscall/ipc.h"

void zos_ipc_dispatch(uint8_t number, uint8_t bank, uint16_t block)
{
	(void)bank;
	switch (number) {
	case ZOS_SYSCALL_IPC_LOOKUP: {
		struct zos_ipc_lookup_args *args =
			(struct zos_ipc_lookup_args *)block;
		int8_t handle;

		if (!zos_syscall_block_ok(block, sizeof(*args))) {
			zos_syscall_error(ZOS_EINVAL);
			return;
		}
		args->version = 0u;
		handle = ipc_lookup(args->class, args->instance,
				    &args->version);
		zos_syscall_success((uint16_t)(uint8_t)handle);
		return;
	}
	case ZOS_SYSCALL_IPC_CALL: {
		struct zos_ipc_call_args *args =
			(struct zos_ipc_call_args *)block;

		if (!zos_syscall_block_ok(block, sizeof(*args)) ||
		    args->frame_length == 0u ||
		    args->frame_length > ZOS_IPC_FRAME_SIZE ||
		    args->reserved != 0u) {
			zos_syscall_error(ZOS_EINVAL);
			return;
		}
		zos_syscall_success((uint16_t)ipc_syscall_call(args));
		return;
	}
	default:
		zos_syscall_error(ZOS_ENOSYS);
		return;
	}
}
