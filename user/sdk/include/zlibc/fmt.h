/* ZephyrOS zlibc: number formatting and a small printf.
 *
 * The `*oa` helpers format into `dst` (NUL-terminated) in `base` 10 or
 * 16 (2/8 also work) with a minimum `width` and a `pad` character ('0'
 * for zero-pad, ' ' otherwise).  printf() writes through the console
 * to the console; snprintf() writes into a bounded buffer.
 *
 * Supported conversions: %s %c %d %u %x %ld %lu %lx %% plus an optional
 * '-'-less width and a '0' zero-pad flag (e.g. %05d, %4s).
 */

#ifndef ZLIBC_FMT_H
#define ZLIBC_FMT_H

#include "zos/types.h"

void utoa(char *dst, unsigned int value, unsigned char base,
	      unsigned char width, char pad);
void ultoa(char *dst, unsigned long value, unsigned char base,
	       unsigned char width, char pad);
void itoa(char *dst, int value, unsigned char base,
	      unsigned char width, char pad);
void ltoa(char *dst, long value, unsigned char base,
	      unsigned char width, char pad);

int printf(const char *fmt, ...);
int snprintf(char *dst, unsigned int size, const char *fmt, ...);

#endif /* ZLIBC_FMT_H */
