/* KonaOS libgfx: the built-in font and its mutable RAM copy
 * (docs/gfx-sdk.md section 12).
 *
 * The const table is the pinned CP850 6x8 asset (the console font's
 * source, `drivers/v9958/assets/font_cp850_6x8.inc`), generated at
 * build time by tools/fontpack.py into `font_builtin.inc` - 256
 * glyphs in code order, eight row bytes each, active pixels in bits
 * 7..2.  gfx_open copies the table into the RAM font; the glyph calls
 * change that copy, so a redefined glyph is what the next draw uses
 * and there is no VRAM cache.
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

/* Install the built-in font into the RAM font (gfx_open calls this,
 * so opening a fresh context always starts from the built-in). */
void gfx_font_install(void)
{
	memcpy(gfx_state.font_ram, gfx_font_builtin, GFX_FONT_SIZE);
}

/* The 6x8 glyph calls' bodies (the public gfx_font_* dispatch lives
 * in gfx_font4.c, so a program that never uses the 4x6 font links
 * neither its table nor its copy). */
int gfx_font_set_glyph6(uint8_t code, const uint8_t rows[8])
{
	uint8_t row;

	if (rows == (const uint8_t *)0) {
		return -KOS_EINVAL;
	}
	/* The argument's six pixels live in the low six bits; the font
	 * holds them in bits 7..2. */
	for (row = 0u; row < GFX_FONT_GLYPH_BYTES; ++row) {
		gfx_state.font_ram[(uint16_t)code * GFX_FONT_GLYPH_BYTES +
				   row] = (uint8_t)(rows[row] << 2);
	}
	return 0;
}

int gfx_font_reset_glyph6(uint8_t code)
{
	uint8_t row;

	for (row = 0u; row < GFX_FONT_GLYPH_BYTES; ++row) {
		gfx_state.font_ram[(uint16_t)code * GFX_FONT_GLYPH_BYTES +
				   row] =
			gfx_font_builtin[(uint16_t)code *
					 GFX_FONT_GLYPH_BYTES + row];
	}
	return 0;
}

void gfx_font_reset6(void)
{
	gfx_font_install();
}
