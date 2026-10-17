#include "klibc/fmt.h"

/* Divide in place by `base`, return the remainder (divmod.asm). */
unsigned int kos_divmod16(unsigned int *value, unsigned int base);

void utoa(char *dst, unsigned int value, unsigned char base,
	      unsigned char width, char pad)
{
	static const char digits[] = "0123456789ABCDEF";
	char tmp[17];
	unsigned char n = 0;
	unsigned char i;

	if (base < 2u) {
		base = 10u;
	}
	do {
		tmp[n++] = digits[kos_divmod16(&value, base)];
	} while (value != 0u && n < sizeof(tmp));
	while (n < width) {
		tmp[n++] = pad;
	}
	for (i = 0; i < n; ++i) {
		dst[i] = tmp[n - 1u - i];
	}
	dst[n] = '\0';
}
