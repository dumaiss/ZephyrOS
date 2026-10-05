#include "zlibc/string.h"

int memcmp(const void *a, const void *b, unsigned int n)
{
	const unsigned char *x = (const unsigned char *)a;
	const unsigned char *y = (const unsigned char *)b;

	while (n-- != 0u) {
		if (*x != *y) {
			return (int)*x - (int)*y;
		}
		++x;
		++y;
	}
	return 0;
}
