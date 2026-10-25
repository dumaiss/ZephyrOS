/* KonaOS libgfx: the built-in font and its mutable RAM copy
 * (docs/gfx-sdk.md section 12).
 *
 * The const table is the pinned CP850 6x8 asset (the console font's
 * source, `drivers/v9958/assets/font_cp850_6x8.inc`), generated at
 * build time by tools/fontpack.py into `font_builtin.inc` - 256
 * glyphs in code order, eight row bytes each, active pixels in bits
 * 7..2.  gfx_open installs it into the RAM font; the glyph calls
 * change that copy, so a redefined glyph is what the next draw uses
 * and there is no VRAM cache.
 *
 * The RAM font is row-major for the text loop (gfx_text.asm): one
 * 256-byte page per glyph row, indexed by the code, each byte the
 * row's six pixels (bit 5 the left) times 4 - the offset of the pixel
 * pattern's entry in the colour pair's expansion table.  This file
 * also owns the 256-byte-aligned page area that holds both fonts'
 * pages and the expansion tables.
 *
 * No VIDEO IPC.
 */

#include "kos.h"

#include "kos/gfx.h"
#include "gfx_internal.h"

/* The built-in font, byte-identical to the console's pinned asset. */
static const uint8_t gfx_font_builtin[GFX_FONT_SIZE] = {
#include "font_builtin.inc"
};

/* The page area: GFX_TEXT_PAGES pages, aligned at run time. */
static uint8_t gfx_page_area[GFX_TEXT_PAGES * 256u + 255u];

uint8_t gfx_font6_page;
uint8_t gfx_font4_page;
uint8_t gfx_pair_page;

/* The 6x8 row byte for a const row byte: the pixels (bits 7..2) are
 * already the pattern times 4. */
#define GFX_FONT6_ROW(byte) ((uint8_t)((byte) & 0xFCu))

static uint8_t *gfx_font6_row_page(uint8_t row)
{
	return (uint8_t *)((uint16_t)(gfx_font6_page + row) << 8);
}

void gfx_font_install(void)
{
	uint16_t base = ((uint16_t)gfx_page_area + 255u) & 0xFF00u;

	gfx_font6_page = (uint8_t)(base >> 8);
	gfx_font4_page = (uint8_t)(gfx_font6_page + GFX_FONT6_PAGES);
	gfx_pair_page = (uint8_t)(gfx_font4_page + GFX_FONT4_PAGES);
	gfx_font_transpose6(gfx_font6_page, gfx_font_builtin);
}

/* The 6x8 glyph calls' bodies (the public gfx_font_* dispatch lives
 * in gfx_font4.c, so a program that never uses the 4x6 font links
 * neither its table nor its pages' build). */
int gfx_font_set_glyph6(uint8_t code, const uint8_t rows[8])
{
	uint8_t row;

	if (rows == (const uint8_t *)0) {
		return -KOS_EINVAL;
	}
	/* The argument's six pixels live in the low six bits. */
	for (row = 0u; row < GFX_FONT_GLYPH_BYTES; ++row) {
		gfx_font6_row_page(row)[code] = (uint8_t)(rows[row] << 2);
	}
	return 0;
}

int gfx_font_reset_glyph6(uint8_t code)
{
	uint8_t row;

	for (row = 0u; row < GFX_FONT_GLYPH_BYTES; ++row) {
		gfx_font6_row_page(row)[code] = GFX_FONT6_ROW(
			gfx_font_builtin[(uint16_t)code *
					 GFX_FONT_GLYPH_BYTES + row]);
	}
	return 0;
}

void gfx_font_reset6(void)
{
	gfx_font_install();
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
