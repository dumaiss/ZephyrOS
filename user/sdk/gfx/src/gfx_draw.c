/* KonaOS libgfx: the drawing primitives (docs/gfx-sdk.md section 8).
 *
 * gfx_clear is a fill of the visible page; gfx_fill_rect and
 * gfx_copy_rect are the rectangle primitives.  Each call validates
 * its geometry and the 16-colour mode's evenness first - a refusal is
 * -KOS_EINVAL and queues nothing - then submits a 16-byte record: a
 * call outside a batch starts its command at once, one inside is
 * queued and runs with the batch (gfx_batch.c).  The V9958 backend
 * turns FILL into HMMV and COPY into HMMM, choosing the copy's DIX/
 * DIY from the rectangles' overlap.
 *
 * No VIDEO IPC.
 */

#include "kos.h"

#include "kos/gfx.h"
#include "gfx_internal.h"

/* The mode's full width, and whether its rows take whole bytes. */
static int gfx_draw_ready(uint16_t x, uint16_t width)
{
	uint16_t full_width = (gfx_state.mode ==
			       (uint8_t)GFX_MODE_512x212_16) ?
		GFX_WIDTH_G6 : GFX_WIDTH_G7;

	if (gfx_state.video_handle == 0 || gfx_state.mode_set == 0u) {
		return -KOS_EINVAL;
	}
	if (width == 0u) {
		return 0;
	}
	if (gfx_state.mode == (uint8_t)GFX_MODE_512x212_16 &&
	    ((x | width) & 1u) != 0u) {
		return -KOS_EINVAL;
	}
	/* Four YJK pixels share one J/K pair: x and width keep the runs. */
	if (gfx_state.mode == (uint8_t)GFX_MODE_256x212_YJK &&
	    ((x | width) & 3u) != 0u) {
		return -KOS_EINVAL;
	}
	if (x >= full_width || (uint16_t)(x + width) > full_width) {
		return -KOS_EINVAL;
	}
	return 1;
}

static int gfx_draw_check_y(uint16_t y, uint16_t height)
{
	if (height == 0u) {
		return 0;
	}
	if (y >= 512u || (uint16_t)(y + height) > 512u) {
		return -KOS_EINVAL;
	}
	return 1;
}

int gfx_clear(gfx_color_t color)
{
	return gfx_fill_rect(0u, 0u, gfx_state.width, GFX_HEIGHT, color);
}

int gfx_fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
		  gfx_color_t color)
{
	struct gfx_cmd cmd;
	int ready = gfx_draw_ready(x, width);
	int rows;

	if (ready <= 0) {
		return ready;
	}
	rows = gfx_draw_check_y(y, height);
	if (rows <= 0) {
		return rows;
	}
	if (gfx_state.mode == (uint8_t)GFX_MODE_512x212_16 &&
	    (color & 0xF0u) != 0u) {
		return -KOS_EINVAL;
	}
	cmd.opcode = GFX_OP_FILL;
	cmd.flags = 0u;
	cmd.sx = 0u;
	cmd.sy = 0u;
	cmd.dx = x;
	cmd.dy = y;
	cmd.width = width;
	cmd.height = height;
	cmd.color = color;
	cmd.reserved = 0u;
	gfx_batch_submit(&cmd);
	return 0;
}

int gfx_copy_rect(uint16_t src_x, uint16_t src_y, uint16_t dst_x,
		  uint16_t dst_y, uint16_t width, uint16_t height)
{
	struct gfx_cmd cmd;
	int source = gfx_draw_ready(src_x, width);
	int rows;

	if (source <= 0) {
		return source;
	}
	rows = gfx_draw_check_y(src_y, height);
	if (rows <= 0) {
		return rows;
	}
	if (gfx_draw_ready(dst_x, width) <= 0) {
		return -KOS_EINVAL;
	}
	if (gfx_draw_check_y(dst_y, height) <= 0) {
		return -KOS_EINVAL;
	}
	cmd.opcode = GFX_OP_COPY;
	cmd.flags = 0u;
	cmd.sx = src_x;
	cmd.sy = src_y;
	cmd.dx = dst_x;
	cmd.dy = dst_y;
	cmd.width = width;
	cmd.height = height;
	cmd.color = 0u;
	cmd.reserved = 0u;
	gfx_batch_submit(&cmd);
	return 0;
}
