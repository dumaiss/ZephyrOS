/* KonaOS native syscall interface (public native ABI).
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
 *     unspecified: other flags and the alternate registers
 *
 * The recovery/diagnostic syscalls declare their blocks in the ABI's
 * kos/diag.h.  Argument blocks must lie in the caller window
 * 0100h-1FFFh; data buffers may occupy 0100h-DFFFh and are transferred
 * through checked copy gates.  C names,
 * argument layouts, and SDCC calling conventions are private and must not
 * be exposed to syscall callers (user programs use the SDK, docs/sdk.md).
 *
 * This header owns the syscall numbers, the address windows and the
 * process argument blocks.  The process startup block and getinfo record
 * are in the ABI's kos/process.h; the diagnostic blocks are in the ABI's
 * kos/diag.h.
 */

#ifndef KOS_SYSCALL_H
#define KOS_SYSCALL_H

#include "kos/types.h"
#include "kos/errno.h"
#include "kos/process.h"

/* Native syscall numbers.  01h-03h and 10h-1Fh are unassigned and return
 * KOS_ENOSYS.  Console I/O is the CONSOLE protocol on the standard
 * handles, and files are typed handles behind `bind`/`call`/`unbind`
 * (the ABI's kos/object.h); the SDK builds `open`/`close` on them. */
#define KOS_SYSCALL_EXIT          0x00u
#define KOS_SYSCALL_GETINFO       0x04u
#define KOS_SYSCALL_EXEC          0x05u

/* Address windows enforced before any caller byte is touched. */
#define KOS_SYSCALL_WINDOW_BASE 0x0100u /* argument blocks begin here */
#define KOS_SYSCALL_WINDOW_END  0x2000u /* exclusive; blocks are read in mode 11 */
#define KOS_USER_BUFFER_BASE    0x0100u /* read/write buffer window   */
#define KOS_USER_BUFFER_END     0xE000u /* exclusive; common begins   */

/* SYS_EXIT  {status:u16} - does not return; the child's resources are
 * released and the suspended parent resumes with this status.  A block
 * outside the caller window is treated as KOS_EFAULT. */
struct kos_exit_args {
	uint16_t status;
};

/* SYS_EXEC 05h {path:u16, length:u16, args:u16, args_length:u16} - run a
 * child synchronously (docs/abi.md section 5.3).  `path`
 * addresses 1..127 non-NUL caller-window bytes resolved against the
 * caller's CWD; the rules are those of the namespace family.  `args`
 * addresses `args_length` raw bytes in the caller buffer window
 * `0100h-DFFFh`; the kernel copies them verbatim into the child's startup
 * block and never splits or quotes them.  `args_length` 0 means no
 * arguments and `args` is ignored; a length above `KOS_STARTUP_TAIL_MAX`
 * (125) or a buffer outside the window is KOS_EINVAL in the preflight,
 * before anything is allocated.  On success the child ran to exit and HL
 * holds its u16 exit status (carry clear), including a synthesized
 * KOS_EFAULT from a raw entry return.  On failure carry is set with a
 * KOS_E* code in A and nothing was allocated:
 *
 *   missing file            KOS_ENOENT
 *   directory               KOS_EISDIR
 *   malformed/oversize EXE  KOS_EINVAL
 *   bad argument block      KOS_EINVAL
 *   no free slot or bank    KOS_ENOSPC
 *   nesting/frame limit     KOS_EBUSY
 *   provider read error     passed through unchanged
 *
 * Descriptors are not inherited. */
struct kos_exec_args {
	uint16_t path;
	uint16_t length;
	uint16_t args;        /* raw tail bytes, or 0 when args_length is 0 */
	uint16_t args_length; /* 0..KOS_STARTUP_TAIL_MAX                     */
};

#if defined(__SDCC)
_Static_assert(sizeof(struct kos_exit_args) == 2,
	       "exit argument block must be 2 packed bytes");
_Static_assert(sizeof(struct kos_exec_args) == 8,
	       "exec argument block must be 8 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_exec_args, path) == 0,
	       "exec path must be the first argument-block word");
_Static_assert(__builtin_offsetof(struct kos_exec_args, length) == 2,
	       "exec length must follow the path word");
_Static_assert(__builtin_offsetof(struct kos_exec_args, args) == 4,
	       "exec args pointer must follow the path length");
_Static_assert(__builtin_offsetof(struct kos_exec_args, args_length) == 6,
	       "exec args length must be the last argument-block word");
#endif

/* The target self-test runner is declared by the test-owned seam
 * header tests/target/include/selftest.h, not by this ABI header. */

#endif /* KOS_SYSCALL_H */
