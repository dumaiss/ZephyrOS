#include "zlibc/string.h"

void *memset(void *dst, int c, unsigned int n)
{
	unsigned char *d = (unsigned char *)dst;
	unsigned char byte = (unsigned char)c;

	while (n-- != 0u) {
		*d++ = byte;
	}
	return dst;
}
