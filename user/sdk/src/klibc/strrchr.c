#include "klibc/string.h"

char *strrchr(const char *s, int c)
{
	char ch = (char)c;
	char *last = (char *)0;

	for (;;) {
		if (*s == ch) {
			last = (char *)s;
		}
		if (*s == '\0') {
			return last;
		}
		++s;
	}
}
