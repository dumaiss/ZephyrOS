#include "zlibc/fmt.h"

void zos_ltoa(char *dst, long value, unsigned char base,
	      unsigned char width, char pad)
{
	static const char digits[] = "0123456789ABCDEF";
	char tmp[33];
	unsigned char n = 0;
	unsigned long mag;
	unsigned int total;
	unsigned int zeros = 0;
	int neg = value < 0;
	char *o = dst;

	if (base < 2u) {
		base = 10u;
	}
	mag = neg ? (unsigned long)(0uL - (unsigned long)value)
		  : (unsigned long)value;
	do {
		tmp[n++] = digits[(unsigned char)(mag % base)];
		mag = mag / base;
	} while (mag != 0u && n < sizeof(tmp));
	total = (unsigned int)n + (neg ? 1u : 0u);
	if (pad != '0' && width > total) {
		unsigned int spaces = (unsigned int)width - total;

		while (spaces-- != 0u) {
			*o++ = ' ';
		}
	} else if (pad == '0' && width > total) {
		zeros = (unsigned int)width - total;
	}
	if (neg) {
		*o++ = '-';
	}
	while (zeros-- != 0u) {
		*o++ = '0';
	}
	while (n != 0u) {
		*o++ = tmp[--n];
	}
	*o = '\0';
}
