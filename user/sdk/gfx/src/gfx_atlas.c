/* KonaOS libgfx: the glyph atlas's layout (docs/gfx-sdk.md sections 14
 * and 14.1).
 *
 * gfx_text_atlas gives libgfx an off-screen band and a glyph range.
 * One atlas per text colour holds the range's glyphs in that colour on
 * colour 0; the band holds as many atlases (slots) as fit.  This file
 * lays the band out - the slots' lines and every glyph's cell - so the
 * transparent draw (backends/v9958/gfx_atlas_draw.asm) builds atlases
 * and copies cells with table lookups only.
 *
 * No VIDEO IPC.
 */

#include "kos.h"

#include "kos/gfx.h"
#include "gfx_internal.h"

uint8_t gfx_atlas_active;
uint8_t gfx_atlas_mode;
uint8_t gfx_atlas_font;
uint8_t gfx_atlas_gw;
uint8_t gfx_atlas_ch;
uint8_t gfx_atlas_first;
uint16_t gfx_atlas_count;
uint8_t gfx_atlas_per_row;
uint8_t gfx_atlas_rows;
uint8_t gfx_atlas_slots;
uint8_t gfx_atlas_lru_next;
uint8_t gfx_atlas_slot_color[GFX_ATLAS_SLOTS_MAX];
uint8_t gfx_atlas_slot_lru[GFX_ATLAS_SLOTS_MAX];
uint16_t gfx_atlas_slot_y[GFX_ATLAS_SLOTS_MAX];
uint16_t gfx_atlas_cell_x[256];
uint8_t gfx_atlas_cell_y[256];

int gfx_atlas_setup(enum gfx_font font, uint16_t y, uint16_t lines,
		    uint8_t first, uint16_t count)
{
	uint8_t g6;
	uint8_t row_bytes;
	uint8_t i;
	uint16_t slot_lines;
	uint16_t index;
	uint16_t cell_x;
	uint8_t cell_y;
	uint8_t col;

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
	gfx_atlas_active = 0u;
	if (font == GFX_FONT_4X6) {
		gfx_atlas_gw = GFX_FONT4_GLYPH_WIDTH;
		gfx_atlas_ch = GFX_FONT4_GLYPH_HEIGHT;
		row_bytes = g6 != 0u ? 2u : 4u;
	} else {
		gfx_atlas_gw = GFX_FONT_GLYPH_WIDTH;
		gfx_atlas_ch = GFX_FONT_GLYPH_HEIGHT;
		row_bytes = g6 != 0u ? 3u : 6u;
	}
	/* 256 bytes to a row: 128 glyphs at 2 bytes, 85 at 3, 64 at 4,
	 * 42 at 6. */
	if (row_bytes == 2u) {
		gfx_atlas_per_row = 128u;
	} else if (row_bytes == 3u) {
		gfx_atlas_per_row = 85u;
	} else if (row_bytes == 4u) {
		gfx_atlas_per_row = 64u;
	} else {
		gfx_atlas_per_row = 42u;
	}
	/* The cells, row by row: x steps by the glyph width, a full row
	 * moves down one cell. */
	gfx_atlas_rows = 1u;
	col = 0u;
	cell_x = 0u;
	cell_y = 0u;
	for (index = 0u; index < count; ++index) {
		if (col == gfx_atlas_per_row) {
			col = 0u;
			cell_x = 0u;
			cell_y = (uint8_t)(cell_y + gfx_atlas_ch);
			++gfx_atlas_rows;
		}
		gfx_atlas_cell_x[index] = cell_x;
		gfx_atlas_cell_y[index] = cell_y;
		cell_x = (uint16_t)(cell_x + gfx_atlas_gw);
		++col;
	}
	slot_lines = (uint16_t)((uint16_t)gfx_atlas_rows * gfx_atlas_ch);
	if (lines < slot_lines) {
		return -KOS_EINVAL;
	}
	gfx_atlas_mode = gfx_state.mode;
	gfx_atlas_font = (uint8_t)font;
	gfx_atlas_first = first;
	gfx_atlas_count = count;
	gfx_atlas_slots = (uint8_t)(lines / slot_lines);
	if (gfx_atlas_slots > GFX_ATLAS_SLOTS_MAX) {
		gfx_atlas_slots = GFX_ATLAS_SLOTS_MAX;
	}
	gfx_atlas_lru_next = 0u;
	for (i = 0u; i < GFX_ATLAS_SLOTS_MAX; ++i) {
		gfx_atlas_slot_color[i] = GFX_ATLAS_SLOT_FREE;
		gfx_atlas_slot_lru[i] = 0u;
		gfx_atlas_slot_y[i] = (uint16_t)(y + (uint16_t)i * slot_lines);
	}
	gfx_atlas_active = 1u;
	return 0;
}

int gfx_text_atlas(enum gfx_font font, uint16_t y, uint16_t lines,
		   uint8_t first, uint16_t count)
{
	return gfx_atlas_setup(font, y, lines, first, count);
}
