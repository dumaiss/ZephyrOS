/* ZephyrOS native process-exec runtime (user/runtime, package 28).
 *
 * C wrapper over SYS_EXEC 05h.  Programs call zos_exec() instead of the
 * raw `RST 30h` ABI; it shares the low-window argument block and gate
 * stub of the filesystem runtime (user/runtime/zos_fs.c), so the string
 * length rule and the negated-errno convention are identical.
 *
 * The child is an ordinary process: it inherits the caller's CWD, does
 * not inherit descriptors, and runs synchronously to SYS_EXIT.  The
 * return value is the child's u16 exit status (>= 0) or -errno.
 */

#ifndef ZEPHYROS_USER_ZEX_EXEC_RUNTIME_H
#define ZEPHYROS_USER_ZEX_EXEC_RUNTIME_H

/* SYS_EXEC shares the FS runtime's scratch block and gate stub. */
#include "zex_fs_runtime.h"

/* Run `path` (NUL-terminated, 1..127 bytes) synchronously.  Returns the
 * child's exit status or -errno (see docs/abi.md section 11 for the error
 * mapping). */
int zos_exec(const char *path);

#endif /* ZEPHYROS_USER_ZEX_EXEC_RUNTIME_H */
