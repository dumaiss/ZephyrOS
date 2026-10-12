/* KonaOS klibc: SDCC integer-arithmetic support.
 *
 * SDCC lowers the / and % operators to calls to the compiler helpers
 * __divuint/__moduint/__divulong/__modulong (unsigned) and
 * __divsint/__modsint/__divslong/__modslong (signed).  This member
 * implements the algorithms; sdcc_arith.asm exports the helper symbols
 * as one-jump stubs, because a C function whose source name starts with
 * an underscore would be placed in SDCC's _HOME area instead of _CODE.
 *
 * The bodies are shift/subtract long divisions that never use / or %, so
 * they need no other runtime.  A program that does not divide never links
 * this member.
 */

#include "kos/types.h"

unsigned int  kos_divuint(unsigned int a, unsigned int b);
unsigned int  kos_moduint(unsigned int a, unsigned int b);
unsigned long kos_divulong(unsigned long a, unsigned long b);
unsigned long kos_modulong(unsigned long a, unsigned long b);

unsigned int kos_divuint(unsigned int a, unsigned int b)
{
	unsigned int q = 0;
	unsigned int r = 0;
	unsigned char i;

	if (b == 0u) {
		return 0u;
	}
	for (i = 0; i < 16u; ++i) {
		r = (unsigned int)((r << 1) | (a >> 15));
		a = (unsigned int)(a << 1);
		q = (unsigned int)(q << 1);
		if (r >= b) {
			r = (unsigned int)(r - b);
			q = (unsigned int)(q | 1u);
		}
	}
	return q;
}

unsigned int kos_moduint(unsigned int a, unsigned int b)
{
	unsigned int r = 0;
	unsigned char i;

	if (b == 0u) {
		return 0u;
	}
	for (i = 0; i < 16u; ++i) {
		r = (unsigned int)((r << 1) | (a >> 15));
		a = (unsigned int)(a << 1);
		if (r >= b) {
			r = (unsigned int)(r - b);
		}
	}
	return r;
}

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
