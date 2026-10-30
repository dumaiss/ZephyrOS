/* Resident: the timer handler (it runs wherever the program was
 * interrupted, far code included) and a function called by pointer. */

#include "kos.h"
#include "place.h"

volatile uint16_t res_ticks;

void res_on_tick(unsigned int count, unsigned int missed)
{
	(void)missed;
	res_ticks = (uint16_t)(res_ticks + count);
}

uint16_t res_inc(uint16_t x)
{
	return (uint16_t)(x + 1u);
}
