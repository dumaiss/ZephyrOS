#include "zlibc/fmt.h"

void ultoa(char *dst, unsigned long value, unsigned char base,
	       unsigned char width, char pad)
{
	static const char digits[] = "0123456789ABCDEF";
	char tmp[33];
	unsigned char n = 0;
	unsigned char i;

	if (base < 2u) {
		base = 10u;
	}
	do {
		tmp[n++] = digits[(unsigned char)(value % base)];
		value = value / base;
	} while (value != 0u && n < sizeof(tmp));
	while (n < width) {
		tmp[n++] = pad;
	}
	for (i = 0; i < n; ++i) {
		dst[i] = tmp[n - 1u - i];
	}
	dst[n] = '\0';
}
