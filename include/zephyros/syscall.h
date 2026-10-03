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
 * M7 adds six recovery/diagnostic services (memory read/write, port
 * in/out, bank info, system info) behind the same gate.  Argument blocks
 * must lie in the caller window 0100h-1FFFh; console buffers may occupy
 * 0100h-DFFFh and are transferred through checked common-resident copy
 * gates.  C names, argument layouts, and SDCC calling conventions are
 * private and must not be exposed to syscall callers (user programs use
 * the user/runtime wrappers).
 */

#ifndef ZEPHYROS_SYSCALL_H
#define ZEPHYROS_SYSCALL_H

#include "zephyros/types.h"
#include "zephyros/errno.h"

/* Native syscall numbers (stable from M6; diagnostic set from M7). */
#define ZOS_SYSCALL_EXIT          0x00u
#define ZOS_SYSCALL_CONSOLE_WRITE 0x01u
#define ZOS_SYSCALL_CONSOLE_READ  0x02u
#define ZOS_SYSCALL_CONSOLE_POLL  0x03u
#define ZOS_SYSCALL_GETINFO       0x04u
#define ZOS_SYSCALL_DIAG_MEM_READ  0x28u /* 40 */
#define ZOS_SYSCALL_DIAG_MEM_WRITE 0x29u /* 41 */
#define ZOS_SYSCALL_DIAG_IO_IN     0x2Au /* 42 */
#define ZOS_SYSCALL_DIAG_IO_OUT    0x2Bu /* 43 */
#define ZOS_SYSCALL_DIAG_BANK_INFO 0x2Cu /* 44 */
#define ZOS_SYSCALL_DIAG_SYSTEM_INFO 0x2Du /* 45 */

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
 * returned in HL (success).  READ is a non-blocking drain: with no byte
 * pending it returns 0, and after the first byte it returns as soon as
 * no further byte is immediately available. */
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

/* --- M7 diagnostic services (recovery facilities) ---------------------
 *
 * These services exist for the native monitor and, later, the diagnostic
 * suite.  They are not a driver bypass: normal programs keep to the
 * ordinary services and never select bank 7 or touch the memory latch
 * through them.
 *
 * SYS_DIAG_MEM_READ  28h {bank:u8, reserved:u8, address:u16}
 *     reads one byte from any application bank 0-6 at 0000h-DFFFh and
 *     returns it in HL.  `reserved` must be zero.
 * SYS_DIAG_MEM_WRITE 29h {bank:u8, value:u8, address:u16}
 *     writes one byte in any application bank 0-6 at 0100h-DFFFh; the
 *     native page zero 0000h-00FFh and common memory are protected.
 * SYS_DIAG_IO_IN     2Ah {port:u8}
 *     reads one byte from I/O port 10h-FFh and returns it in HL.
 * SYS_DIAG_IO_OUT    2Bh {port:u8, value:u8}
 *     writes one byte to I/O port 10h-FFh.
 * SYS_DIAG_BANK_INFO 2Ch no block
 *     returns HL = (ZOS_DIAG_BANK_MASK << 8) | current application bank.
 *     Bit N of the mask is set when bank N is a valid application bank;
 *     bit 7 is always clear (bank 7 is kernel-only).
 * SYS_DIAG_SYSTEM_INFO 2Dh result record at the argument pointer
 *     writes an eight-byte v1 record and returns 8 in HL.
 *
 * Ports 00h-0Fh are rejected with ZOS_EINVAL because the memory latch
 * is aliased throughout that range: an "I/O" access there would be a
 * bank/mode operation in disguise.  Every rejection is side-effect free
 * and every service restores the exact active latch. */

#define ZOS_DIAG_MEM_BANK_MAX   6u     /* banks 0-6 are application banks */
#define ZOS_DIAG_MEM_READ_MIN   0x0000u /* page zero is readable          */
#define ZOS_DIAG_MEM_READ_MAX   0xDFFFu
#define ZOS_DIAG_MEM_WRITE_MIN  0x0100u /* native page zero is protected  */
#define ZOS_DIAG_MEM_WRITE_MAX  0xDFFFu
#define ZOS_DIAG_IO_PORT_MIN    0x10u  /* 00h-0Fh alias the latch        */
#define ZOS_DIAG_IO_PORT_MAX    0xFFu

/* Bitmask of valid application banks; bit 7 (bank 7) is never set. */
#define ZOS_DIAG_BANK_MASK      0x7Fu

struct zos_diag_mem_read_args {
	uint8_t  bank;     /* 0-6                                  */
	uint8_t  reserved; /* must be 0                            */
	uint16_t address;  /* 0000h-DFFFh; no wrap                 */
};

struct zos_diag_mem_write_args {
	uint8_t  bank;     /* 0-6                                  */
	uint8_t  value;    /* byte to store                        */
	uint16_t address;  /* 0100h-DFFFh; no wrap                 */
};

struct zos_diag_io_in_args {
	uint8_t  port;     /* 10h-FFh                              */
};

struct zos_diag_io_out_args {
	uint8_t  port;     /* 10h-FFh                              */
	uint8_t  value;    /* byte to write                        */
};

/* SYS_DIAG_SYSTEM_INFO v1 record. */
#define ZOS_DIAG_SYSTEM_INFO_SIZE 8u
#define ZOS_DIAG_SYSTEM_INFO_ABI  1u

struct zos_diag_system_info {
	uint8_t  size;        /* = 8                                 */
	uint8_t  abi;         /* = 1                                 */
	uint8_t  app_mode;    /* mode value of application mappings  */
	uint8_t  os_mode;     /* mode value of the OS mapping        */
	uint16_t common_base; /* first byte of permanent common RAM  */
	uint8_t  kernel_bank; /* bank holding the kernel (7)         */
	uint8_t  romfs_first_page; /* first ROMFS recovery page      */
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
_Static_assert(sizeof(struct zos_diag_mem_read_args) == 4,
	       "diag mem-read block must be 4 packed bytes");
_Static_assert(__builtin_offsetof(struct zos_diag_mem_read_args, address) == 2,
	       "diag mem-read address offset");
_Static_assert(sizeof(struct zos_diag_mem_write_args) == 4,
	       "diag mem-write block must be 4 packed bytes");
_Static_assert(__builtin_offsetof(struct zos_diag_mem_write_args, address) == 2,
	       "diag mem-write address offset");
_Static_assert(sizeof(struct zos_diag_io_in_args) == 1,
	       "diag io-in block must be 1 packed byte");
_Static_assert(sizeof(struct zos_diag_io_out_args) == 2,
	       "diag io-out block must be 2 packed bytes");
_Static_assert(sizeof(struct zos_diag_system_info) == ZOS_DIAG_SYSTEM_INFO_SIZE,
	       "diag system-info record must be 8 packed bytes");
_Static_assert(__builtin_offsetof(struct zos_diag_system_info,
				  common_base) == 4,
	       "diag system-info common_base offset");
_Static_assert(__builtin_offsetof(struct zos_diag_system_info,
				  kernel_bank) == 6,
	       "diag system-info kernel_bank offset");
_Static_assert(__builtin_offsetof(struct zos_diag_system_info,
				  romfs_first_page) == 7,
	       "diag system-info romfs_first_page offset");
#endif

#if ZOS_SYSCALL_SELFTEST
/* Internal target-side self-test driver (build option only, not ABI). */
void zos_run_syscall_selftest(void);
#endif

#endif /* ZEPHYROS_SYSCALL_H */
