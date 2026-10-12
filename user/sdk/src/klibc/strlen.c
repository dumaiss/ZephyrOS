#include "zlibc/string.h"

unsigned int strlen(const char *s)
{
	unsigned int n = 0;

	while (s[n] != '\0') {
		++n;
	}
	return n;
}
