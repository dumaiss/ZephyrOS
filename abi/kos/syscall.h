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

/* Native syscall numbers.  01h-03h and 0Fh-1Fh are unassigned and return
 * KOS_ENOSYS.  Console I/O is the CONSOLE protocol on the standard
 * handles, and files are typed handles behind `bind`/`call`/`unbind`
 * (the ABI's kos/object.h); the SDK builds `open`/`close` on them. */
#define KOS_SYSCALL_EXIT          0x00u
#define KOS_SYSCALL_GETINFO       0x04u
#define KOS_SYSCALL_EXEC          0x05u
#define KOS_SYSCALL_REBOOT        0x0Cu
#define KOS_SYSCALL_NOTIFY        0x0Du
#define KOS_SYSCALL_EVENT_RETURN  0x0Eu

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

/* SYS_REBOOT 0Ch - warm reboot; it does not return and takes no argument
 * block.  The kernel masks interrupts, quiesces the interrupt sources, and
 * writes the reset latch (`00h`: mode 00, ROM page 0, bank 0) before
 * jumping to the ROM reset vector, so the reset copier re-copies the OS
 * exactly as at power-on and trusts nothing in RAM.  No peripheral sees a
 * hardware reset: every boot init programs its device from any state.
 * The block argument is ignored. */

/* SYS_NOTIFY 0Dh {handle:u8, reserved:u8, handler:u16, trampoline:u16} -
 * subscribe the calling process to the events of the device handle, or
 * unsubscribe when `handler` is 0.  `handle` names a bound handle whose
 * endpoint declared MOD_EP_EVENTS (`"events": true` in the driver's
 * manifest); any other handle is KOS_ENOTSUP.  The handler and the
 * trampoline are 16-bit addresses in the caller's own image (0100h up to
 * common memory), because both run as ordinary application code; the SDK's
 * `notify` passes its own libkos trampoline.  The nucleus assigns one
 * subscription per (process, handle) pair, tells the driver
 * KOS_OP_SUBSCRIBE with that id, and charges no table slot for a repeat
 * NOTIFY on a subscribed handle, which only replaces the handler.  On
 * success HL = 0 and the events arrive as described in docs/sdk.md;
 * failures are
 *
 *   bad block, reserved set, handler/trampoline outside the image  KOS_EINVAL
 *   not a bound handle of the calling process                       KOS_EINVAL
 *   the endpoint has no event support                               KOS_ENOTSUP
 *   no free subscription slot                                       KOS_ENOSPC
 *   the driver refused (its one source is taken)                    -its status
 *
 * and an unsubscribe of a handle that is not subscribed is KOS_ENOENT.
 * The subscription dies with the last handle bound to the object (unbind
 * or process exit); the driver hears KOS_OP_RELEASE for the handle as any
 * other release, which is what stops a running event source. */
struct kos_notify_args {
	uint8_t handle;
	uint8_t reserved; /* 0 */
	uint16_t handler; /* void (*)(unsigned count, unsigned missed) */
	uint16_t trampoline; /* the libkos entry the nucleus returns to */
};

/* SYS_EVENT_RETURN 0Eh - return from an event handler; takes no argument
 * block and does not return.  The libkos trampoline issues it after the
 * handler returned: the nucleus clears the "handler running" flag, hands
 * the next pending event to the same frame when there is one, and
 * otherwise unwinds the frame to the interrupted code.  Calling it without
 * a handler running is KOS_EINVAL. */

#if defined(__SDCC)
_Static_assert(sizeof(struct kos_exit_args) == 2,
	       "exit argument block must be 2 packed bytes");
_Static_assert(sizeof(struct kos_exec_args) == 8,
	       "exec argument block must be 8 packed bytes");
_Static_assert(sizeof(struct kos_notify_args) == 6,
	       "notify argument block must be 6 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_notify_args, handler) == 2,
	       "notify handler must follow the two byte header");
_Static_assert(__builtin_offsetof(struct kos_notify_args, trampoline) == 4,
	       "notify trampoline must be the last word");
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
