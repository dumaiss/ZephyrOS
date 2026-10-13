/* KonaOS extended-memory syscall drivers (kernel-private interface).
 *
 * The stable numbers and packed argument blocks are the public native ABI
 * (abi/kos/xmem.h, abi/kos/seg.h); this header is the private seam between
 * the syscall dispatcher (kernel/syscall.c), the driver bodies
 * (kernel/syscall/xmem.c) and the process teardown that releases a pid's
 * objects (kernel/process/process.c).
 */

#ifndef KONAOS_KERNEL_SYSCALL_XMEM_H
#define KONAOS_KERNEL_SYSCALL_XMEM_H

#include "kos/types.h"
#include "kos/xmem.h"

/* The extended-memory family bodies (20h-27h).  `block` is the raw
 * argument-block pointer from the register ABI. */
void kos_xmem_dispatch(uint8_t number, uint16_t block);

/* Create a loader-owned segment object for `pid` (kernel/process/exec.c):
 * `size` is the segment's image + BSS, rounded up to whole slots.
 * `flags` is the container row's EXE_SEG_FLAG_* set.  A plain `DATA`
 * segment is placed non-mappable (it is reached by handle) and the caller
 * receives its handle plus the bank/slot base it copies into; a `FAR_CODE`
 * segment and a `DATA` segment that asked for `EXE_SEG_FLAG_MAPPABLE` are
 * placed at slot 4 (slots 4-5 when it is over 8 KiB: linked at 8000h, one
 * such segment per auxiliary bank), and the mappable data object keeps its
 * handle so SYS_XMEM_MAP can open the aperture onto it; a `FAR_CODE`
 * segment has no handle (SYS_SEG_MAP returns its token).  Returns 0,
 * -KOS_EINVAL for a bad request or -KOS_ENOSPC when no region or table
 * entry is free; nothing is registered on failure. */
int kos_xmem_segment_alloc(uint8_t pid, uint8_t id, uint8_t kind,
			   uint16_t flags, uint16_t size, uint16_t *handle,
			   uint8_t *bank, uint8_t *slot);

/* Release every object `pid` owns.  Called by process teardown after the
 * process's handles are unbound (architecture 36.13/36.15): exit releases
 * everything, and a suspended parent's objects stay untouched. */
void kos_xmem_release_process(uint8_t pid);

#endif /* KONAOS_KERNEL_SYSCALL_XMEM_H */
