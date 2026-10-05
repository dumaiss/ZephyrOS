/* ZephyrOS native process-exec runtime (user/runtime, packages 28/28b).
 *
 * C wrapper over SYS_EXEC 05h.  Programs call zos_exec() instead of the
 * raw `RST 30h` ABI; it shares the low-window argument block and gate
 * stub of the filesystem runtime (user/runtime/zos_fs.c), so the string
 * length rule and the negated-errno convention are identical.
 *
 * `args` is the raw command tail: the kernel copies it byte-exact into
 * the child's startup block (0080h) and never splits or quotes it.  It
 * may be NULL or empty for "no arguments".  Parsing is user-space policy
 * (user/runtime/zex_args.h, docs/zex-format.md section 5).
 *
 * The child is an ordinary process: it inherits the caller's CWD, does
 * not inherit descriptors, and runs synchronously to SYS_EXIT.  The
 * return value is the child's u16 exit status (>= 0) or -errno.
 */

#ifndef ZEPHYROS_USER_ZEX_EXEC_RUNTIME_H
#define ZEPHYROS_USER_ZEX_EXEC_RUNTIME_H

/* SYS_EXEC shares the FS runtime's scratch block and gate stub. */
#include "zex_fs_runtime.h"

/* Largest raw tail the kernel transports (mirrors
 * include/zephyros/syscall.h ZOS_STARTUP_TAIL_MAX). */
#define ZEX_TAIL_MAX 125

/* Run `path` (NUL-terminated, 1..127 bytes) synchronously with the raw
 * argument tail `args` (NULL or "" for none; over 125 bytes is refused
 * locally with -ZOS_EINVAL).  Returns the child's exit status or -errno
 * (see docs/abi.md section 11 for the error mapping). */
int zos_exec(const char *path, const char *args);

#endif /* ZEPHYROS_USER_ZEX_EXEC_RUNTIME_H */
