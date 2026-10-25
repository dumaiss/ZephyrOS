/* KonaOS libgfx: the shapes (docs/gfx-sdk.md section 8.1).
 *
 * gfx_rect, gfx_line and gfx_point validate their geometry and the
 * op, then submit one record each (four LINE commands for a
 * rectangle, its corners plotted exactly once).  The ellipses are
 * computed here, on the CPU, with the midpoint algorithm walking one
 * quadrant and mirroring it; a filled ellipse turns into one
 * horizontal LMMV span per scanline, an outline into the left and
 * right span pieces covering every outline pixel, never overlapping,
 * so an XOR ellipse drawn twice restores everything.  A shape expands
 * into its span records at queue time (the record's sx, sy, dx, dy
 * carry the geometry and flags the operation, per section 9.1); the
 * batch auto-flushes as it fills.  The radii's squares are 32-bit in
 * the error terms only; the span output is 16-bit.
 *
 * Speed beats code size in this file: the per-pixel work is the
 * tightest C the compiler allows, and the VDP side of every record
 * (the block build and the R#32-46 write) is the backend's assembly.
 *
 * No VIDEO IPC.
 */

#include "kos.h"

#include "kos/gfx.h"
#include "gfx_internal.h"

/* 32 x 16 -> 32 bit multiplication in four 16 x 16 products (the
 * SDK's runtime holds no long-multiply helper; the ellipse's error
 * terms need the full 32 bits, and the radii's squares fit them
 * exactly).  Each product's operands are 16 bits at most, so the
 * pieces cannot overlap. */
static uint32_t gfx_mul32(uint32_t a, uint16_t b)
{
	uint32_t a_lo = (uint16_t)a;
	uint32_t a_hi = (uint16_t)(a >> 16);
	uint32_t b_lo = (uint16_t)b;
	uint32_t b_hi = (uint16_t)(b >> 8);

	return (uint32_t)((uint16_t)a_lo * (uint16_t)b_lo) +
	       ((uint32_t)((uint16_t)a_lo * (uint16_t)b_hi) << 8) +
	       ((uint32_t)((uint16_t)a_hi * (uint16_t)b_lo) << 16) +
	       ((uint32_t)((uint16_t)a_hi * (uint16_t)b_hi) << 24);
}

/* 32 x 32 -> 32 bit multiplication, the same way: four 16 x 16
 * products, used once per ellipse for the second region's error. */
static uint32_t gfx_mul32x32(uint32_t a, uint32_t b)
{
	uint32_t a_lo = (uint16_t)a;
	uint32_t a_hi = (uint16_t)(a >> 16);
	uint32_t b_lo = (uint16_t)b;
	uint32_t b_hi = (uint16_t)(b >> 16);

	return (uint32_t)((uint16_t)a_lo * (uint16_t)b_lo) +
	       ((uint32_t)((uint16_t)a_lo * (uint16_t)b_hi) << 16) +
	       ((uint32_t)((uint16_t)a_hi * (uint16_t)b_lo) << 16) +
	       ((uint32_t)((uint16_t)a_hi * (uint16_t)b_hi) << 16);
}

/* The op for a shape: an unknown op is -KOS_EINVAL, and in YJK every
 * shape is -KOS_ENOTSUP (only gfx_fill_rect's aligned COPY keeps the
 * four-pixel J/K runs). */
static int gfx_shape_op(enum gfx_op op)
{
	if (op != GFX_OP_COPY && op != GFX_OP_XOR) {
		return -KOS_EINVAL;
	}
	if (gfx_state.mode == (uint8_t)GFX_MODE_256x212_YJK) {
		return -KOS_ENOTSUP;
	}
	return 1;
}

/* The 16-colour mode's colours are four bits. */
static int gfx_shape_color(gfx_color_t color)
{
	if (gfx_state.mode == (uint8_t)GFX_MODE_512x212_16 &&
	    (color & 0xF0u) != 0u) {
		return -KOS_EINVAL;
	}
	return 1;
}

int gfx_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
	     gfx_color_t color, enum gfx_op op)
{
	int op_ok = gfx_shape_op(op);
	int color_ok;
	int ready;
	int rows;
	uint16_t last_x;
	uint16_t last_y;

	if (op_ok <= 0) {
		return op_ok;
	}
	color_ok = gfx_shape_color(color);
	if (color_ok <= 0) {
		return color_ok;
	}
	ready = gfx_rect_ready(x, width);
	if (ready <= 0) {
		return ready;
	}
	rows = gfx_rect_check_y(y, height);
	if (rows <= 0) {
		return rows;
	}
	last_x = (uint16_t)(x + width - 1u);
	last_y = (uint16_t)(y + height - 1u);
	/* The top and bottom edges first, then the right and left sides
	 * from bottom to top; every corner is one LINE endpoint, never
	 * two, so the corner dots are plotted exactly once. */
	if (gfx_line(x, y, last_x, y, color, op) < 0 ||
	    gfx_line(last_x, y, last_x, last_y, color, op) < 0 ||
	    gfx_line(last_x, last_y, x, last_y, color, op) < 0 ||
	    gfx_line(x, last_y, x, y, color, op) < 0) {
		return -KOS_EINVAL;
	}
	return 0;
}

int gfx_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
	     gfx_color_t color, enum gfx_op op)
{
	struct gfx_cmd cmd;
	int op_ok = gfx_shape_op(op);
	int color_ok;

	if (op_ok <= 0) {
		return op_ok;
	}
	color_ok = gfx_shape_color(color);
	if (color_ok <= 0) {
		return color_ok;
	}
	if (x0 >= gfx_state.width || x1 >= gfx_state.width ||
	    y0 >= 512u || y1 >= 512u) {
		return -KOS_EINVAL;
	}
	/* The VDP's DX/DY is the start, Maj/Min the signed endpoint
	 * deltas, so all eight octants and the horizontal and vertical
	 * cases run from (x0, y0) to (x1, y1) inclusive. */
	cmd.opcode = GFX_CMD_LINE;
	cmd.flags = (uint8_t)op == GFX_OP_XOR ? GFX_V9958_OP_EOR :
		GFX_V9958_OP_IMP;
	cmd.sx = x1;
	cmd.sy = y1;
	cmd.dx = x0;
	cmd.dy = y0;
	cmd.width = 0u;
	cmd.height = 0u;
	cmd.color = color;
	cmd.reserved = 0u;
	gfx_batch_submit(&cmd);
	return 0;
}

int gfx_point(uint16_t x, uint16_t y, gfx_color_t color, enum gfx_op op)
{
	struct gfx_cmd cmd;
	int op_ok = gfx_shape_op(op);
	int color_ok;

	if (op_ok <= 0) {
		return op_ok;
	}
	color_ok = gfx_shape_color(color);
	if (color_ok <= 0) {
		return color_ok;
	}
	if (x >= gfx_state.width || y >= 512u) {
		return -KOS_EINVAL;
	}
	cmd.opcode = GFX_CMD_POINT;
	cmd.flags = (uint8_t)op == GFX_OP_XOR ? GFX_V9958_OP_EOR :
		GFX_V9958_OP_IMP;
	cmd.sx = 0u;
	cmd.sy = 0u;
	cmd.dx = x;
	cmd.dy = y;
	cmd.width = 0u;
	cmd.height = 0u;
	cmd.color = color;
	cmd.reserved = 0u;
	gfx_batch_submit(&cmd);
	return 0;
}

/* One scanline's bounds live in the shared span scratch
 * (struct gfx_span_set in gfx_internal.h): the leftmost and rightmost
 * visited dot, and whether the line was visited. */

/* Record the visited point's mirrors: the scanline cy + y and cy - y
 * each touch the dots cx - x and cx + x. */
static int gfx_span_visit(struct gfx_span_set *set, uint16_t cx,
			  uint16_t cy, uint16_t x, uint16_t y)
{
	uint16_t line;
	uint16_t left = (uint16_t)(cx - x);
	uint16_t right = (uint16_t)(cx + x);

	line = (uint16_t)(cy + y);
	if (set->count[line] == 0u) {
		set->count[line] = 1u;
		set->first[line] = left;
		set->last[line] = right;
	} else {
		if (left < set->first[line]) {
			set->first[line] = left;
		}
		if (right > set->last[line]) {
			set->last[line] = right;
		}
	}
	if (y != 0u) {
		line = (uint16_t)(cy - y);
		if (set->count[line] == 0u) {
			set->count[line] = 1u;
			set->first[line] = left;
			set->last[line] = right;
		} else {
			if (left < set->first[line]) {
				set->first[line] = left;
			}
			if (right > set->last[line]) {
				set->last[line] = right;
			}
		}
	}
	return 0;
}

/* The midpoint ellipse walk: region 1 steps X from the top of the
 * ellipse to the 45-degree point, region 2 steps Y down to the bottom
 * of the first quadrant; every visited point mirrors into the
 * per-scanline bounds in `set`.  The radii's squares and the decision
 * variables are 32-bit (511^2 does not fit 16 bits); the decision
 * terms stay inside a signed 32-bit integer for every ellipse the
 * API accepts. */
static int gfx_ellipse_walk(uint16_t cx, uint16_t cy, uint16_t ry,
			    uint32_t rx2, uint32_t ry2,
			    struct gfx_span_set *set)
{
	uint16_t x = 0u;
	uint16_t y = ry;
	int32_t d1 = (int32_t)(ry2 - gfx_mul32(rx2, ry) + rx2 / 4u);
	uint32_t dx2 = 0u;      /* 2 * ry2 * x */
	uint32_t dy2 = gfx_mul32(2u * rx2, y);
	int32_t d2;

	while (dx2 <= dy2) {
		gfx_span_visit(set, cx, cy, x, y);
		x++;
		dx2 += 2u * ry2;
		if (d1 < 0) {
			d1 += (int32_t)(dx2 + ry2);
		} else {
			y--;
			dy2 -= 2u * rx2;
			d1 += (int32_t)(dx2 + ry2 - dy2);
		}
	}
	d2 = (int32_t)(gfx_mul32x32(ry2, gfx_mul32(x - 1u, x - 1u)) +
		       gfx_mul32x32(rx2, gfx_mul32(y, y)) -
		       gfx_mul32x32(rx2, ry2));
	while (y != 0u) {
		uint16_t old_x = x;

		gfx_span_visit(set, cx, cy, x, y);
		y--;
		if (d2 > 0) {
			d2 += (int32_t)(rx2 - gfx_mul32(2u * rx2, y));
		} else {
			x++;
			d2 += (int32_t)(rx2 - gfx_mul32(2u * rx2, y) +
					gfx_mul32(2u * ry2, old_x));
		}
	}
	/* The last visited row is the quadrant's bottom (y == 0). */
	gfx_span_visit(set, cx, cy, x, 0u);
	return 0;
}

/* Submit the record for the horizontal span [first, last] on
 * scanline `y` (the record is staged in gfx_state.span_record, so no
 * stack frame grows per span). */
static void gfx_shape_submit_span(uint16_t first, uint16_t last,
				  uint16_t y, gfx_color_t color,
				  enum gfx_op op)
{
	struct gfx_cmd *cmd = &gfx_state.span_record;

	cmd->opcode = GFX_CMD_SPAN;
	cmd->flags = (uint8_t)op == GFX_OP_XOR ? GFX_V9958_OP_EOR :
		GFX_V9958_OP_IMP;
	cmd->sx = 0u;
	cmd->sy = 0u;
	cmd->dx = first;
	cmd->dy = y;
	cmd->width = (uint16_t)(last - first + 1u);
	cmd->height = 1u;
	cmd->color = color;
	cmd->reserved = 0u;
	gfx_batch_submit(cmd);
}

/* One shape's validation: the op, the 16-colour colour, and the
 * ellipse's bounding box inside the mode's width and the 512-line
 * space.  A radius of zero is fine (the degenerate line or point). */
static int gfx_ellipse_ready(uint16_t cx, uint16_t cy, uint16_t rx,
			     uint16_t ry, gfx_color_t color, enum gfx_op op)
{
	int op_ok = gfx_shape_op(op);
	int color_ok;

	if (op_ok <= 0) {
		return op_ok;
	}
	color_ok = gfx_shape_color(color);
	if (color_ok <= 0) {
		return color_ok;
	}
	if (cx < rx || (uint16_t)(cx + rx) >= gfx_state.width ||
	    cy < ry || (uint16_t)(cy + ry) > 511u) {
		return -KOS_EINVAL;
	}
	return 1;
}

/* The shared ellipse: walk one quadrant into the interval set, then
 * emit the per-scanline spans - for the fill, one span across the
 * mirrored interval; for the outline, the interval's own first..last
 * runs (disjoint by construction, so XOR restores). */
static int gfx_ellipse_run(uint16_t cx, uint16_t cy, uint16_t rx,
			   uint16_t ry, gfx_color_t color, enum gfx_op op,
			   int filled)
{
	struct gfx_span_set *set = &gfx_state.spans;
	uint32_t rx2;
	uint32_t ry2;
	uint16_t y;

	if (rx == 0u && ry == 0u) {
		/* The degenerate point. */
		return gfx_point(cx, cy, color, op);
	}
	if (rx == 0u || ry == 0u) {
		/* The degenerate line. */
		return gfx_line(rx == 0u ? cx : (uint16_t)(cx - rx),
				ry == 0u ? cy : (uint16_t)(cy - ry),
				(uint16_t)(cx + rx), (uint16_t)(cy + ry),
				color, op);
	}
	memset(set, 0, sizeof(*set));
	rx2 = gfx_mul32(rx, rx);
	ry2 = gfx_mul32(ry, ry);
	if (gfx_ellipse_walk(cx, cy, ry, rx2, ry2, set) < 0) {
		return -KOS_EINVAL;
	}
	for (y = 0u; y < GFX_SPAN_LINES; ++y) {
		if (set->count[y] == 0u) {
			continue;
		}
		if (filled != 0) {
			/* One span per scanline: every dot covered
			 * exactly once, so an XOR fill drawn twice
			 * restores everything. */
			gfx_shape_submit_span(set->first[y], set->last[y],
					     y, color, op);
			continue;
		}
		/* The outline: the scanline's leftmost and rightmost
		 * boundary dots - two spans per scanline, one where
		 * they meet - drawn exactly once. */
		gfx_shape_submit_span(set->first[y], set->first[y], y,
				     color, op);
		if (set->last[y] != set->first[y]) {
			gfx_shape_submit_span(set->last[y], set->last[y], y,
					     color, op);
		}
	}
	return 0;
}

int gfx_ellipse(uint16_t cx, uint16_t cy, uint16_t rx, uint16_t ry,
		gfx_color_t color, enum gfx_op op)
{
	int ready = gfx_ellipse_ready(cx, cy, rx, ry, color, op);

	if (ready <= 0) {
		return ready;
	}
	return gfx_ellipse_run(cx, cy, rx, ry, color, op, 0);
}

int gfx_fill_ellipse(uint16_t cx, uint16_t cy, uint16_t rx, uint16_t ry,
		     gfx_color_t color, enum gfx_op op)
{
	int ready = gfx_ellipse_ready(cx, cy, rx, ry, color, op);

	if (ready <= 0) {
		return ready;
	}
	return gfx_ellipse_run(cx, cy, rx, ry, color, op, 1);
}
