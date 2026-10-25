/* KonaOS libgfx: text (docs/gfx-sdk.md sections 13 and 14).
 *
 * Opaque text is scanline-major: the C side validates once per call,
 * waits for the engine, then sets one VRAM address per scanline of
 * the string and hands the row to the assembly loop
 * (gfx_v9958_io_text_row), which streams every glyph's bytes (font
 * row byte, the pair-table lookup, OUT).  In GRAPHIC 6 the 6x8
 * expansion is the pair cache's three 256-entry pages; the 4x6's is
 * the 32-byte pair table (gfx_font4.c).  In GRAPHIC 7 the loop
 * expands the font row's pixels, one byte each.
 *
 * Transparent text (GFX_TEXT_TRANSPARENT) goes to the glyph atlas
 * (gfx_atlas.c): each glyph is one batched LMMM with transparency.
 *
 * gfx_text is single-line and byte-oriented: no newlines, tabs,
 * escapes or encodings.  Glyphs are clipped whole at the right edge;
 * the off-screen page is an ordinary target.  A pending drawing batch
 * is flushed first.  No VIDEO IPC.
 */

#include "kos.h"

#include "kos/gfx.h"
#include "gfx_internal.h"

/* Build one G6 pair's three expansion pages: page b maps a font row
 * byte to output byte b of the cell - font pixels 2b and 2b+1 (bits
 * 7-2b and 6-2b) through the pair table, the same construction the
 * console renderer uses. */
static void gfx_text_build_pages(struct gfx_text_cache_entry *entry)
{
	uint8_t pair[4];
	uint8_t page;
	uint16_t row;
	uint8_t shift;

	pair[0] = (uint8_t)((entry->bg << 4) | entry->bg);
	pair[1] = (uint8_t)((entry->bg << 4) | entry->fg);
	pair[2] = (uint8_t)((entry->fg << 4) | entry->bg);
	pair[3] = (uint8_t)((entry->fg << 4) | entry->fg);
	for (page = 0u; page < 3u; ++page) {
		shift = (uint8_t)(6u - 2u * page);
		for (row = 0u; row < 256u; ++row) {
			entry->pages[page][row] =
				pair[(row >> shift) & 0x03u];
		}
	}
}

/* The 6x8 pair cache: a matching entry, or a fresh round-robin
 * one. */
static struct gfx_text_cache_entry *gfx_text_pair(uint8_t fg, uint8_t bg)
{
	struct gfx_text_cache_entry *entry;
	uint8_t i;

	for (i = 0u; i < GFX_TEXT_CACHE_ENTRIES; ++i) {
		entry = &gfx_state.text_cache[i];
		if (entry->active != 0u && entry->fg == fg &&
		    entry->bg == bg) {
			return entry;
		}
	}
	entry = &gfx_state.text_cache[gfx_state.text_cache_next];
	gfx_state.text_cache_next =
		(uint8_t)(gfx_state.text_cache_next + 1u >=
			  GFX_TEXT_CACHE_ENTRIES ?
			  0u : gfx_state.text_cache_next + 1u);
	entry->fg = fg;
	entry->bg = bg;
	gfx_text_build_pages(entry);
	entry->active = 1u;
	return entry;
}

/* The font's RAM bytes and its geometry. */
static const uint8_t *gfx_text_font_ram(uint8_t font)
{
	if (font == GFX_FONT_4X6) {
		return gfx_font4_ram();
	}
	return gfx_state.font_ram;
}

static uint8_t gfx_text_font_stride(uint8_t font)
{
	return font == GFX_FONT_4X6 ? GFX_FONT4_GLYPH_STRIDE
		: GFX_FONT_GLYPH_BYTES;
}

static uint8_t gfx_text_font_width(uint8_t font)
{
	return font == GFX_FONT_4X6 ? GFX_FONT4_GLYPH_WIDTH
		: GFX_FONT_GLYPH_WIDTH;
}

static uint8_t gfx_text_font_height(uint8_t font)
{
	return font == GFX_FONT_4X6 ? GFX_FONT4_GLYPH_HEIGHT
		: GFX_FONT_GLYPH_HEIGHT;
}

/* One opaque glyph cell at (x, y) through the scanline emit.  The
 * caller owns the validations and the engine wait (every set_vram
 * waits again, so the bytes can never race a running command). */
void gfx_text_emit_glyph(uint16_t x, uint16_t y, uint8_t code,
			 uint8_t fg, uint8_t bg, uint8_t font)
{
	uint8_t g6 = gfx_state.mode == (uint8_t)GFX_MODE_512x212_16 ?
		1u : 0u;
	const uint8_t *ram = gfx_text_font_ram(font);
	uint8_t height = gfx_text_font_height(font);
	uint16_t col = g6 != 0u ? (uint16_t)(x / 2u) : x;
	uint8_t row_index;

	for (row_index = 0u; row_index < height; ++row_index) {
		struct gfx_text_row_args args;
		uint8_t code_here = code;
		uint16_t offset = (uint16_t)((uint16_t)(y + row_index) *
					     256u + col);
		uint8_t r14 = (uint8_t)(((offset >> 14) & 0x03u) |
					(((y + row_index) >> 8) << 2));

		gfx_v9958_io_set_vram(r14, offset);
		args.text = &code_here;
		args.font = ram + row_index;
		args.table0 = (const uint8_t *)0;
		args.table1 = (const uint8_t *)0;
		args.table2 = (const uint8_t *)0;
		args.count = 1u;
		args.fg = fg;
		args.bg = bg;
		if (g6 != 0u) {
			if (font == GFX_FONT_4X6) {
				args.table0 = gfx_font4_pair(fg, bg);
				args.mode = 1u;
			} else {
				struct gfx_text_cache_entry *entry =
					gfx_text_pair(fg, bg);

				args.table0 = entry->pages[0];
				args.table1 = entry->pages[1];
				args.table2 = entry->pages[2];
				args.mode = 0u;
			}
		} else {
			args.mode = font == GFX_FONT_4X6 ? 3u : 2u;
		}
		gfx_v9958_io_text_row(&args);
	}
}

/* The shared run: validation, then the transparent path (the atlas)
 * or the scanline-major opaque emit, whole glyphs clipped at the
 * right edge. */
static int gfx_text_run(uint16_t x, uint16_t y, const char *text,
			uint16_t length, const struct gfx_text_style *style)
{
	uint8_t g6;
	uint8_t font;
	uint8_t width;
	uint8_t height;
	uint16_t fit;
	uint16_t col;
	uint8_t row_index;
	const uint8_t *ram;
	uint8_t stride;

	if (gfx_state.video_handle == 0 || gfx_state.mode_set == 0u) {
		return -KOS_EINVAL;
	}
	if (gfx_state.mode == (uint8_t)GFX_MODE_256x212_YJK) {
		return -KOS_ENOTSUP;
	}
	if (style == (const struct gfx_text_style *)0 ||
	    (text == (const char *)0 && length != 0u)) {
		return -KOS_EINVAL;
	}
	font = style->font;
	if (font != GFX_FONT_6X8 && font != GFX_FONT_4X6) {
		return -KOS_EINVAL;
	}
	if ((style->flags & ~(uint8_t)GFX_TEXT_TRANSPARENT) != 0u) {
		return -KOS_EINVAL;
	}
	g6 = gfx_state.mode == (uint8_t)GFX_MODE_512x212_16 ? 1u : 0u;
	if (g6 != 0u) {
		if ((style->fg | style->bg) & 0xF0u) {
			return -KOS_EINVAL;
		}
	}
	if ((style->flags & GFX_TEXT_TRANSPARENT) != 0u) {
		return gfx_atlas_draw(x, y, text, length, style->fg);
	}
	/* Opaque: the stream writes whole bytes, so in GRAPHIC 6 x is
	 * byte-aligned (the 4x6's columns are always so). */
	if (g6 != 0u && (x & 1u) != 0u) {
		return -KOS_EINVAL;
	}
	width = gfx_text_font_width(font);
	height = gfx_text_font_height(font);
	if (y > 512u - height) {
		return -KOS_EINVAL;
	}
	if (x >= gfx_state.width) {
		return -KOS_EINVAL;
	}
	if (gfx_flush() < 0) {
		return -KOS_EINVAL;
	}
	/* The stream's bytes must land after every queued command: wait
	 * for the engine once. */
	gfx_v9958_io_wait_command();
	fit = (gfx_state.width - x) / width;
	if (fit > length) {
		fit = length;
	}
	ram = gfx_text_font_ram(font);
	stride = gfx_text_font_stride(font);
	col = g6 != 0u ? (uint16_t)(x / 2u) : x;
	for (row_index = 0u; row_index < height; ++row_index) {
		struct gfx_text_row_args args;
		uint16_t offset = (uint16_t)((uint16_t)(y + row_index) *
					     256u + col);
		uint8_t r14 = (uint8_t)(((offset >> 14) & 0x03u) |
					(((y + row_index) >> 8) << 2));

		gfx_v9958_io_set_vram(r14, offset);
		args.text = (const uint8_t *)text;
		args.font = ram + row_index;
		args.table0 = (const uint8_t *)0;
		args.table1 = (const uint8_t *)0;
		args.table2 = (const uint8_t *)0;
		args.count = (uint8_t)fit;
		args.fg = style->fg;
		args.bg = style->bg;
		if (g6 != 0u) {
			if (font == GFX_FONT_4X6) {
				args.table0 = gfx_font4_pair(style->fg,
							     style->bg);
				args.mode = 1u;
			} else {
				struct gfx_text_cache_entry *entry =
					gfx_text_pair(style->fg,
						      style->bg);

				args.table0 = entry->pages[0];
				args.table1 = entry->pages[1];
				args.table2 = entry->pages[2];
				args.mode = 0u;
			}
		} else {
			args.mode = font == GFX_FONT_4X6 ? 3u : 2u;
		}
		gfx_v9958_io_text_row(&args);
	}
	return 0;
}

int gfx_text(uint16_t x, uint16_t y, const char *text,
	     const struct gfx_text_style *style)
{
	return gfx_text_run(x, y, text,
			    text != (const char *)0 ?
			    (uint16_t)strlen(text) : 0u, style);
}

int gfx_text_n(uint16_t x, uint16_t y, const char *text, uint16_t length,
	       const struct gfx_text_style *style)
{
	return gfx_text_run(x, y, text, length, style);
}

uint16_t gfx_text_width(const char *text, enum gfx_font font)
{
	uint8_t width;

	if (text == (const char *)0) {
		return 0u;
	}
	if (font == GFX_FONT_4X6) {
		width = GFX_FONT4_GLYPH_WIDTH;
	} else if (font == GFX_FONT_6X8) {
		width = GFX_FONT_GLYPH_WIDTH;
	} else {
		return 0u;
	}
	return (uint16_t)(strlen(text) * width);
}
