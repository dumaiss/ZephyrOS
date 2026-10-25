/* KonaOS libgfx: the built-in 4x6 CP850 font and its mutable RAM
 * copy (docs/gfx-sdk.md section 12).
 *
 * The const table is the pinned CP850 4x6 asset
 * (`drivers/v9958/assets/font_cp850_4x6.inc`), generated at build
 * time by tools/fontpack.py into `font4_builtin.inc` - 256 glyphs in
 * code order, six row bytes each (active pixels in bits 7..4) with
 * two pad bytes, so every glyph sits at the same 8-byte stride as the
 * 6x8 font and the text row loop addresses both with one shift.  The
 * RAM copy is made at the font's first use; the glyph calls change
 * that copy, so a redefined glyph is what the next draw uses.
 *
 * The G6 expansion's pair table: 32 bytes per colour pair (two output
 * bytes for each of the font nibble's 16 values), the most recent
 * pairs cached round-robin - eight pairs, 256 bytes (recorded in the
 * package's validation record).
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

/* The RAM copy, made at first use. */
static uint8_t gfx_font4_ram_bytes[GFX_FONT4_SIZE];
static uint8_t gfx_font4_active;

/* The G6 pair-table cache: GFX_FONT4_PAIR_ENTRIES recent colour
 * pairs, one 32-byte table each. */
#define GFX_FONT4_PAIR_ENTRIES 8u

struct gfx_font4_pair_entry {
	uint8_t active;            /* 1 after the first build */
	uint8_t fg;
	uint8_t bg;
	uint8_t table[32];
};

static struct gfx_font4_pair_entry gfx_font4_pairs[GFX_FONT4_PAIR_ENTRIES];
static uint8_t gfx_font4_pair_next;

/* Build one pair's table: for each font nibble (pixels 3..0), the
 * cell's two bytes - pixels 3 and 2 into byte 0, 1 and 0 into byte
 * 1, each nibble the foreground or the background colour. */
static void gfx_font4_build_pair(struct gfx_font4_pair_entry *entry)
{
	uint8_t index;

	for (index = 0u; index < 16u; ++index) {
		uint8_t hi = (index & 0x08u) != 0u ? entry->fg
			: entry->bg;
		uint8_t lo = (index & 0x04u) != 0u ? entry->fg
			: entry->bg;

		entry->table[index * 2u] = (uint8_t)((hi << 4) | lo);
		hi = (index & 0x02u) != 0u ? entry->fg : entry->bg;
		lo = (index & 0x01u) != 0u ? entry->fg : entry->bg;
		entry->table[index * 2u + 1u] =
			(uint8_t)((hi << 4) | lo);
	}
}

const uint8_t *gfx_font4_builtin(void)
{
	return gfx_font4_const;
}

/* The RAM copy, made at the font's first use. */
void gfx_font4_install(void)
{
	if (gfx_font4_active == 0u) {
		memcpy(gfx_font4_ram_bytes, gfx_font4_const, GFX_FONT4_SIZE);
		gfx_font4_active = 1u;
	}
}

uint8_t *gfx_font4_ram(void)
{
	gfx_font4_install();
	return gfx_font4_ram_bytes;
}

int gfx_font4_set_glyph(uint8_t code, const uint8_t rows[6])
{
	uint8_t row;

	if (rows == (const uint8_t *)0) {
		return -KOS_EINVAL;
	}
	gfx_font4_install();
	/* The argument's four pixels live in the low four bits; the font
	 * holds them in bits 7..4. */
	for (row = 0u; row < GFX_FONT4_GLYPH_BYTES; ++row) {
		gfx_font4_ram_bytes[(uint16_t)code * GFX_FONT4_GLYPH_STRIDE +
			      row] = (uint8_t)(rows[row] << 4);
	}
	return 0;
}

int gfx_font4_reset_glyph(uint8_t code)
{
	uint8_t row;

	gfx_font4_install();
	for (row = 0u; row < GFX_FONT4_GLYPH_BYTES; ++row) {
		gfx_font4_ram_bytes[(uint16_t)code * GFX_FONT4_GLYPH_STRIDE +
			      row] =
			gfx_font4_const[(uint16_t)code *
					  GFX_FONT4_GLYPH_STRIDE + row];
	}
	return 0;
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
			gfx_state.font_stale4[code >> 3] |=
				(uint8_t)(1u << (code & 7u));
		}
		return result;
	}
	if (font != GFX_FONT_6X8) {
		return -KOS_EINVAL;
	}
	result = gfx_font_set_glyph6(code, rows);
	if (result == 0) {
		gfx_state.font_stale6[code >> 3] |=
			(uint8_t)(1u << (code & 7u));
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
			gfx_state.font_stale4[code >> 3] |=
				(uint8_t)(1u << (code & 7u));
		}
		return result;
	}
	if (font != GFX_FONT_6X8) {
		return -KOS_EINVAL;
	}
	result = gfx_font_reset_glyph6(code);
	if (result == 0) {
		gfx_state.font_stale6[code >> 3] |=
			(uint8_t)(1u << (code & 7u));
	}
	return result;
}

void gfx_font_reset(enum gfx_font font)
{
	uint8_t i;

	if (font == GFX_FONT_4X6) {
		memcpy(gfx_font4_ram(), gfx_font4_builtin(),
		       GFX_FONT4_SIZE);
		for (i = 0u; i < 32u; ++i) {
			gfx_state.font_stale4[i] = 0xFFu;
		}
		return;
	}
	gfx_font_reset6();
	for (i = 0u; i < 32u; ++i) {
		gfx_state.font_stale6[i] = 0xFFu;
	}
}

/* The pair's table: a matching cache entry, or a fresh round-robin
 * one. */
const uint8_t *gfx_font4_pair(uint8_t fg, uint8_t bg)
{
	struct gfx_font4_pair_entry *entry;
	uint8_t i;

	for (i = 0u; i < GFX_FONT4_PAIR_ENTRIES; ++i) {
		entry = &gfx_font4_pairs[i];
		if (entry->active != 0u && entry->fg == fg &&
		    entry->bg == bg) {
			return entry->table;
		}
	}
	entry = &gfx_font4_pairs[gfx_font4_pair_next];
	gfx_font4_pair_next =
		(uint8_t)(gfx_font4_pair_next + 1u >= GFX_FONT4_PAIR_ENTRIES ?
			  0u : gfx_font4_pair_next + 1u);
	entry->fg = fg;
	entry->bg = bg;
	gfx_font4_build_pair(entry);
	entry->active = 1u;
	return entry->table;
}
