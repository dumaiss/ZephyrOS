/* KRES host tests: the tiny check harness. */

#ifndef KRES_TEST_H
#define KRES_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kres_mem_io.h"

static int checks;
static int failures;

#define CHECK(cond)                                                      \
	do {                                                             \
		++checks;                                                \
		if (!(cond)) {                                           \
			++failures;                                      \
			fprintf(stderr, "%s:%d: check failed: %s\n",      \
				__FILE__, __LINE__, #cond);              \
		}                                                        \
	} while (0)

#define CHECK_EQ(a, b)                                                   \
	do {                                                             \
		long _a = (long)(a);                                     \
		long _b = (long)(b);                                     \
		++checks;                                                \
		if (_a != _b) {                                          \
			++failures;                                      \
			fprintf(stderr,                                    \
				"%s:%d: check failed: %s == %s (%ld vs %ld)\n", \
				__FILE__, __LINE__, #a, #b, _a, _b);       \
		}                                                        \
	} while (0)

#define TEST_DONE()                                                      \
	do {                                                             \
		if (failures != 0) {                                     \
			fprintf(stderr, "%d checks, %d failures\n",      \
				checks, failures);                       \
			return 1;                                        \
		}                                                        \
		printf("%s: OK (%d checks)\n", __FILE__, checks);         \
		return 0;                                                \
	} while (0)

#endif /* KRES_TEST_H */
