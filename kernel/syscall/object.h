/* KonaOS object/namespace syscall dispatcher interface (kernel-private).
 *
 * The stable numbers and packed argument blocks are the public native ABI
 * (abi/kos/object.h); this header is the private seam between the syscall
 * dispatcher (kernel/syscall.c) and the object and namespace bodies
 * (kernel/syscall/object.c).
 */

#ifndef KONAOS_KERNEL_SYSCALL_OBJECT_H
#define KONAOS_KERNEL_SYSCALL_OBJECT_H

#include "kos/types.h"

/* The object and namespace family bodies (06h-0Bh).  `block` is the raw
 * argument-block pointer from the register ABI. */
void kos_object_dispatch(uint8_t number, uint16_t block);

/* Copy one caller-window path into `out` (NUL-terminated, at most 127
 * bytes) with the shared validation rules.  The path is ordinary caller
 * data, so its bank follows the buffer-identity rule (an address behind
 * the mode-10 aperture names the far bank).  Returns 0, -KOS_EINVAL or
 * -KOS_EFAULT.  SYS_EXEC uses it so its path rules match `bind`'s
 * exactly. */
int kos_object_copy_path(uint16_t addr, uint16_t length, char *out);

#endif /* KONAOS_KERNEL_SYSCALL_OBJECT_H */
