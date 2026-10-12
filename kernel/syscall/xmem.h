/* ZephyrOS extended-memory syscall drivers (kernel-private interface).
 *
 * The stable numbers and packed argument blocks are the public native ABI
 * (abi/zos/xmem.h, abi/zos/seg.h); this header is the private seam between
 * the syscall dispatcher (kernel/syscall.c), the driver bodies
 * (kernel/syscall/xmem.c) and the process teardown that releases a pid's
 * objects (kernel/process/process.c).
 */

#ifndef ZEPHYROS_KERNEL_SYSCALL_XMEM_H
#define ZEPHYROS_KERNEL_SYSCALL_XMEM_H

#include "zos/types.h"
#include "zos/xmem.h"

/* The extended-memory family bodies (20h-27h).  `block` is the raw
 * argument-block pointer from the register ABI. */
void zos_xmem_dispatch(uint8_t number, uint16_t block);

/* Create a loader-owned segment object for `pid` (kernel/process/exec.c):
 * `size` is the segment's image + BSS, rounded up to whole slots.  A
 * `DATA` segment is placed non-mappable (it is reached by handle) and the
 * caller receives its handle plus the bank/slot base it copies into; a
 * `FAR_CODE` segment is placed at slot 4 (slots 4-5 when it is over
 * 8 KiB: linked at 8000h, one far-code segment per auxiliary bank) and
 * has no handle (SYS_SEG_MAP returns its token).  Returns 0, -ZOS_EINVAL
 * for a bad request or -ZOS_ENOSPC when no region or table entry is free;
 * nothing is registered on failure. */
int zos_xmem_segment_alloc(uint8_t pid, uint8_t id, uint8_t kind,
			   uint16_t size, uint16_t *handle, uint8_t *bank,
			   uint8_t *slot);

/* Release every object `pid` owns.  Called by process teardown after the
 * process's handles are unbound (architecture 36.13/36.15): exit releases
 * everything, and a suspended parent's objects stay untouched. */
void zos_xmem_release_process(uint8_t pid);

#endif /* ZEPHYROS_KERNEL_SYSCALL_XMEM_H */
