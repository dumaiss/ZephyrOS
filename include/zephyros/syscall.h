/* ZephyrOS native syscall interface.
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
 * M6 implements five services: exit, console write/read/poll, getinfo.
 * Argument blocks must lie in the caller window 0100h-1FFFh; console
 * buffers may occupy 0100h-DFFFh and are transferred through checked
 * common-resident copy gates.  C names, argument layouts, and SDCC
 * calling conventions are private and must not be exposed to syscall
 * callers (user programs use the user/runtime wrappers).
 */

#ifndef ZEPHYROS_SYSCALL_H
#define ZEPHYROS_SYSCALL_H

#include "zephyros/types.h"
#include "zephyros/errno.h"

/* Native syscall numbers (stable from M6). */
#define ZOS_SYSCALL_EXIT          0x00u
#define ZOS_SYSCALL_CONSOLE_WRITE 0x01u
#define ZOS_SYSCALL_CONSOLE_READ  0x02u
#define ZOS_SYSCALL_CONSOLE_POLL  0x03u
#define ZOS_SYSCALL_GETINFO       0x04u

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

/* SYS_CONSOLE_WRITE / SYS_CONSOLE_READ  {buffer:u16, length:u16}.
 * `buffer` is a logical address in the caller's bank; the kernel stages
 * the transfer through a bounded common buffer.  The transfer count is
 * returned in HL (success).  READ blocks for the first byte, then
 * returns as soon as no further byte is immediately available. */
struct zos_console_args {
	uint16_t buffer;
	uint16_t length;
};

/* SYS_CONSOLE_POLL takes no block; HL returns 0 (no input) or 1. */

/* SYS_GETINFO writes a six-byte v1 record at the argument block and
 * returns 6 in HL.  Record flags: bit 0 is set when the parent is the
 * kernel launcher (no user parent process).  The value is shared with
 * PROCESS_FLAG_KERNEL_PARENT in process.h; kernel/syscall.c pins it. */
#define ZOS_GETINFO_SIZE 6u
#define ZOS_GETINFO_ABI  1u
#define PROCESS_INFO_FLAG_KERNEL_PARENT 0x0001u

struct zos_info_record {
	uint8_t  size;  /* = ZOS_GETINFO_SIZE */
	uint8_t  abi;   /* = ZOS_GETINFO_ABI  */
	uint8_t  pid;   /* process slot 0-6   */
	uint8_t  bank;  /* application bank 0-6 */
	uint16_t flags; /* PROCESS_INFO_FLAG_* */
};

#if defined(__SDCC)
_Static_assert(sizeof(struct zos_exit_args) == 2,
	       "exit argument block must be 2 packed bytes");
_Static_assert(sizeof(struct zos_console_args) == 4,
	       "console argument block must be 4 packed bytes");
_Static_assert(sizeof(struct zos_info_record) == ZOS_GETINFO_SIZE,
	       "getinfo record must be 6 packed bytes");
_Static_assert(__builtin_offsetof(struct zos_info_record, flags) == 4,
	       "getinfo flags offset");
#endif

#if ZOS_SYSCALL_SELFTEST
/* Internal target-side self-test driver (build option only, not ABI). */
void zos_run_syscall_selftest(void);
#endif

#endif /* ZEPHYROS_SYSCALL_H */
