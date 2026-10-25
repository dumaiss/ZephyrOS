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
	uint8_t page;
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

/* Clear the visible page to `color` (in the 16-colour mode 0..15)
 * with one high-speed fill, waited for.  Inside a batch the fill is
 * queued and executed with the batch.  No VIDEO IPC. */
int gfx_clear(gfx_color_t color);

/* The operation a shape draws with: GFX_OP_COPY writes the colour,
 * GFX_OP_XOR combines it with what is there, so a shape drawn twice
 * restores the screen (a cursor, a play marker, a selection). */
enum gfx_op {
	GFX_OP_COPY,
	GFX_OP_XOR
};

/* Batch drawing commands: between gfx_begin and the matching gfx_end,
 * drawing calls append compact records to a process-local queue (32
 * records; a full queue flushes itself).  gfx_end flushes when the
 * outermost batch closes; nested begin/end pairs are counted.
 * gfx_flush executes the queued records now.  A drawing call outside a
 * batch executes at once.  No VIDEO IPC. */
void gfx_begin(void);
int gfx_end(void);
int gfx_flush(void);

/* Wait until the last drawing command has finished, for a program
 * that reads VRAM or times its frame.  No VIDEO IPC. */
int gfx_wait(void);

/* Show the page (0 or 1): wait for the command engine and the next
 * vertical blank, then switch the displayed page (R#2) - atomic, no
 * tearing.  gfx_get_info reports the shown page; gfx_set_mode shows
 * page 0.  No VIDEO IPC. */
int gfx_show_page(uint8_t page);

/* Wait for the start of the next vertical retrace (status register
 * 2's VR: low, then high), for a frame loop or a partial present.  No
 * VIDEO IPC. */
int gfx_wait_vblank(void);

/* A timing marker: one register write (R#16 = 15, the palette
 * pointer) that the gfxtest timing replay reads back to count CPU
 * T-states between markers.  No VIDEO IPC. */
void gfx_timing_mark(void);

/* Fill the rectangle (x, y, width, height) with `color`: HMMV when
 * the op is COPY and x and width are byte-aligned, LMMV (a logical,
 * per-pixel fill) otherwise, so any x is accepted in the 16-colour
 * mode with LMMV, and XOR combines with what is there.  In YJK (four
 * pixels share one J/K pair) only an aligned COPY is accepted
 * (-KOS_ENOTSUP otherwise).  y may target the off-screen page
 * (256-511).  A zero width or height is a no-op (0, nothing queued);
 * a rectangle outside the mode's width or the 512-line space is
 * -KOS_EINVAL and queues nothing.  Batched or immediate per above. */
int gfx_fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
		  gfx_color_t color, enum gfx_op op);

/* The rectangle's outline in `color`: four LINE commands, each corner
 * plotted exactly once.  Sizes, bounds, YJK and batching as
 * gfx_fill_rect (the outline runs per dot, so in YJK it is
 * -KOS_ENOTSUP). */
int gfx_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
	     gfx_color_t color, enum gfx_op op);

/* The line from (x0, y0) to (x1, y1) inclusive, one LINE command.
 * YJK is -KOS_ENOTSUP; a line outside the mode's width or the
 * 512-line space is -KOS_EINVAL and queues nothing. */
int gfx_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
	     gfx_color_t color, enum gfx_op op);

/* One dot, one PSET command; bounds, YJK and batching as gfx_line. */
int gfx_point(uint16_t x, uint16_t y, gfx_color_t color, enum gfx_op op);

/* The ellipse outline centred at (cx, cy) with radii rx and ry: the
 * midpoint algorithm walks one quadrant and mirrors it, and each
 * scanline's outline pixels are drawn as horizontal LMMV spans that
 * never overlap, so an XOR ellipse drawn twice restores everything.
 * The radii's squares are 32-bit in the error terms only.  A zero
 * radius draws the degenerate line or point.  The bounding box must
 * lie inside the mode's width and the 512-line space (-KOS_EINVAL
 * queues nothing); YJK is -KOS_ENOTSUP. */
int gfx_ellipse(uint16_t cx, uint16_t cy, uint16_t rx, uint16_t ry,
		gfx_color_t color, enum gfx_op op);

/* The filled ellipse: one horizontal LMMV span per scanline.  Bounds,
 * YJK and batching as gfx_ellipse. */
int gfx_fill_ellipse(uint16_t cx, uint16_t cy, uint16_t rx, uint16_t ry,
		     gfx_color_t color, enum gfx_op op);

/* Copy the rectangle (width, height) from (src_x, src_y) to (dst_x,
 * dst_y) with one high-speed move (HMMM); the direction is chosen
 * from the overlap, so overlapping copies are exact in all four
 * directions.  Source and destination may sit on either page.  Bounds
 * and zero sizes as gfx_fill_rect. */
int gfx_copy_rect(uint16_t src_x, uint16_t src_y, uint16_t dst_x,
		  uint16_t dst_y, uint16_t width, uint16_t height);

/* The built-in 6x8 CP850 font.  gfx_open copies it into the 2,048-byte
 * RAM font; these calls change that copy, so a redefined glyph is what
 * the next draw uses and there is no VRAM cache.  `rows` holds eight
 * row bytes whose low six bits are the pixels.  No VIDEO IPC. */
int gfx_font_set_glyph(uint8_t code, const uint8_t rows[8]);
int gfx_font_reset_glyph(uint8_t code);
void gfx_font_reset(void);

/* Opaque text.  `fg` and `bg` are the mode's colour (0..15 in the
 * 16-colour mode). */
struct gfx_text_style {
	gfx_color_t fg;
	gfx_color_t bg;
};

/* Draw one line of byte-oriented text (no newlines, tabs or escapes)
 * at (x, y), the top-left pixel of the first glyph; x and width count
 * pixels (6 per glyph, height 8).  Glyphs are clipped whole at the
 * right edge; the off-screen page is an ordinary target.  A pending
 * drawing batch is flushed first.  The YJK mode is -KOS_ENOTSUP; in
 * the 16-colour mode x must be even.  No VIDEO IPC. */
int gfx_text(uint16_t x, uint16_t y, const char *text,
	     const struct gfx_text_style *style);
int gfx_text_n(uint16_t x, uint16_t y, const char *text, uint16_t length,
	       const struct gfx_text_style *style);

/* The default font's width of `text`: its length x 6. */
uint16_t gfx_text_width(const char *text);

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
