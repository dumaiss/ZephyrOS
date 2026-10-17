/* KonaOS klibc: SDCC integer-arithmetic support.
 *
 * SDCC lowers the / and % operators to calls to the compiler helpers
 * __divuint/__moduint/__divulong/__modulong (unsigned) and
 * __divsint/__modsint/__divslong/__modslong (signed).  The 16-bit
 * unsigned pair is assembly (sdcc_arith16.asm); this member implements
 * the 32-bit pair; sdcc_arith_entries.asm exports the helper symbols
 * as one-jump stubs, because a C function whose source name starts with
 * an underscore would be placed in SDCC's _HOME area instead of _CODE.
 *
 * The bodies are shift/subtract long divisions that never use / or %, so
 * they need no other runtime.  A program that does not divide never links
 * this member.
 */

#include "kos/types.h"

unsigned long kos_divulong(unsigned long a, unsigned long b);
unsigned long kos_modulong(unsigned long a, unsigned long b);

unsigned long kos_divulong(unsigned long a, unsigned long b)
{
	unsigned long q = 0;
	unsigned long r = 0;
	unsigned char i;

	if (b == 0uL) {
		return 0uL;
	}
	for (i = 0; i < 32u; ++i) {
		r = (unsigned long)((r << 1) | (a >> 31));
		a = (unsigned long)(a << 1);
		q = (unsigned long)(q << 1);
		if (r >= b) {
			r = (unsigned long)(r - b);
			q = (unsigned long)(q | 1uL);
		}
	}
	return q;
}

unsigned long kos_modulong(unsigned long a, unsigned long b)
{
	unsigned long r = 0;
	unsigned char i;

	if (b == 0uL) {
		return 0uL;
	}
	for (i = 0; i < 32u; ++i) {
		r = (unsigned long)((r << 1) | (a >> 31));
		a = (unsigned long)(a << 1);
		if (r >= b) {
			r = (unsigned long)(r - b);
		}
	}
	return r;
}
