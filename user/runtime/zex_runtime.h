/* ZephyrOS native program runtime (user/runtime, M6; M7 adds the
 * diagnostic wrappers declared below and implemented in
 * zex_diag_runtime.asm).
 *
 * C-facing wrappers over the stable register ABI documented in
 * docs/abi.md.  Programs link user/runtime/zex_runtime.asm and call these
 * functions; the SDCC calling conventions used here are private to the
 * program and its runtime and are never part of the ZephyrOS ABI.
 *
 * The runtime owns a bounded low-window scratch block (an `_DATA` object
 * placed by the standard ZEX link layout at the start of BSS, inside the
 * syscall argument window 0100h-1FFFh) so arbitrary C stacks near E000h
 * never violate the argument-block rule.  The diagnostic wrappers own a
 * second scratch block in the same window.
 *
 * Return conventions: a transferred byte count >= 0, or the negated
 * native error code (e.g. -ZOS_ENODEV) on failure.  `int` is 16-bit on
 * the Z80 target.  zex_exit() does not return.
 */

#ifndef ZEPHYROS_USER_ZEX_RUNTIME_H
#define ZEPHYROS_USER_ZEX_RUNTIME_H

/* User-side mirrors of the native error values used by runtime-local
 * checks and by the blocking console read (the kernel reports its own
 * codes). */
#define ZEX_EINVAL 0x02
#define ZEX_EAGAIN 0x09

/* Six-byte getinfo record, mirroring docs/abi.md and zephyros/syscall.h. */
#define ZEX_INFO_SIZE 6
#define ZEX_INFO_ABI  1
#define ZEX_INFO_FLAG_KERNEL_PARENT 0x0001

struct zex_info {
	unsigned char  size;  /* = ZEX_INFO_SIZE (6)              */
	unsigned char  abi;   /* = ZEX_INFO_ABI                   */
	unsigned char  pid;   /* process slot 0-6                 */
	unsigned char  bank;  /* application bank 0-6             */
	unsigned int   flags; /* ZEX_INFO_FLAG_*                  */
};

/* Terminate the process with `status`.  Releases the process bank/slot
 * and resumes the parent; control never returns. */
void zex_exit(unsigned int status);

/* Console services through the bound normal console device.
 * zex_write()/zex_read() transfer up to `len` bytes and return the
 * count.  zex_read() is a non-blocking drain: it returns 0 (or the
 * negated ZOS_EAGAIN from a driver that reports it directly) when no
 * byte is pending; zex_poll() returns 0/1.  Errors are negative. */
int zex_write(const char *buf, unsigned int len);
int zex_read(char *buf, unsigned int len);
int zex_poll(void);

/* Fill `record` with the six-byte v1 information record; returns 6 or a
 * negative error. */
int zex_getinfo(struct zex_info *record);

/* --- M7 diagnostic wrappers (zex_diag_runtime.asm) --------------------
 *
 * Recovery/diagnostic facilities behind the native syscall gate.  The
 * kernel validates bank/range/port and rejects bank 7, addresses
 * outside the documented windows, the reserved field, and the
 * latch-aliased ports 00h-0Fh; rejected requests are side-effect free.
 * Results follow the runtime convention: >= 0 on success (the read byte
 * or a written length), or -errno.
 */

/* Mirrors include/zephyros/syscall.h; the pairs are pinned by tests. */
#define ZEX_DIAG_BANK_MASK        0x7F
#define ZEX_DIAG_MEM_BANK_MAX     6
#define ZEX_DIAG_MEM_READ_MIN     0x0000
#define ZEX_DIAG_MEM_READ_MAX     0xDFFF
#define ZEX_DIAG_MEM_WRITE_MIN    0x0100
#define ZEX_DIAG_MEM_WRITE_MAX    0xDFFF
#define ZEX_DIAG_IO_PORT_MIN      0x10
#define ZEX_DIAG_IO_PORT_MAX      0xFF
#define ZEX_DIAG_SYSTEM_INFO_SIZE 8
#define ZEX_DIAG_SYSTEM_INFO_ABI  1

struct zex_diag_mem_read {
	unsigned char bank;     /* 0-6                                  */
	unsigned char reserved; /* must be 0                            */
	unsigned int  address;  /* 0000h-DFFFh                          */
};

struct zex_diag_mem_write {
	unsigned char bank;     /* 0-6                                  */
	unsigned char value;
	unsigned int  address;  /* 0100h-DFFFh                          */
};

struct zex_diag_io {
	unsigned char port;     /* 10h-FFh for IN; also used for OUT    */
	unsigned char value;    /* value for OUT                        */
};

struct zex_bank_info {
	unsigned char bank;     /* current application bank 0-6         */
	unsigned char mask;     /* bit N set = bank N is usable (7Fh)   */
};

struct zex_system_info {
	unsigned char  size;            /* = 8                          */
	unsigned char  abi;             /* = 1                          */
	unsigned char  app_mode;        /* application mapping mode     */
	unsigned char  os_mode;         /* OS mapping mode              */
	unsigned int   common_base;     /* permanent common RAM start   */
	unsigned char  kernel_bank;     /* kernel bank (7)              */
	unsigned char  romfs_first_page;/* first ROMFS recovery page    */
};

/* One byte in `address` of `bank` (read) / one byte written (write).
 * Read returns the byte; write returns 0. */
int zex_diag_mem_read(struct zex_diag_mem_read *req);
int zex_diag_mem_write(struct zex_diag_mem_write *req);

/* One byte from / to I/O `port`.  IN returns the byte; OUT returns 0. */
int zex_diag_io_in(struct zex_diag_io *req);
int zex_diag_io_out(struct zex_diag_io *req);

/* Write {bank, mask} to `out`; returns 2. */
int zex_diag_bank_info(struct zex_bank_info *out);

/* Write the eight-byte v1 record to `out`; returns 8. */
int zex_diag_system_info(struct zex_system_info *out);

#endif /* ZEPHYROS_USER_ZEX_RUNTIME_H */
