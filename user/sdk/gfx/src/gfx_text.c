/* KonaOS libgfx: text (docs/gfx-sdk.md sections 13 and 14).
 *
 * Two stages, kept apart behind a sink interface: the expansion turns
 * a glyph's eight font rows into pixel bytes in the style's colours
 * (in GRAPHIC 6 three bytes per row through the pair's three
 * 256-entry tables, the most recent pairs cached; in GRAPHIC 7 six
 * bytes per row, one per pixel), and the sink places them.  The
 * opaque sink writes each glyph row through the data port with one
 * VRAM address per row, so a string is one run with no command
 * engine.  A transparent sink is one more emit function (see
 * gfx_internal.h): it would start one LMMC per string with TIMP set
 * and R#17's auto-increment off and feed the expanded rows through
 * R#44, the chip skipping colour-0 pixels - the font, the expansion
 * and the text calls stay as they are.
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

/* The cache slot for a colour pair: reuse a matching slot, otherwise
 * build one in the round-robin slot. */
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

/* Expand one font row byte into the style's pixels; returns the row
 * width in bytes (3 in G6, 6 in G7). */
uint8_t gfx_text_expand_row(uint8_t font_row, uint8_t fg, uint8_t bg,
			    uint8_t *row)
{
	uint8_t bit;

	if (gfx_state.mode == (uint8_t)GFX_MODE_512x212_16) {
		const struct gfx_text_cache_entry *entry =
			gfx_text_pair(fg, bg);

		row[0] = entry->pages[0][font_row];
		row[1] = entry->pages[1][font_row];
		row[2] = entry->pages[2][font_row];
		return 3u;
	}
	for (bit = 0u; bit < 6u; ++bit) {
		row[bit] = (font_row & (0x80u >> bit)) != 0u ? fg : bg;
	}
	return 6u;
}

/* The opaque sink: one VRAM address per glyph row, the expanded bytes
 * streamed through the data port. */
static void gfx_text_opaque_emit(const struct gfx_text_sink *sink,
				 uint16_t col, uint16_t y,
				 const uint8_t *row, uint8_t row_bytes)
{
	uint16_t offset = (uint16_t)((uint16_t)y * 256u + col);
	uint8_t r14 = (uint8_t)(((offset >> 14) & 0x03u) |
				((y >> 8) << 2));

	(void)sink;
	gfx_v9958_io_set_vram(r14, offset);
	gfx_v9958_io_stream_vram(row, row_bytes);
}

const struct gfx_text_sink gfx_text_sink_opaque = {
	gfx_text_opaque_emit
};

/* The shared run: validation, the batch flush, then every glyph
 * through the expansion and the sink, whole-glyph clipped at the
 * right edge. */
static int gfx_text_run(uint16_t x, uint16_t y, const char *text,
			uint16_t length,
			const struct gfx_text_style *style,
			const struct gfx_text_sink *sink)
{
	uint8_t g6;
	uint8_t row[6];
	uint16_t i;

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
	if (y > 512u - GFX_FONT_GLYPH_HEIGHT) {
		return -KOS_EINVAL;
	}
	g6 = gfx_state.mode == (uint8_t)GFX_MODE_512x212_16 ? 1u : 0u;
	if (g6 != 0u) {
		if ((x & 1u) != 0u) {
			return -KOS_EINVAL;
		}
		if ((style->fg | style->bg) & 0xF0u) {
			return -KOS_EINVAL;
		}
	}
	if (x >= gfx_state.width) {
		return -KOS_EINVAL;
	}
	if (gfx_flush() < 0) {
		return -KOS_EINVAL;
	}
	for (i = 0u; i < length; ++i) {
		uint16_t col = g6 != 0u ?
			(uint16_t)((x + i * GFX_FONT_GLYPH_WIDTH) / 2u) :
			(uint16_t)(x + i * GFX_FONT_GLYPH_WIDTH);
		uint8_t code = (uint8_t)text[i];
		uint8_t row_index;

		/* The mode's width is 256 bytes in either mode: a glyph
		 * whose row bytes cross the edge is not drawn. */
		if ((uint16_t)(col + (g6 != 0u ? 3u : 6u)) > 256u) {
			break;
		}
		for (row_index = 0u; row_index < GFX_FONT_GLYPH_BYTES;
		     ++row_index) {
			uint8_t count = gfx_text_expand_row(
				gfx_state.font_ram[(uint16_t)code *
						   GFX_FONT_GLYPH_BYTES +
						   row_index],
				style->fg, style->bg, row);

			sink->emit(sink, col, (uint16_t)(y + row_index),
				   row, count);
		}
	}
	return 0;
}

int gfx_text(uint16_t x, uint16_t y, const char *text,
	     const struct gfx_text_style *style)
{
	return gfx_text_run(x, y, text,
			    text != (const char *)0 ?
			    (uint16_t)strlen(text) : 0u, style,
			    &gfx_text_sink_opaque);
}

int gfx_text_n(uint16_t x, uint16_t y, const char *text, uint16_t length,
	       const struct gfx_text_style *style)
{
	return gfx_text_run(x, y, text, length, style,
			    &gfx_text_sink_opaque);
}

uint16_t gfx_text_width(const char *text)
{
	if (text == (const char *)0) {
		return 0u;
	}
	return (uint16_t)(strlen(text) * GFX_FONT_GLYPH_WIDTH);
}
