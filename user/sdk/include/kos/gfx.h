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

/* One palette entry in the portable representation: 8 bits per channel;
 * the backend converts to its hardware's scale. */
struct gfx_rgb {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

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

/* Program `count` palette entries starting at `first` (the 16-colour
 * mode holds 16 entries).  Direct I/O under the hardware WAIT; a mode
 * without a programmable palette (256-colour and YJK) is
 * -KOS_ENOTSUP.  No VIDEO IPC. */
int gfx_set_palette(uint8_t first, uint8_t count,
		    const struct gfx_rgb *colors);

/* Begin a streaming pixel upload into the rectangle (x, y, width,
 * height): the geometry is pixels, y may target the off-screen page
 * (256-511), and the rectangle must lie inside the 512-line space and
 * the mode's width (x and width even in the 16-colour mode, multiples
 * of 4 in YJK, where four pixels share one J/K pair).  The byte
 * stream follows the mode's packing; the transfer takes exactly
 * bytes(width, height), position is kept across gfx_upload_write
 * calls, and a pending drawing batch is flushed first.  Not nestable.
 * No VIDEO IPC. */
int gfx_upload_begin(uint16_t x, uint16_t y, uint16_t width,
		     uint16_t height);

/* Stream `length` payload bytes of the current upload through the data
 * port, one VRAM address per row (one for the whole transfer when the
 * rectangle spans the full width).  Any chunk length; 0 or the bytes
 * written, -KOS_EINVAL when the chunk would run past the upload's end
 * (the chunk is refused whole and the position is unchanged). */
int gfx_upload_write(const void *data, uint16_t length);

/* Finish the upload: 0 when the exact byte count was received, or
 * -KOS_EINVAL with the upload cancelled when bytes are missing. */
int gfx_upload_end(void);

#endif /* KOS_SDK_GFX_H */
