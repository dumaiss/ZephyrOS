/* KonaOS SDK: libgfx, the native graphics library (docs/gfx-sdk.md).
 *
 * A program includes only this header and links `LIBS=gfx`.  One call
 * - gfx_open - takes `/dev/video` exclusively (one bind, one
 * KVIDEO_OP_DIRECT_INFO); from then on every call works the hardware
 * directly through the ports the record reported, with no VIDEO IPC,
 * until gfx_close (or the process's exit) releases the handle and the
 * driver restores the console.
 *
 * Return convention matches kos/console.h: a non-negative result, or
 * the negated native error code.  gfx_close is safe after any
 * successful gfx_open and any number of times.
 */

#ifndef KOS_SDK_GFX_H
#define KOS_SDK_GFX_H

#include "kos/types.h"
#include "kos/errno.h"
#include "kos/kvideo.h"

/* The graphics modes.  Every mode is 212 lines, non-interlaced. */
enum gfx_mode {
	GFX_MODE_512x212_16,   /* 512 x 212, 4 bpp, 16-colour palette   */
	GFX_MODE_256x212_256,  /* 256 x 212, 8 bpp                     */
	GFX_MODE_256x212_YJK   /* 256 x 212, host-encoded YJK bytes    */
};

typedef uint8_t gfx_color_t;

/* The current mode's geometry (gfx_get_info). */
struct gfx_info {
	enum gfx_mode mode;
	uint16_t width;
	uint16_t height;
	uint8_t bpp;
};

/* Take `/dev/video` exclusively and initialize the graphics context:
 * one exclusive bind, the object-type check, KVIDEO_OP_DIRECT_INFO and
 * the backend selection.  A failure after the bind releases the handle
 * before returning, so the call is atomic for the application.  The
 * process has one context; a second open while one is open is
 * -KOS_EBUSY. */
int gfx_open(void);

/* Release the VIDEO handle (the driver restores the console); the call
 * is idempotent and is skipped after a failed gfx_open. */
void gfx_close(void);

/* Fill `*info` with the mode set by gfx_set_mode and its geometry; 0,
 * -KOS_EINVAL without an open context or a set mode, or another
 * negative error. */
int gfx_get_info(struct gfx_info *info);

/* Establish the complete register state of `mode` from any VDP state,
 * with the display off until gfx_display_enable(1).  No VIDEO IPC. */
int gfx_set_mode(enum gfx_mode mode);

/* Turn the display on (enabled nonzero) or off.  No VIDEO IPC. */
int gfx_display_enable(uint8_t enabled);

/* Clear both VRAM pages' visible area to `color` (in the 16-colour
 * mode 0..15) with one high-speed fill, waited for.  No VIDEO IPC. */
int gfx_clear(gfx_color_t color);

#endif /* KOS_SDK_GFX_H */
