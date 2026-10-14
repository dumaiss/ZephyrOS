/* KonaOS native diagnostic ABI (recovery-only syscall family).
 *
 * The recovery facilities behind the native syscall gate.  They exist
 * for the native monitor and the diagnostic suite, are documented as
 * recovery-only in docs/abi.md, and are not a driver bypass: normal
 * programs keep to the ordinary drivers.  The numbers and packed blocks
 * here are the syscall-visible contract; the kernel bodies live in
 * kernel/syscall/diag.c.
 *
 * Every driver validates its complete argument block before touching
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
 *       returns HL = (KOS_DIAG_BANK_MASK << 8) | current application
 *       bank.  Bit N of the mask is set when bank N is a valid
 *       application bank; bit 7 is always clear (bank 7 is kernel-only).
 *   SYS_DIAG_SYSTEM_INFO 2Dh result record at the argument pointer
 *       writes the v1 record below and returns its size (13) in HL.
 *
 * Ports 00h-0Fh are rejected with KOS_EINVAL because the memory latch is
 * a bank/mode operation in disguise.  Every rejection is side-effect free
 * and every driver restores the exact active latch.
 */

#ifndef KOS_DIAG_H
#define KOS_DIAG_H

#include "kos/types.h"

/* Diagnostic syscall numbers (native ABI). */
#define KOS_SYSCALL_DIAG_MEM_READ    0x28u /* 40 */
#define KOS_SYSCALL_DIAG_MEM_WRITE   0x29u /* 41 */
#define KOS_SYSCALL_DIAG_IO_IN       0x2Au /* 42 */
#define KOS_SYSCALL_DIAG_IO_OUT      0x2Bu /* 43 */
#define KOS_SYSCALL_DIAG_BANK_INFO   0x2Cu /* 44 */
#define KOS_SYSCALL_DIAG_SYSTEM_INFO 0x2Du /* 45 */

#define KOS_DIAG_MEM_BANK_MAX   6u      /* banks 0-6 are application banks */
#define KOS_DIAG_MEM_READ_MIN   0x0000u /* page zero is readable          */
#define KOS_DIAG_MEM_READ_MAX   0xDFFFu
#define KOS_DIAG_MEM_WRITE_MIN  0x0100u /* native page zero is protected  */
#define KOS_DIAG_MEM_WRITE_MAX  0xDFFFu
#define KOS_DIAG_IO_PORT_MIN    0x10u  /* 00h-0Fh alias the latch        */
#define KOS_DIAG_IO_PORT_MAX    0xFFu

/* Bitmask of valid application banks; bit 7 (bank 7) is never set. */
#define KOS_DIAG_BANK_MASK      0x7Fu

struct kos_diag_mem_read_args {
	uint8_t  bank;     /* 0-6                                  */
	uint8_t  reserved; /* must be 0                            */
	uint16_t address;  /* 0000h-DFFFh; no wrap                 */
};

struct kos_diag_mem_write_args {
	uint8_t  bank;     /* 0-6                                  */
	uint8_t  value;    /* byte to store                        */
	uint16_t address;  /* 0100h-DFFFh; no wrap                 */
};

struct kos_diag_io_in_args {
	uint8_t  port;     /* 10h-FFh                              */
};

struct kos_diag_io_out_args {
	uint8_t  port;     /* 10h-FFh                              */
	uint8_t  value;    /* byte to write                        */
};

/* SYS_DIAG_SYSTEM_INFO v1 record.  The platform constants come first;
 * the interrupt executive's dispatch counters follow them, because they
 * are what a board run needs when a source's handler does not run: a
 * driver that programs the wrong CTC port (the board's select wiring
 * crosses A0 and A1, so channel 2 answers 41h) has its ticks vector
 * through another channel's entry, which the executive counts as an
 * unclaimed - or, for an entry it holds no stub for, an unexpected -
 * dispatch while the driver's own count stays at zero.  The record grew
 * from eight to thirteen bytes with them; `size` is the layout's tag,
 * and the ABI number is unchanged. */
#define KOS_DIAG_SYSTEM_INFO_SIZE 13u
#define KOS_DIAG_SYSTEM_INFO_ABI  1u

struct kos_diag_system_info {
	uint8_t  size;        /* = 13                                */
	uint8_t  abi;         /* = 1                                 */
	uint8_t  app_mode;    /* mode value of application mappings  */
	uint8_t  os_mode;     /* mode value of the OS mapping        */
	uint16_t common_base; /* first byte of permanent common RAM  */
	uint8_t  kernel_bank; /* bank holding the kernel (7)         */
	uint8_t  romfs_first_page; /* first ROMFS recovery page      */
	uint16_t irq_unexpected;   /* dispatches with no source      */
	uint16_t irq_unclaimed;    /* dispatches with no handler     */
	uint8_t  irq_last_source;  /* last source the gate dispatched */
	                           /* (IRQ_SOURCE_COUNT before any)  */
};

#if defined(__SDCC)
_Static_assert(sizeof(struct kos_diag_mem_read_args) == 4,
	       "diag mem-read block must be 4 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_diag_mem_read_args, address) == 2,
	       "diag mem-read address offset");
_Static_assert(sizeof(struct kos_diag_mem_write_args) == 4,
	       "diag mem-write block must be 4 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_diag_mem_write_args, address) == 2,
	       "diag mem-write address offset");
_Static_assert(sizeof(struct kos_diag_io_in_args) == 1,
	       "diag io-in block must be 1 packed byte");
_Static_assert(sizeof(struct kos_diag_io_out_args) == 2,
	       "diag io-out block must be 2 packed bytes");
_Static_assert(sizeof(struct kos_diag_system_info) == KOS_DIAG_SYSTEM_INFO_SIZE,
	       "diag system-info record must be 8 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_diag_system_info,
				  common_base) == 4,
	       "diag system-info common_base offset");
_Static_assert(__builtin_offsetof(struct kos_diag_system_info,
				  kernel_bank) == 6,
	       "diag system-info kernel_bank offset");
_Static_assert(__builtin_offsetof(struct kos_diag_system_info,
				  romfs_first_page) == 7,
	       "diag system-info romfs_first_page offset");
_Static_assert(__builtin_offsetof(struct kos_diag_system_info,
				  irq_unexpected) == 8,
	       "diag system-info irq_unexpected offset");
_Static_assert(__builtin_offsetof(struct kos_diag_system_info,
				  irq_unclaimed) == 10,
	       "diag system-info irq_unclaimed offset");
_Static_assert(__builtin_offsetof(struct kos_diag_system_info,
				  irq_last_source) == 12,
	       "diag system-info irq_last_source offset");
#endif

#endif /* KOS_DIAG_H */
