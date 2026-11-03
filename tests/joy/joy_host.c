/* Host harness for the SDK's latch decoders (user/sdk/src/joy_decode.c):
 * one line per latch byte 00h-FFh, `raw dirs key`, for tests/test_joy.py
 * to judge against the IO controller's table. */

#include <stdio.h>

#include "kos/joy.h"

int main(void)
{
	unsigned int raw;

	for (raw = 0u; raw < 256u; ++raw) {
		printf("%02x %02x %d\n", raw,
		       (unsigned int)joy_dirs((uint8_t)raw),
		       joy_key((uint8_t)raw));
	}
	return 0;
}
