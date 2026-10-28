/* KonaOS libkos: an input wait's pacing.
 *
 * The console poll behind an empty read is itself bus activity, a burst
 * at every wake - the timer tick, 156.25 Hz - squarely in the audio band
 * (the sound card picks it up).  While input is idle, idle_input() halts
 * through KOS_IDLE_INPUT_WAKES interrupts before the next poll, about 10
 * polls a second on the tick; within KOS_IDLE_INPUT_WINDOW wakes after
 * input (about a second) it halts once, so typing keeps the per-tick
 * response.  Keys wait in the IO controller's queue meanwhile and are
 * never lost.
 */

#include "kos.h"

#define KOS_IDLE_INPUT_WAKES  16u
#define KOS_IDLE_INPUT_WINDOW 156u

static unsigned char idle_input_fast; /* wakes left at the per-tick rate */

void idle_input(void)
{
	unsigned char wakes;

	if (idle_input_fast != 0u) {
		--idle_input_fast;
		idle();
		return;
	}
	for (wakes = 0u; wakes < KOS_IDLE_INPUT_WAKES; ++wakes) {
		idle();
	}
}

void idle_input_ready(void)
{
	idle_input_fast = KOS_IDLE_INPUT_WINDOW;
}
