/* KonaOS libkos: idle, the wait for the next interrupt.
 *
 * A program waiting on input calls idle() between empty polls: it then
 * polls once per interrupt and the CPU is halted in between, which keeps
 * the bus activity of a polling loop out of the audio band (the sound
 * card picks it up).  A HALT needs an interrupt to end it: the one source
 * every system that has it keeps running is the timer driver's tick, so
 * idle() halts only where `/dev/timer` exists (asked once) and returns at
 * once elsewhere.
 */

#include "kos.h"

void kos_idle_halt(void);

static unsigned char idle_mode; /* 0 = not asked yet, 1 = halt, 2 = never */

void idle(void)
{
	if (idle_mode == 0u) {
		unsigned char type = 0u;
		int handle = bind("/dev/timer", 0u, &type);

		idle_mode = 2u;
		if (handle >= 0) {
			(void)unbind((unsigned char)handle);
			if (type == KOS_TYPE_TIMER) {
				idle_mode = 1u;
			}
		}
	}
	if (idle_mode == 1u) {
		kos_idle_halt();
	}
}
