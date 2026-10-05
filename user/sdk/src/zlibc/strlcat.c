#include "zlibc/string.h"

unsigned int strlcat(char *dst, const char *src, unsigned int size)
{
	unsigned int dlen = 0;
	unsigned int srclen = 0;
	unsigned int i;

	while (dst[dlen] != '\0') {
		++dlen;
	}
	while (src[srclen] != '\0') {
		++srclen;
	}
	if (dlen >= size) {
		return dlen + srclen;
	}
	i = dlen;
	while (i + 1u < size && src[i - dlen] != '\0') {
		dst[i] = src[i - dlen];
		++i;
	}
	dst[i] = '\0';
	return dlen + srclen;
}
