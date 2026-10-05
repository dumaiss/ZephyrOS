#include "zlibc/string.h"

void *memmove(void *dst, const void *src, unsigned int n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	if (d == s || n == 0u) {
		return dst;
	}
	if (d < s) {
		while (n-- != 0u) {
			*d++ = *s++;
		}
	} else {
		d += n;
		s += n;
		while (n-- != 0u) {
			*--d = *--s;
		}
	}
	return dst;
}
