#ifndef SC2KOS_HOST_KOS_H
#define SC2KOS_HOST_KOS_H

#include <stdio.h>
#include <string.h>

static inline unsigned int strlcpy(char *dst, const char *src,
				   unsigned int size)
{
	unsigned int length = (unsigned int)strlen(src);
	unsigned int count = length;

	if (size != 0u) {
		if (count >= size) { count = size - 1u; }
		memcpy(dst, src, count);
		dst[count] = '\0';
	}
	return length;
}

static inline void sc_host_ultoa(char *dst, unsigned long value,
				 unsigned char base, unsigned char width, char pad)
{
	char reverse[34];
	unsigned int count = 0u;
	unsigned int at = 0u;

	do {
		unsigned int digit = (unsigned int)(value % base);
		reverse[count++] = (char)(digit < 10u ? '0' + digit : 'A' + digit - 10u);
		value /= base;
	} while (value != 0uL);
	while (count < width) { reverse[count++] = pad; }
	while (count != 0u) { dst[at++] = reverse[--count]; }
	dst[at] = '\0';
}

static inline void utoa(char *dst, unsigned int value, unsigned char base,
			unsigned char width, char pad)
{
	sc_host_ultoa(dst, value, base, width, pad);
}

static inline void ultoa(char *dst, unsigned long value, unsigned char base,
			 unsigned char width, char pad)
{
	sc_host_ultoa(dst, value, base, width, pad);
}

#endif
