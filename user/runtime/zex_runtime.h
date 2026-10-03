/* ZephyrOS native program runtime (user/runtime, M6).
 *
 * C-facing wrappers over the stable register ABI documented in
 * docs/abi.md.  Programs link user/runtime/zex_runtime.asm and call these
 * functions; the SDCC calling conventions used here are private to the
 * program and its runtime and are never part of the ZephyrOS ABI.
 *
 * The runtime owns a bounded low-window scratch block (an `_DATA` object
 * placed by the standard ZEX link layout at the start of BSS, inside the
 * syscall argument window 0100h-1FFFh) so arbitrary C stacks near E000h
 * never violate the argument-block rule.
 *
 * Return conventions: a transferred byte count >= 0, or the negated
 * native error code (e.g. -ZOS_ENODEV) on failure.  `int` is 16-bit on
 * the Z80 target.  zex_exit() does not return.
 */

#ifndef ZEPHYROS_USER_ZEX_RUNTIME_H
#define ZEPHYROS_USER_ZEX_RUNTIME_H

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
 * count; zex_read() is a non-blocking drain (0 when no byte is
 * available); zex_poll() returns 0/1.  Errors are negative. */
int zex_write(const char *buf, unsigned int len);
int zex_read(char *buf, unsigned int len);
int zex_poll(void);

/* Fill `record` with the six-byte v1 information record; returns 6 or a
 * negative error. */
int zex_getinfo(struct zex_info *record);

#endif /* ZEPHYROS_USER_ZEX_RUNTIME_H */
