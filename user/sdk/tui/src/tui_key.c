/* libtui keyboard: the SDK key decoder (kos/key.h) with the TUI's gate
 * and mouse events.
 *
 * The decoding is libkos's key_poll(); TUI_KEY_* are its KEY_* codes.
 * This member adds what the TUI owns:
 *
 * - the gate: the calls answer only between tui_init() and tui_shutdown();
 * - the reset: tui_init() advances a generation counter in the shared TUI
 *   state, and the next key call resets the decoder (so the lifecycle
 *   references neither this member nor the decoder, and a screen-only
 *   program links neither);
 * - the mouse: with the mouse enabled, a poll that finds no key reads the
 *   mouse once through the hook tui_mouse_enable() installs; a read that
 *   made an event returns TUI_KEY_MOUSE, keys already pending first.  A
 *   read costs several milliseconds, so tui_getkey() polls keys on every
 *   idle() wake but reads the mouse on its first poll and then on every
 *   TUI_KEY_MOUSE_WAKES-th wake; tui_pollkey() reads on every call that
 *   found no key.
 */

#include "tui_internal.h"
#include "kos/key.h"
#include "kos.h"

/* tui_getkey() reads the mouse on one of this many wakes (about 52 Hz). */
#define TUI_KEY_MOUSE_WAKES 3u

static uint16_t tui_key_generation;
/* Set by tui_getkey() around a poll that must not read the mouse. */
static uint8_t tui_key_mouse_idle;

static void tui_key_sync(void)
{
	if (tui_key_generation != tui_state.key_generation) {
		tui_key_generation = tui_state.key_generation;
		key_reset();
	}
}

int tui_key_pending(void)
{
	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	tui_key_sync();
	return key_pending();
}

tui_key_t tui_pollkey(void)
{
	tui_key_t key;

	if (tui_state.initialized == 0u) {
		return TUI_KEY_NONE;
	}
	tui_key_sync();
	key = key_poll();
	if (key == TUI_KEY_NONE && tui_key_mouse_idle == 0u &&
	    tui_mouse_hook != (const struct tui_mouse_hook *)0 &&
	    tui_mouse_hook->poll() > 0) {
		return TUI_KEY_MOUSE;
	}
	return key;
}

tui_key_t tui_getkey(void)
{
	tui_key_t key;
	uint8_t wake = 0u;

	if (tui_state.initialized == 0u) {
		return TUI_KEY_NONE;
	}
	for (;;) {
		/* Only the first poll and every TUI_KEY_MOUSE_WAKES-th wake
		 * after it may read the mouse. */
		tui_key_mouse_idle = (uint8_t)(wake != 0u);
		key = tui_pollkey();
		tui_key_mouse_idle = 0u;
		if (key != TUI_KEY_NONE) {
			return key;
		}
		idle();
		if (++wake >= TUI_KEY_MOUSE_WAKES) {
			wake = 0u;
		}
	}
}
