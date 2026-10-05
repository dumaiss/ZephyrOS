/* ZephyrOS native diagnostic ABI (recovery-only syscall family).
 *
 * The M7 recovery facilities behind the native syscall gate.  They exist
 * for the native monitor and the diagnostic suite, are documented as
 * recovery-only in docs/abi.md, and are not a driver bypass: normal
 * programs keep to the ordinary services.  The numbers and packed blocks
 * here are the syscall-visible contract; the kernel bodies live in
 * kernel/syscall/diag.c.
 *
 * Every service validates its complete argument block before touching
 * caller memory or device state and leaves a rejected request
 * side-effect free.
 *
 *   SYS_DIAG_MEM_READ  28h {bank:u8, reserved:u8, address:u16}
 *       reads one byte from any application bank 0-6 at 0000h-DFFFh and
 *       returns it in HL.  `reserved` must be zero.
 *   SYS_DIAG_MEM_WRITE 29h {bank:u8, value:u8, address:u16}
 *       writes one byte in any application bank 0-6 at 0100h-DFFFh; the
 *       native page zero 0000h-00FFh and common memory are protected.
 *   SYS_DIAG_IO_IN     2Ah {port:u8}
 *       reads one byte from I/O port 10h-FFh and returns it in HL.
 *   SYS_DIAG_IO_OUT    2Bh {port:u8, value:u8}
 *       writes one byte to I/O port 10h-FFh.
 *   SYS_DIAG_BANK_INFO 2Ch no block
 *       returns HL = (ZOS_DIAG_BANK_MASK << 8) | current application
 *       bank.  Bit N of the mask is set when bank N is a valid
 *       application bank; bit 7 is always clear (bank 7 is kernel-only).
 *   SYS_DIAG_SYSTEM_INFO 2Dh result record at the argument pointer
 *       writes an eight-byte v1 record and returns 8 in HL.
 *
 * Ports 00h-0Fh are rejected with ZOS_EINVAL because the memory latch is
 * a bank/mode operation in disguise.  Every rejection is side-effect free
 * and every service restores the exact active latch.
 */

#ifndef ZOS_DIAG_H
#define ZOS_DIAG_H

#include "zos/types.h"

/* Diagnostic syscall numbers (native ABI, fixed by M7). */
#define ZOS_SYSCALL_DIAG_MEM_READ    0x28u /* 40 */
#define ZOS_SYSCALL_DIAG_MEM_WRITE   0x29u /* 41 */
#define ZOS_SYSCALL_DIAG_IO_IN       0x2Au /* 42 */
#define ZOS_SYSCALL_DIAG_IO_OUT      0x2Bu /* 43 */
#define ZOS_SYSCALL_DIAG_BANK_INFO   0x2Cu /* 44 */
#define ZOS_SYSCALL_DIAG_SYSTEM_INFO 0x2Du /* 45 */

#define ZOS_DIAG_MEM_BANK_MAX   6u      /* banks 0-6 are application banks */
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

#endif /* ZOS_DIAG_H */
