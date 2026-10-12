/* KonaOS klibc: SDCC signed integer-arithmetic support.
 *
 * The signed counterparts of sdcc_arith.c; a separate member so a
 * program that never divides a signed value does not link them.  They
 * reuse the unsigned shift/subtract primitives.
 */

#include "kos/types.h"

unsigned int  kos_divuint(unsigned int a, unsigned int b);
unsigned int  kos_moduint(unsigned int a, unsigned int b);
unsigned long kos_divulong(unsigned long a, unsigned long b);
unsigned long kos_modulong(unsigned long a, unsigned long b);

int kos_divsint(int a, int b)
{
	unsigned int ua;
	unsigned int ub;
	unsigned int q;
	int neg;

	if (b == 0) {
		return 0;
	}
	neg = ((a < 0) != (b < 0));
	ua = (a < 0) ? (unsigned int)(0u - (unsigned int)a) : (unsigned int)a;
	ub = (b < 0) ? (unsigned int)(0u - (unsigned int)b) : (unsigned int)b;
	q = kos_divuint(ua, ub);
	return neg ? (int)(0u - q) : (int)q;
}

int kos_modsint(int a, int b)
{
	unsigned int ua;
	unsigned int ub;
	unsigned int r;

	if (b == 0) {
		return 0;
	}
	ua = (a < 0) ? (unsigned int)(0u - (unsigned int)a) : (unsigned int)a;
	ub = (b < 0) ? (unsigned int)(0u - (unsigned int)b) : (unsigned int)b;
	r = kos_moduint(ua, ub);
	return (a < 0) ? (int)(0u - r) : (int)r;
}

long kos_divslong(long a, long b)
{
	unsigned long ua;
	unsigned long ub;
	unsigned long q;
	int neg;

	if (b == 0L) {
		return 0L;
	}
	neg = ((a < 0L) != (b < 0L));
	ua = (a < 0L) ? (unsigned long)(0uL - (unsigned long)a)
		      : (unsigned long)a;
	ub = (b < 0L) ? (unsigned long)(0uL - (unsigned long)b)
		      : (unsigned long)b;
	q = kos_divulong(ua, ub);
	return neg ? (long)(0uL - q) : (long)q;
}

long kos_modslong(long a, long b)
{
	unsigned long ua;
	unsigned long ub;
	unsigned long r;

	if (b == 0L) {
		return 0L;
	}
	ua = (a < 0L) ? (unsigned long)(0uL - (unsigned long)a)
		      : (unsigned long)a;
	ub = (b < 0L) ? (unsigned long)(0uL - (unsigned long)b)
		      : (unsigned long)b;
	r = kos_modulong(ua, ub);
	return (a < 0L) ? (long)(0uL - r) : (long)r;
}
