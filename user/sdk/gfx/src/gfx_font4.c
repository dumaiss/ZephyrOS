/* KonaOS libgfx: the built-in 4x6 CP850 font and its mutable RAM copy
 * (docs/gfx-sdk.md section 12).
 *
 * The const table is the pinned CP850 4x6 asset
 * (`drivers/v9958/assets/font_cp850_4x6.inc`), generated at build
 * time by tools/fontpack.py into `font4_builtin.inc` - 256 glyphs in
 * code order, six row bytes each (active pixels in bits 7..4) with
 * two pad bytes.  The RAM copy is made at the font's first use, in the
 * text loop's row-major layout (gfx_font.c): one page per glyph row,
 * each byte the row's four pixels (bit 3 the left) times 2.  The glyph
 * calls change that copy, so a redefined glyph is what the next draw
 * uses.
 *
 * The public glyph calls' dispatch lives here too, with the redefined-
 * glyph marks the atlas reads.
 *
 * No VIDEO IPC.
 */

#include "kos.h"

#include "kos/gfx.h"
#include "gfx_internal.h"

/* The built-in 4x6 font, byte-identical to the pinned asset at an
 * 8-byte stride (rows 6 and 7 are pads). */
static const uint8_t gfx_font4_const[GFX_FONT4_SIZE] = {
#include "font4_builtin.inc"
};

static uint8_t gfx_font4_active;

uint8_t gfx_font_stale6[32];
uint8_t gfx_font_stale4[32];

/* The 4x6 row byte for a const row byte: pixels 7..4 times 2. */
#define GFX_FONT4_ROW(byte) ((uint8_t)(((byte) >> 3) & 0x1Eu))

static uint8_t *gfx_font4_row_page(uint8_t row)
{
	return (uint8_t *)((uint16_t)(gfx_font4_page + row) << 8);
}

static void gfx_font4_copy(void)
{
	gfx_font_transpose4(gfx_font4_page, gfx_font4_const);
}

/* The RAM copy, made at the font's first use. */
void gfx_font4_install(void)
{
	if (gfx_font4_active == 0u) {
		gfx_font4_copy();
		gfx_font4_active = 1u;
	}
}

int gfx_font4_set_glyph(uint8_t code, const uint8_t rows[6])
{
	uint8_t row;

	if (rows == (const uint8_t *)0) {
		return -KOS_EINVAL;
	}
	gfx_font4_install();
	/* The argument's four pixels live in the low four bits. */
	for (row = 0u; row < GFX_FONT4_GLYPH_BYTES; ++row) {
		gfx_font4_row_page(row)[code] =
			(uint8_t)((rows[row] & 0x0Fu) << 1);
	}
	return 0;
}

int gfx_font4_reset_glyph(uint8_t code)
{
	uint8_t row;

	gfx_font4_install();
	for (row = 0u; row < GFX_FONT4_GLYPH_BYTES; ++row) {
		gfx_font4_row_page(row)[code] = GFX_FONT4_ROW(
			gfx_font4_const[(uint16_t)code *
					GFX_FONT4_GLYPH_STRIDE + row]);
	}
	return 0;
}

void gfx_font4_reset(void)
{
	gfx_font4_copy();
	gfx_font4_active = 1u;
}

/* The public glyph calls' dispatch (the 6x8 bodies live in
 * gfx_font.c): every mutation marks the glyph stale in the atlases,
 * so a redefined glyph is redrawn into each atlas at its next
 * transparent use. */
int gfx_font_set_glyph(enum gfx_font font, uint8_t code,
		       const uint8_t *rows)
{
	int result;

	if (gfx_state.video_handle == 0) {
		return -KOS_EINVAL;
	}
	if (font == GFX_FONT_4X6) {
		result = gfx_font4_set_glyph(code, rows);
		if (result == 0) {
			gfx_font_stale4[code >> 3] |=
				(uint8_t)(1u << (code & 7u));
		}
		return result;
	}
	if (font != GFX_FONT_6X8) {
		return -KOS_EINVAL;
	}
	result = gfx_font_set_glyph6(code, rows);
	if (result == 0) {
		gfx_font_stale6[code >> 3] |= (uint8_t)(1u << (code & 7u));
	}
	return result;
}

int gfx_font_reset_glyph(enum gfx_font font, uint8_t code)
{
	int result;

	if (gfx_state.video_handle == 0) {
		return -KOS_EINVAL;
	}
	if (font == GFX_FONT_4X6) {
		result = gfx_font4_reset_glyph(code);
		if (result == 0) {
			gfx_font_stale4[code >> 3] |=
				(uint8_t)(1u << (code & 7u));
		}
		return result;
	}
	if (font != GFX_FONT_6X8) {
		return -KOS_EINVAL;
	}
	result = gfx_font_reset_glyph6(code);
	if (result == 0) {
		gfx_font_stale6[code >> 3] |= (uint8_t)(1u << (code & 7u));
	}
	return result;
}

void gfx_font_reset(enum gfx_font font)
{
	uint8_t i;

	if (font == GFX_FONT_4X6) {
		gfx_font4_reset();
		for (i = 0u; i < 32u; ++i) {
			gfx_font_stale4[i] = 0xFFu;
		}
		return;
	}
	gfx_font_reset6();
	for (i = 0u; i < 32u; ++i) {
		gfx_font_stale6[i] = 0xFFu;
	}
}
