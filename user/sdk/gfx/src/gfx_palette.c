/* KonaOS libgfx: the palette.
 *
 * The 16-colour mode holds a 16-entry programmable palette;
 * gfx_set_palette converts the portable 8-bit RGB to the backend's
 * scale and writes it under the hardware WAIT.  The 256-colour and YJK
 * modes have no application palette: -KOS_ENOTSUP.  No VIDEO IPC.
 */

#include "kos.h"

#include "kos/gfx.h"
#include "gfx_internal.h"

int gfx_set_palette(uint8_t first, uint8_t count,
		    const struct gfx_rgb *colors)
{
	if (gfx_state.video_handle == 0 || gfx_state.mode_set == 0u) {
		return -KOS_EINVAL;
	}
	if (gfx_state.mode != (uint8_t)GFX_MODE_512x212_16) {
		return -KOS_ENOTSUP;
	}
	if (count == 0u) {
		return 0;
	}
	if (colors == (const struct gfx_rgb *)0 || first > 15u ||
	    (uint16_t)first + count > 16u) {
		return -KOS_EINVAL;
	}
	v9958_set_palette(&gfx_state, first, count, colors);
	return 0;
}
