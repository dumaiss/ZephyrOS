/* ZephyrOS extended-memory syscall services (kernel-private interface).
 *
 * The stable numbers and packed argument blocks are the public native ABI
 * (abi/zos/xmem.h); this header is the private seam between the syscall
 * dispatcher (kernel/syscall.c), the service bodies (kernel/syscall/xmem.c)
 * and the process teardown that releases a pid's objects
 * (kernel/process/process.c).
 */

#ifndef ZEPHYROS_KERNEL_SYSCALL_XMEM_H
#define ZEPHYROS_KERNEL_SYSCALL_XMEM_H

#include "zos/types.h"
#include "zos/xmem.h"

/* The extended-memory family bodies (20h-25h).  `block` is the raw
 * argument-block pointer from the register ABI. */
void zos_xmem_dispatch(uint8_t number, uint16_t block);

/* Release every object `pid` owns.  Called by process teardown after the
 * process's handles are unbound (architecture 36.13/36.15): exit releases
 * everything, and a suspended parent's objects stay untouched. */
void zos_xmem_release_process(uint8_t pid);

#endif /* ZEPHYROS_KERNEL_SYSCALL_XMEM_H */
