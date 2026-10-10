/* ZephyrOS object/namespace syscall dispatcher interface (kernel-private).
 *
 * The stable numbers and packed argument blocks are the public native ABI
 * (abi/zos/object.h); this header is the private seam between the syscall
 * dispatcher (kernel/syscall.c) and the object and namespace bodies
 * (kernel/syscall/object.c).
 */

#ifndef ZEPHYROS_KERNEL_SYSCALL_OBJECT_H
#define ZEPHYROS_KERNEL_SYSCALL_OBJECT_H

#include "zos/types.h"

/* The object and namespace family bodies (06h-0Bh).  `block` is the raw
 * argument-block pointer from the register ABI; `bank` is the caller's
 * execution bank (the namespace enumeration and `getcwd` write there). */
void zos_object_dispatch(uint8_t number, uint8_t bank, uint16_t block);

/* Copy one caller-window path into `out` (NUL-terminated, at most 127
 * bytes) with the shared validation rules; `bank` is the caller's
 * execution bank, which the staged read needs.  Returns 0,
 * -ZOS_EINVAL or -ZOS_EFAULT.  SYS_EXEC uses it so its path rules match
 * `bind`'s exactly. */
int zos_object_copy_path(uint8_t bank, uint16_t addr, uint16_t length,
			 char *out);

#endif /* ZEPHYROS_KERNEL_SYSCALL_OBJECT_H */
