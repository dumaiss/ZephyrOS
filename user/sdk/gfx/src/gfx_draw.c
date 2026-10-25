/* KonaOS libgfx: the drawing primitives (docs/gfx-sdk.md section 8).
 *
 * gfx_clear is a fill of the visible page; gfx_fill_rect and
 * gfx_copy_rect are the rectangle primitives (the shapes live in
 * gfx_shapes.c).  Each call validates its geometry and the op - a
 * refusal is -KOS_EINVAL (or YJK's -KOS_ENOTSUP) and queues nothing -
 * then submits a 16-byte record: a call outside a batch starts its
 * command at once, one inside is queued and runs with the batch
 * (gfx_batch.c).  The V9958 backend turns FILL into HMMV when the
 * op is COPY and x and width are byte-aligned, and into LMMV (the
 * per-dot logical fill) otherwise, and COPY into HMMM, choosing the
 * copy's DIX/DIY from the rectangles' overlap.
 *
 * No VIDEO IPC.
 */

#include "kos.h"

#include "kos/gfx.h"
#include "gfx_internal.h"

/* The mode's full width and the x bounds (shared with the shapes:
 * gfx_shapes.c). */
int gfx_rect_ready(uint16_t x, uint16_t width)
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
	if (x >= full_width || (uint16_t)(x + width) > full_width) {
		return -KOS_EINVAL;
	}
	return 1;
}

/* The op and the shape rule: an unknown operation is -KOS_EINVAL; in
 * YJK only an aligned COPY fill keeps the four-pixel J/K runs, the
 * other shapes are -KOS_ENOTSUP. */
static int gfx_draw_op(enum gfx_op op, int yjk)
{
	if (op != GFX_OP_COPY && op != GFX_OP_XOR) {
		return -KOS_EINVAL;
	}
	if (yjk != 0 && op != GFX_OP_COPY) {
		return -KOS_ENOTSUP;
	}
	return 1;
}

int gfx_rect_check_y(uint16_t y, uint16_t height)
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
	return gfx_fill_rect(0u, 0u, gfx_state.width, GFX_HEIGHT, color,
			     GFX_OP_COPY);
}

int gfx_fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
		  gfx_color_t color, enum gfx_op op)
{
	struct gfx_cmd cmd;
	int ready = gfx_rect_ready(x, width);
	int rows;
	int op_ok;
	int yjk = gfx_state.mode == (uint8_t)GFX_MODE_256x212_YJK ? 1 : 0;
	int g6 = gfx_state.mode == (uint8_t)GFX_MODE_512x212_16 ? 1 : 0;

	if (ready <= 0) {
		return ready;
	}
	rows = gfx_rect_check_y(y, height);
	if (rows <= 0) {
		return rows;
	}
	op_ok = gfx_draw_op(op, yjk);
	if (op_ok <= 0) {
		return op_ok;
	}
	/* In YJK only an aligned COPY keeps the four-pixel J/K runs. */
	if (yjk != 0 && ((x | width) & 3u) != 0u) {
		return -KOS_EINVAL;
	}
	if (g6 != 0 && (color & 0xF0u) != 0u) {
		return -KOS_EINVAL;
	}
	cmd.opcode = GFX_CMD_FILL;
	/* The operation nibble; XOR also forces the per-pixel LMMV, and
	 * an odd x or width does (a high-speed move takes whole bytes). */
	cmd.flags = (uint8_t)op == GFX_OP_XOR ? GFX_V9958_OP_EOR :
		GFX_V9958_OP_IMP;
	cmd.sx = 0u;
	cmd.sy = 0u;
	cmd.dx = x;
	cmd.dy = y;
	cmd.width = width;
	cmd.height = height;
	cmd.color = color;
	cmd.reserved = ((g6 != 0 && ((x | width) & 1u) != 0u) ||
			(uint8_t)op == GFX_OP_XOR) ? 1u : 0u;
	gfx_batch_submit(&cmd);
	return 0;
}

int gfx_copy_rect(uint16_t src_x, uint16_t src_y, uint16_t dst_x,
		  uint16_t dst_y, uint16_t width, uint16_t height)
{
	struct gfx_cmd cmd;
	int source = gfx_rect_ready(src_x, width);
	int rows;

	if (source <= 0) {
		return source;
	}
	rows = gfx_rect_check_y(src_y, height);
	if (rows <= 0) {
		return rows;
	}
	if (gfx_rect_ready(dst_x, width) <= 0) {
		return -KOS_EINVAL;
	}
	if (gfx_rect_check_y(dst_y, height) <= 0) {
		return -KOS_EINVAL;
	}
	if (gfx_state.mode == (uint8_t)GFX_MODE_512x212_16 &&
	    ((src_x | dst_x | width) & 1u) != 0u) {
		return -KOS_EINVAL;
	}
	if (gfx_state.mode == (uint8_t)GFX_MODE_256x212_YJK &&
	    ((src_x | dst_x | width) & 3u) != 0u) {
		return -KOS_EINVAL;
	}
	cmd.opcode = GFX_CMD_COPY;
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
