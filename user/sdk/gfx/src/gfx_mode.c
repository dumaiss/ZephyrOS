/* KonaOS libgfx: mode setup and the display switch.
 *
 * gfx_set_mode establishes a complete known register state from any
 * VDP state, with the display off; gfx_display_enable is the only call
 * that turns it on.  Both go straight to the backend: no VIDEO IPC.
 */

#include "kos.h"

#include "kos/gfx.h"
#include "gfx_internal.h"

int gfx_set_mode(enum gfx_mode mode)
{
	switch (mode) {
	case GFX_MODE_512x212_16:
	case GFX_MODE_256x212_256:
	case GFX_MODE_256x212_YJK:
		break;
	default:
		return -KOS_EINVAL;
	}
	if (gfx_state.video_handle == 0) {
		return -KOS_EINVAL;
	}
	if (gfx_state.backend != KVIDEO_BACKEND_V9958) {
		return -KOS_ENOTSUP;
	}
	if (mode == GFX_MODE_256x212_YJK &&
	    (gfx_state.caps & KVIDEO_YJK) == 0u) {
		return -KOS_ENOTSUP;
	}
	/* A queued batch holds records validated in the current mode:
	 * it executes before the register state changes. */
	if (gfx_flush() < 0) {
		return -KOS_EINVAL;
	}
	v9958_set_mode(&gfx_state, mode);
	gfx_state.mode_set = 1u;
	gfx_state.mode = (uint8_t)mode;
	if (mode == GFX_MODE_512x212_16) {
		gfx_state.width = GFX_WIDTH_G6;
		gfx_state.height = GFX_HEIGHT;
		gfx_state.bpp = GFX_BPP_G6;
	} else {
		gfx_state.width = GFX_WIDTH_G7;
		gfx_state.height = GFX_HEIGHT;
		gfx_state.bpp = GFX_BPP_G7;
	}
	return 0;
}

int gfx_display_enable(uint8_t enabled)
{
	if (enabled > 1u) {
		return -KOS_EINVAL;
	}
	if (gfx_state.video_handle == 0 || gfx_state.mode_set == 0u) {
		return -KOS_EINVAL;
	}
	v9958_display_enable(&gfx_state, enabled);
	return 0;
}
