/* ZephyrOS kernel-private IPC interface (kernel/ipc).
 *
 * The substrate itself is fixed-core code: the endpoint table, the call
 * machinery, the descriptor table, the copy resolvers and the IRQ
 * trampoline all execute, and are touched, while the window shows bank 6
 * (architecture 36.5).  This header declares only what the cold side
 * (the syscall bodies) and the fixed assembly primitives expose.
 *
 * Cold callers run with the P-normalizing syscall gate's saved latch in
 * zos_gate_saved_latch (read through ipc_gate_latch, kernel/ipc/ipc_asm.asm);
 * every descriptor is captured from that latch, so the caller's bank and
 * page survive the gate's normalization.
 */

#ifndef ZEPHYROS_KERNEL_IPC_IPC_H
#define ZEPHYROS_KERNEL_IPC_IPC_H

#include "zos/ipc.h"
#include "zos/types.h"

#include "zephyros/ipc.h"

/* Endpoint lookup for the SYS_IPC_LOOKUP body and the service API table.
 * Returns the handle (1..N) or 0 when no endpoint matches; `version` is
 * filled on a match when non-null. */
int8_t ipc_lookup(uint8_t class, uint8_t instance, uint8_t *version);

/* SYS_IPC_CALL body: validate and dispatch the caller's block.  The block
 * is in the caller window (visible while the gate dispatches with P = 0);
 * the reply frame, status and descriptor capture land back in it.  The
 * return value is the IPC status (the syscall publishes it in HL with
 * carry clear); a structural problem in the block is reported to the
 * caller before this is reached. */
int16_t ipc_syscall_call(struct zos_ipc_call_args *args);

/* The service bootstrap table (include/zephyros/ipc.h) a service init
 * entry receives in HL.  The resident loader reads it before handing the
 * pointer to the upcall shim. */
const struct ipc_service_api *ipc_service_api(void);

/* Register the bulk extent of the image whose endpoints the loader is
 * about to register (architecture 36.6): the next register_ep call copies
 * the pair into the endpoint record, so bulk_read can validate the
 * calling service's own extent.  The loader calls this once per image
 * (0, 0 for an image with no bulk segment) before its endpoint pass. */
void ipc_service_bulk_set(uint16_t base, uint16_t size);

/* Synchronous call into a service from nucleus code (bank 7, no process
 * and no syscall gate): the IOC-FAT provider's class shim and the boot
 * console probe.  No descriptor is issued - only the syscall gate's latch
 * names a caller buffer - so a caller that has to move bytes uses the
 * staged form plus the common copy gate (architecture 36.8).  `frame` is
 * the caller's own 32-byte copy; the reply lands back in it. */
int16_t ipc_nucleus_call(uint8_t handle, struct zos_ipc_frame *frame);

#endif /* ZEPHYROS_KERNEL_IPC_IPC_H */
