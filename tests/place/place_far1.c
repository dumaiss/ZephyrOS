/* Far segment 1: register and stack arguments, every result width, a
 * nested crossing (far 2, then home) and a wait the handler ends. */

#include "kos.h"
#include "place.h"

uint16_t f1_add(uint16_t a, uint16_t b)
{
	return (uint16_t)(a + b);
}

/* Five arguments: the last ones travel on the stack. */
uint32_t f1_mix(uint8_t a, uint16_t b, uint32_t c, uint8_t d, uint16_t e)
{
	return c + ((uint32_t)a << 24) + b + d + ((uint32_t)e << 8);
}

uint16_t f1_nested(uint16_t x)
{
	return (uint16_t)(f2_twice((uint8_t)x) + home_square(x));
}

/* While this segment is mapped, `events` timer events must be
 * delivered: the handler is resident. */
uint16_t f1_spin(uint16_t events)
{
	uint16_t start = res_ticks, rounds = 0u;
	while ((uint16_t)(res_ticks - start) < events && ++rounds < 2000u)
		idle();
	return (uint16_t)(res_ticks - start);
}
