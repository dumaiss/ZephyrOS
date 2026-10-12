/* ZephyrOS native syscall interface (public native ABI).
 *
 * The only stable interface is the documented register ABI in docs/abi.md:
 *
 *     RST 30h
 *     entry:  A  = syscall number
 *             HL = pointer to syscall-specific argument block
 *     return: carry clear = success, carry set = error
 *             HL = primary result
 *             A  = error code when carry is set
 *     preserved: BC, DE, IX, IY; SP and the selected bank are restored
 *     unspecified: other flags; alternate registers are not part of M1
 *
 * M6 implements five drivers: exit, console write/read/poll, getinfo.
 * M7 adds six recovery/diagnostic drivers behind the same gate; their
 * blocks are declared in abi/zos/diag.h.  Argument blocks must lie in the
 * caller window 0100h-1FFFh; console buffers may occupy 0100h-DFFFh and
 * are transferred through checked common-resident copy gates.  C names,
 * argument layouts, and SDCC calling conventions are private and must not
 * be exposed to syscall callers (user programs use the SDK, docs/sdk.md).
 *
 * This header owns the syscall numbers, the address windows and the
 * M6/M9 argument blocks.  The process startup block and getinfo record
 * are in abi/zos/process.h; the diagnostic blocks are in abi/zos/diag.h.
 */

#ifndef ZOS_SYSCALL_H
#define ZOS_SYSCALL_H

#include "zos/types.h"
#include "zos/errno.h"
#include "zos/process.h"

/* Native syscall numbers (stable from M6; object set from M10). */
#define ZOS_SYSCALL_EXIT          0x00u
/* 01h-03h were SYS_CONSOLE_WRITE / SYS_CONSOLE_READ / SYS_CONSOLE_POLL.
 * Package 41 removed them: a program's console I/O goes to the console
 * driver through the IPC pair below (user/sdk/src/console*.{c,asm}), so
 * the numbers stay reserved rather than reused - a stale caller fails
 * with ZOS_ENOSYS instead of writing somewhere unexpected. */
#define ZOS_SYSCALL_GETINFO       0x04u
#define ZOS_SYSCALL_EXEC          0x05u /* package 28 */

/* The M9 native filesystem family (10h-1Fh, `SYS_FS_*`) is retired
 * (package 42b): files are typed handles behind `bind`/`call`/`unbind`
 * (abi/zos/object.h), the SDK `fs.h` builds `open`/`close` on them, and
 * `chdir`/`getcwd` are the namespace syscalls 0Ah/0Bh.  The numbers stay
 * reserved so a stale caller fails with ZOS_ENOSYS instead of reaching
 * something unexpected. */

/* Address windows enforced before any caller byte is touched. */
#define ZOS_SYSCALL_WINDOW_BASE 0x0100u /* argument blocks begin here */
#define ZOS_SYSCALL_WINDOW_END  0x2000u /* exclusive; blocks are read in mode 11 */
#define ZOS_USER_BUFFER_BASE    0x0100u /* read/write buffer window   */
#define ZOS_USER_BUFFER_END     0xE000u /* exclusive; common begins   */

/* SYS_EXIT  {status:u16} - does not return; the child's resources are
 * released and the suspended parent resumes with this status.  A block
 * outside the caller window is treated as ZOS_EFAULT. */
struct zos_exit_args {
	uint16_t status;
};

/* SYS_EXEC 05h {path:u16, length:u16, args:u16, args_length:u16} - run a
 * child synchronously (packages 28/28b, docs/abi.md section 11).  `path`
 * addresses 1..127 non-NUL caller-window bytes resolved against the
 * caller's CWD; the rules are those of the namespace family.  `args`
 * addresses `args_length` raw bytes in the caller buffer window
 * `0100h-DFFFh`; the kernel copies them verbatim into the child's startup
 * block and never splits or quotes them.  `args_length` 0 means no
 * arguments and `args` is ignored; a length above `ZOS_STARTUP_TAIL_MAX`
 * (125) or a buffer outside the window is ZOS_EINVAL in the preflight,
 * before anything is allocated.  On success the child ran to exit and HL
 * holds its u16 exit status (carry clear), including a synthesized
 * ZOS_EFAULT from a raw entry return.  On failure carry is set with a
 * ZOS_E* code in A and nothing was allocated:
 *
 *   missing file            ZOS_ENOENT
 *   directory               ZOS_EISDIR
 *   malformed/oversize ZEX  ZOS_EINVAL
 *   bad argument block      ZOS_EINVAL
 *   no free slot or bank    ZOS_ENOSPC
 *   nesting/frame limit     ZOS_EBUSY
 *   provider read error     passed through unchanged
 *
 * Descriptors are not inherited. */
struct zos_exec_args {
	uint16_t path;
	uint16_t length;
	uint16_t args;        /* raw tail bytes, or 0 when args_length is 0 */
	uint16_t args_length; /* 0..ZOS_STARTUP_TAIL_MAX                     */
};

#if defined(__SDCC)
_Static_assert(sizeof(struct zos_exit_args) == 2,
	       "exit argument block must be 2 packed bytes");
_Static_assert(sizeof(struct zos_exec_args) == 8,
	       "exec argument block must be 8 packed bytes");
_Static_assert(__builtin_offsetof(struct zos_exec_args, path) == 0,
	       "exec path must be the first argument-block word");
_Static_assert(__builtin_offsetof(struct zos_exec_args, length) == 2,
	       "exec length must follow the path word");
_Static_assert(__builtin_offsetof(struct zos_exec_args, args) == 4,
	       "exec args pointer must follow the path length");
_Static_assert(__builtin_offsetof(struct zos_exec_args, args_length) == 6,
	       "exec args length must be the last argument-block word");
#endif

/* The M1 target self-test runner is declared by the test-owned seam
 * header tests/target/include/selftest.h, not by this ABI header. */

#endif /* ZOS_SYSCALL_H */
