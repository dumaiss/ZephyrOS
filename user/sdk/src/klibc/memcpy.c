#include "klibc/string.h"

void *memcpy(void *dst, const void *src, unsigned int n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	while (n-- != 0u) {
		*d++ = *s++;
	}
	return dst;
}
