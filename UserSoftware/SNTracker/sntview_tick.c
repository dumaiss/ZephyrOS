/* SN-Track's timer handler: the engine's tick.  It is the program's one
 * resident source (RESIDENT=, architecture 7.2): a handler runs wherever
 * the program was interrupted, so it and everything it reaches stay
 * visible while a far segment is mapped. */

#include "kos.h"
#include "kos/snd.h"
#include "sntview_tick.h"

volatile uint8_t ticking;
volatile uint32_t ticks_played;
volatile uint16_t missed_events;

void on_tick(unsigned int count, unsigned int missed)
{
	missed_events = (uint16_t)(missed_events + missed);
	if (count != 0u && ticking != 0u) {
		(void)snd_tick_ztr((uint16_t)count);
		ticks_played += count;
	}
}
