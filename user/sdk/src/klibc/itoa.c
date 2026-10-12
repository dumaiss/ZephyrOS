#include "klibc/fmt.h"

void itoa(char *dst, int value, unsigned char base,
	      unsigned char width, char pad)
{
	static const char digits[] = "0123456789ABCDEF";
	char tmp[17];
	unsigned char n = 0;
	unsigned int mag;
	unsigned int total;
	unsigned int zeros = 0;
	int neg = value < 0;
	char *o = dst;

	if (base < 2u) {
		base = 10u;
	}
	mag = neg ? (unsigned int)(0u - (unsigned int)value)
		  : (unsigned int)value;
	do {
		tmp[n++] = digits[mag % base];
		mag = (unsigned int)(mag / base);
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
