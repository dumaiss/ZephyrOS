/* Far segment 2: an 8-bit result, and a call through a pointer to a
 * resident function. */

#include "kos.h"
#include "place.h"

uint8_t f2_twice(uint8_t x)
{
	return (uint8_t)(x << 1);
}

uint16_t f2_apply(uint16_t (*fn)(uint16_t), uint16_t x)
{
	return fn(fn(x));
}
