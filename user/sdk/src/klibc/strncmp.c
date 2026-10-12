#include "klibc/string.h"

int strncmp(const char *a, const char *b, unsigned int n)
{
	while (n != 0u && *a != '\0' && *a == *b) {
		++a;
		++b;
		--n;
	}
	if (n == 0u) {
		return 0;
	}
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
