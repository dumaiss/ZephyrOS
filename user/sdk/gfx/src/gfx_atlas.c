/* KonaOS libgfx: the glyph atlas and transparent text
 * (docs/gfx-sdk.md sections 14 and 14.1).
 *
 * gfx_text_atlas gives libgfx an off-screen band and a glyph range.
 * One atlas per text colour holds the range's glyphs in that colour
 * on colour 0, built with the opaque emit on the colour's first
 * transparent use; when the band is full the least recently used
 * colour's atlas is rebuilt in its place.  Transparent text then
 * draws each glyph as one VDP logical copy (LMMM) with transparency
 * (TIMP), batched: the command's source is the glyph's atlas cell, so
 * colour-0 pixels leave the screen untouched.  A glyph redefined
 * since its atlas was built is redrawn into every atlas at its next
 * use, alone.
 *
 * No VIDEO IPC.
 */

#include "kos.h"

#include "kos/gfx.h"
#include "gfx_internal.h"

/* The band holds at most 44 lines; the 4x6 cell is the shortest at 6,
 * so seven atlases bound every band. */
#define GFX_ATLAS_SLOTS_MAX 8u

struct gfx_atlas_state {
	uint8_t active;            /* 1 after a successful setup */
	uint8_t mode;              /* the setup's mode (packing changes) */
	uint8_t font;              /* GFX_FONT_* */
	uint8_t glyph_width;       /* 4 or 6 dots */
	uint8_t cell_height;       /* 6 or 8 lines */
	uint16_t y;                /* the band's top */
	uint16_t lines;
	uint8_t per_row;           /* glyphs per atlas row */
	uint8_t rows;              /* atlas rows of glyphs */
	uint8_t first;
	uint16_t count;
	uint8_t slots;             /* atlases the band holds */
	uint8_t lru_next;
	struct gfx_atlas_slot {
		uint8_t color;     /* 0xFF = free */
		uint8_t lru;       /* higher = more recent */
	} slot[GFX_ATLAS_SLOTS_MAX];
};

static struct gfx_atlas_state gfx_atlas;

/* The atlas's lines per colour slot. */
static uint16_t gfx_atlas_slot_lines(const struct gfx_atlas_state *atlas)
{
	return (uint16_t)((uint16_t)atlas->rows * atlas->cell_height);
}

/* One glyph's index into the atlas row and column (repeated
 * subtraction: the ranges are small, and libgfx pays no division).
 * The divisor is copied to a local: SDCC re-derives a pointer's
 * offset from the register it just loaded, so `index >=
 * atlas->per_row` in the condition would walk the struct. */
static void gfx_atlas_index(const struct gfx_atlas_state *atlas,
			    uint8_t index, uint8_t *row, uint8_t *col)
{
	uint8_t r = 0u;
	uint8_t per = atlas->per_row;

	while (index >= per) {
		index = (uint8_t)(index - per);
		++r;
	}
	*row = r;
	*col = index;
}

/* One glyph's cell: the dot position. */
static void gfx_atlas_cell(const struct gfx_atlas_state *atlas,
			   uint8_t code, uint16_t *x, uint16_t *y)
{
	uint8_t row;
	uint8_t col;

	gfx_atlas_index(atlas, (uint8_t)(code - atlas->first), &row,
			&col);
	*x = (uint16_t)((uint16_t)col * atlas->glyph_width);
	*y = (uint16_t)((uint16_t)atlas->y +
			(uint16_t)row * atlas->cell_height);
}

int gfx_atlas_setup(enum gfx_font font, uint16_t y, uint16_t lines,
		    uint8_t first, uint16_t count)
{
	uint8_t g6;
	uint8_t row_bytes;
	uint8_t i;
	uint16_t slot_lines;

	if (gfx_state.video_handle == 0 || gfx_state.mode_set == 0u) {
		return -KOS_EINVAL;
	}
	if (gfx_state.mode == (uint8_t)GFX_MODE_256x212_YJK) {
		return -KOS_ENOTSUP;
	}
	if (font != GFX_FONT_6X8 && font != GFX_FONT_4X6) {
		return -KOS_EINVAL;
	}
	/* The band must sit whole in one page's spare lines: 212-255 or
	 * 468-511 (the visible lines are 0-211 and 256-467). */
	if (((y < 212u || (uint16_t)(y + lines) > 256u) &&
	     (y < 468u || (uint16_t)(y + lines) > 512u)) ||
	    lines == 0u) {
		return -KOS_EINVAL;
	}
	if (count == 0u || (uint16_t)(first + count) > 256u) {
		return -KOS_EINVAL;
	}
	g6 = gfx_state.mode == (uint8_t)GFX_MODE_512x212_16 ? 1u : 0u;
	gfx_atlas.active = 1u;
	gfx_atlas.mode = gfx_state.mode;
	gfx_atlas.font = (uint8_t)font;
	if (font == GFX_FONT_4X6) {
		gfx_atlas.glyph_width = GFX_FONT4_GLYPH_WIDTH;
		gfx_atlas.cell_height = GFX_FONT4_GLYPH_HEIGHT;
		row_bytes = g6 != 0u ? 2u : 4u;
	} else {
		gfx_atlas.glyph_width = GFX_FONT_GLYPH_WIDTH;
		gfx_atlas.cell_height = GFX_FONT_GLYPH_HEIGHT;
		row_bytes = g6 != 0u ? 3u : 6u;
	}
	/* 256 bytes to a row: 128 glyphs at 2 bytes, 85 at 3, 64 at 4,
	 * 42 at 6 (no division). */
	if (row_bytes == 2u) {
		gfx_atlas.per_row = 128u;
	} else if (row_bytes == 3u) {
		gfx_atlas.per_row = 85u;
	} else if (row_bytes == 4u) {
		gfx_atlas.per_row = 64u;
	} else {
		gfx_atlas.per_row = 42u;
	}
	/* The atlas rows of glyphs: ceil(count / per_row), without a
	 * division. */
	{
		uint16_t left = count;

		gfx_atlas.rows = 0u;
		do {
			left = left > gfx_atlas.per_row ?
				(uint16_t)(left - gfx_atlas.per_row) : 0u;
			++gfx_atlas.rows;
		} while (left != 0u);
	}
	gfx_atlas.y = y;
	gfx_atlas.lines = lines;
	gfx_atlas.first = first;
	gfx_atlas.count = count;
	slot_lines = gfx_atlas_slot_lines(&gfx_atlas);
	if (lines < slot_lines) {
		gfx_atlas.active = 0u;
		return -KOS_EINVAL;
	}
	gfx_atlas.slots = (uint8_t)(lines / slot_lines);
	gfx_atlas.lru_next = 0u;
	for (i = 0u; i < GFX_ATLAS_SLOTS_MAX; ++i) {
		gfx_atlas.slot[i].color = 0xFFu;
		gfx_atlas.slot[i].lru = 0u;
	}
	return 0;
}

int gfx_text_atlas(enum gfx_font font, uint16_t y, uint16_t lines,
		   uint8_t first, uint16_t count)
{
	return gfx_atlas_setup(font, y, lines, first, count);
}

/* Build one atlas: the range's glyphs in `color` on colour 0, drawn
 * with the opaque emit into the slot's rows.  The caller waited for
 * the engine, so the stream lands after every running command. */
static void gfx_atlas_build_slot(uint8_t slot_index, uint8_t color)
{
	struct gfx_atlas_state *atlas = &gfx_atlas;
	uint16_t base = (uint16_t)((uint16_t)atlas->y +
				   (uint16_t)slot_index *
				   gfx_atlas_slot_lines(atlas));
	uint16_t code;

	for (code = atlas->first;
	     code < (uint16_t)(atlas->first + atlas->count); ++code) {
		uint16_t x;
		uint16_t y;
		uint8_t row;
		uint8_t col;

		gfx_atlas_index(atlas, (uint8_t)(code - atlas->first),
				&row, &col);
		x = (uint16_t)((uint16_t)col * atlas->glyph_width);
		y = (uint16_t)(base + (uint16_t)row * atlas->cell_height);
		gfx_text_emit_glyph(x, y, (uint8_t)code, color, 0u,
				    atlas->font);
	}
	atlas->slot[slot_index].color = color;
}

/* The colour's slot: the existing one, a free one, or the least
 * recently used one, rebuilt. */
static uint8_t gfx_atlas_slot_for(uint8_t color)
{
	struct gfx_atlas_state *atlas = &gfx_atlas;
	uint8_t free_slot = 0xFFu;
	uint8_t oldest = 0u;
	uint8_t i;

	for (i = 0u; i < atlas->slots; ++i) {
		if (atlas->slot[i].color == color) {
			return i;
		}
		if (atlas->slot[i].color == 0xFFu && free_slot == 0xFFu) {
			free_slot = i;
		}
		if (atlas->slot[i].lru < atlas->slot[oldest].lru ||
		    atlas->slot[oldest].color == 0xFFu) {
			oldest = i;
		}
	}
	if (free_slot != 0xFFu) {
		oldest = free_slot;
	}
	gfx_atlas_build_slot(oldest, color);
	return oldest;
}

/* Redraw one glyph's cell in every built atlas, alone (the glyph was
 * redefined since the atlases drew it). */
static void gfx_atlas_redraw_stale(uint8_t code, uint8_t font)
{
	struct gfx_atlas_state *atlas = &gfx_atlas;
	uint8_t i;

	for (i = 0u; i < atlas->slots; ++i) {
		uint16_t x;
		uint16_t y;

		if (atlas->slot[i].color == 0xFFu) {
			continue;
		}
		uint8_t row;
		uint8_t col;

		gfx_atlas_index(atlas, (uint8_t)(code - atlas->first),
				&row, &col);
		x = (uint16_t)((uint16_t)col * atlas->glyph_width);
		y = (uint16_t)((uint16_t)atlas->y +
				(uint16_t)i * gfx_atlas_slot_lines(atlas) +
				(uint16_t)row * atlas->cell_height);
		gfx_text_emit_glyph(x, y, code,
				    atlas->slot[i].color, 0u, font);
	}
}

int gfx_atlas_draw(uint16_t x, uint16_t y, const char *text,
		   uint16_t length, uint8_t fg)
{
	struct gfx_atlas_state *atlas = &gfx_atlas;
	uint8_t font;
	uint8_t *stale;
	uint8_t slot_index;
	uint16_t i;
	uint16_t fit;
	int outside = 0;

	if (atlas->active == 0u || atlas->mode != gfx_state.mode) {
		return -KOS_EINVAL;
	}
	font = atlas->font;
	stale = font == GFX_FONT_4X6 ? gfx_state.font_stale4
		: gfx_state.font_stale6;
	if (x >= gfx_state.width) {
		return -KOS_EINVAL;
	}
	if (gfx_flush() < 0) {
		return -KOS_EINVAL;
	}
	/* The build and the stale redraws stream VRAM through the CPU:
	 * wait for the engine first. */
	gfx_v9958_io_wait_command();
	fit = (gfx_state.width - x) / atlas->glyph_width;
	if (fit > length) {
		fit = length;
	}
	slot_index = gfx_atlas_slot_for(fg);
	for (i = 0u; i < fit; ++i) {
		struct gfx_cmd cmd;
		uint8_t code = (uint8_t)text[i];
		uint16_t sx;
		uint16_t sy;
		uint8_t bit = (uint8_t)(1u << (code & 7u));

		if (code < atlas->first ||
		    code >= (uint8_t)(atlas->first + atlas->count)) {
			++outside;
			continue;
		}
		if ((stale[code >> 3] & bit) != 0u) {
			gfx_atlas_redraw_stale(code, font);
			stale[code >> 3] &= (uint8_t)~bit;
		}
		gfx_atlas_cell(atlas, code, &sx, &sy);
		cmd.opcode = GFX_CMD_GLYPH;
		cmd.flags = GFX_V9958_OP_TIMP;
		cmd.reserved = 0u;
		/* The copy's direction from any overlap with the cell (the
		 * atlas usually sits on other lines, but the target may be
		 * the band itself): the far edges and DIX/DIY, exactly as
		 * the rectangle copy. */
		if (sx < (uint16_t)(x + (uint16_t)i *
				    atlas->glyph_width)) {
			cmd.reserved |= GFX_V9958_CMD_DIX;
			sx = (uint16_t)(sx + atlas->glyph_width - 1u);
			cmd.dx = (uint16_t)(x + (uint16_t)i *
					    atlas->glyph_width +
					    atlas->glyph_width - 1u);
		} else {
			cmd.dx = (uint16_t)(x + (uint16_t)i *
					    atlas->glyph_width);
		}
		sy = (uint16_t)((uint16_t)sy +
				(uint16_t)slot_index *
				gfx_atlas_slot_lines(atlas));
		if (sy < y) {
			cmd.reserved |= GFX_V9958_CMD_DIY;
			sy = (uint16_t)(sy + atlas->cell_height - 1u);
			cmd.dy = (uint16_t)(y + atlas->cell_height - 1u);
		} else {
			cmd.dy = y;
		}
		cmd.sx = sx;
		cmd.sy = sy;
		cmd.width = atlas->glyph_width;
		cmd.height = atlas->cell_height;
		cmd.color = 0u;
		gfx_batch_submit(&cmd);
	}
	atlas->slot[slot_index].lru = ++atlas->lru_next;
	return outside;
}
