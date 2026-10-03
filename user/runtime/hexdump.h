/* ZephyrOS user runtime: 16-byte hexdump row formatting.
 *
 * The row format matches the familiar Zephyr-80 monitor output:
 *
 *     AAAA: HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH  ASCII
 *
 * with three spaces reserved for each byte missing from a short final
 * row (so the ASCII column stays aligned) and '.' for bytes outside
 * 20h-7Eh.  Formatting only: callers supply the bytes they obtained
 * through whatever read path is appropriate (for diagnostic programs,
 * SYS_DIAG_MEM_READ).
 */

#ifndef ZEPHYROS_USER_HEXDUMP_H
#define ZEPHYROS_USER_HEXDUMP_H

/* Worst case: 6 + 16*3 + 1 + 16 + 1 (NUL) = 72 bytes. */
#define ZRT_HEXDUMP_ROW_MAX 72

/* Format one row of up to 16 bytes into `dst` (NUL-terminated). */
void zrt_hexdump_row(char *dst, unsigned int addr,
		     const unsigned char *bytes, unsigned int count);

#endif /* ZEPHYROS_USER_HEXDUMP_H */
