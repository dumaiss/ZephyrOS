#include "zlibc/string.h"

unsigned int strlcpy(char *dst, const char *src, unsigned int size)
{
	unsigned int i = 0;
	unsigned int srclen = 0;

	while (src[srclen] != '\0') {
		++srclen;
	}
	if (size != 0u) {
		while (i + 1u < size && src[i] != '\0') {
			dst[i] = src[i];
			++i;
		}
		dst[i] = '\0';
	}
	return srclen;
}
