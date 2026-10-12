/* KonaOS klibc: the minimal freestanding string/memory subset.
 *
 * No hosted C library is linked (--nostdinc); every function here has
 * its own libkos archive member so a program links only what it uses.
 */

#ifndef KLIBC_STRING_H
#define KLIBC_STRING_H

#include "kos/types.h"

unsigned int strlen(const char *s);
int          strcmp(const char *a, const char *b);
int          strncmp(const char *a, const char *b, unsigned int n);
char        *strchr(const char *s, int c);
char        *strrchr(const char *s, int c);

void        *memcpy(void *dst, const void *src, unsigned int n);
void        *memmove(void *dst, const void *src, unsigned int n);
void        *memset(void *dst, int c, unsigned int n);
int          memcmp(const void *a, const void *b, unsigned int n);

/* Bounded copy/concatenation: always NUL-terminate when `size` > 0;
 * return the length the result would have had (excluding the NUL), so
 * truncation is detectable by comparing with `size`. */
unsigned int strlcpy(char *dst, const char *src, unsigned int size);
unsigned int strlcat(char *dst, const char *src, unsigned int size);

#endif /* KLIBC_STRING_H */
