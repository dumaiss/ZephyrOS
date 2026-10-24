/* KonaOS libgfx: the drawing call of the foundation, gfx_clear.
 *
 * One high-speed fill (HMMV) over both VRAM pages' visible area,
 * waited for before the call returns: the clear is complete the moment
 * the caller sees 0.  No VIDEO IPC.
 */

#include "kos.h"

#include "kos/gfx.h"
#include "gfx_internal.h"

int gfx_clear(gfx_color_t color)
{
	if (gfx_state.video_handle == 0 || gfx_state.mode_set == 0u) {
		return -KOS_EINVAL;
	}
	if (gfx_state.mode == (uint8_t)GFX_MODE_512x212_16 &&
	    (color & 0xF0u) != 0u) {
		return -KOS_EINVAL;
	}
	v9958_clear(&gfx_state, color);
	return 0;
}
