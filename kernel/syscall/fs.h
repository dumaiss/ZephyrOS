/* ZephyrOS M9 native filesystem syscall services (kernel-internal).
 *
 * The stable numbers and packed argument blocks live in
 * include/zephyros/syscall.h and include/zephyros/fs.h; this header is the
 * private contract between the M6/M7 dispatcher (kernel/syscall.c) and the
 * filesystem service bodies (kernel/syscall/fs.c).  It is not part of the
 * native ABI.
 *
 * zos_fs_dispatch() refuses every call with ZOS_EBUSY until the VFS READY
 * phase; each service validates its complete argument block before any
 * state change and publishes its result through the shared dispatcher
 * state.
 */

#ifndef ZEPHYROS_KERNEL_SYSCALL_FS_H
#define ZEPHYROS_KERNEL_SYSCALL_FS_H

#include "zephyros/types.h"

/* Dispatch one FS_* number (10h-1Fh).  `bank` is the caller's application
 * bank, `block` the raw argument-block pointer from the register ABI. */
void zos_fs_dispatch(uint8_t number, uint8_t bank, uint16_t block);

/* Copy one caller-window path into `out` (NUL-terminated, at most 127
 * bytes) with the shared validation rules.  Returns 0 or -ZOS_EINVAL.
 * Reused by SYS_EXEC so its path rules are identical to the FS_* family. */
int zos_fs_copy_path(uint16_t addr, uint16_t length, char *out);

#endif /* ZEPHYROS_KERNEL_SYSCALL_FS_H */
