#ifndef SNTRACK_VIEW_TICK_H
#define SNTRACK_VIEW_TICK_H

#include "kos/types.h"

/* The engine is ticked while `ticking`; the ticks it processed and the
 * events the nucleus dropped, for the time display and the report. */
extern volatile uint8_t ticking;
extern volatile uint32_t ticks_played;
extern volatile uint16_t missed_events;

void on_tick(unsigned int count, unsigned int missed);

#endif
