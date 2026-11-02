/* KonaOS libgfx: the pointer's setup (docs/gfx-sdk.md section 15.2).
 *
 * The tables' install and the shape, in C over the port layer; the
 * show, the move and the hide are assembly from the entry point to the
 * port (backends/v9958/gfx_pointer.asm).  The shape is a RAM copy, so
 * it outlasts a mode set: the install after it uploads the copy.
 */

#include "kos.h"

#include "kos/gfx.h"
#include "gfx_internal.h"

/* The default arrow, white: the left eight columns' sixteen rows, then
 * the right eight's (sprite mode 2's 16x16 pattern order); the hot
 * spot is its top-left pixel. */
static const uint8_t gfx_pointer_arrow[GFX_POINTER_BYTES] = {
	0x80u, 0xC0u, 0xE0u, 0xF0u, 0xF8u, 0xFCu, 0xFEu, 0xFFu,
	0xFFu, 0xFFu, 0xFCu, 0xEEu, 0xCEu, 0x87u, 0x07u, 0x03u,
	0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
	0x80u, 0xC0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
};
#define GFX_POINTER_DEFAULT_COLOR 0x0Fu

static uint8_t gfx_pointer_custom;  /* 1 after gfx_pointer_shape */
static uint8_t gfx_pointer_color;
static uint8_t gfx_pointer_pattern[GFX_POINTER_BYTES];

/* The pattern and the colour table (one colour per line). */
static void gfx_pointer_upload(void)
{
	uint8_t colors[GFX_POINTER_LINES];
	uint8_t color = GFX_POINTER_DEFAULT_COLOR;
	const uint8_t *pattern = gfx_pointer_arrow;

	if (gfx_pointer_custom != 0u) {
		color = gfx_pointer_color;
		pattern = gfx_pointer_pattern;
	}
	memset(colors, color, sizeof(colors));
	gfx_v9958_io_set_vram(GFX_POINTER_A16_A14, GFX_POINTER_PATTERN_ADDR);
	gfx_v9958_io_stream_vram(pattern, GFX_POINTER_BYTES);
	gfx_v9958_io_set_vram(GFX_POINTER_A16_A14, GFX_POINTER_COLOR_ADDR);
	gfx_v9958_io_stream_vram(colors, GFX_POINTER_LINES);
}

/* The SAT as installed: slot 0 the pointer with its Y the terminator
 * (nothing drawn until the first placement, which the cache's
 * terminator mark forces), slot 1 the terminator. */
static const uint8_t gfx_pointer_sat_init[5] = {
	GFX_SAT_TERMINATOR, 0x00u, 0x00u, 0x00u, GFX_SAT_TERMINATOR
};

void gfx_pointer_install(void)
{
	gfx_v9958_io_wait_command();
	gfx_v9958_io_reg(GFX_V9958_R5_SPRITE_BASE, GFX_V9958_REG_R5);
	gfx_v9958_io_reg(GFX_V9958_R11_SPRITE_ATTR, GFX_V9958_REG_R11);
	gfx_v9958_io_reg(GFX_V9958_R6_SPRITE_PATTERN, GFX_V9958_REG_R6);
	gfx_pointer_upload();
	gfx_v9958_io_set_vram(GFX_POINTER_A16_A14, GFX_POINTER_SAT_ADDR);
	gfx_v9958_io_stream_vram(gfx_pointer_sat_init,
				 sizeof(gfx_pointer_sat_init));
	gfx_pointer_sat[0] = GFX_SAT_TERMINATOR;
	gfx_pointer_flags |= GFX_POINTER_INSTALLED;
}

int gfx_pointer_shape(const uint8_t *pattern, uint8_t color)
{
	if (gfx_draw_ready == 0u || color > GFX_POINTER_COLOR_MAX) {
		return -KOS_EINVAL;
	}
	if (pattern == (const uint8_t *)0) {
		pattern = gfx_pointer_arrow;
	}
	memcpy(gfx_pointer_pattern, pattern, GFX_POINTER_BYTES);
	gfx_pointer_color = color;
	gfx_pointer_custom = 1u;
	if ((gfx_pointer_flags & GFX_POINTER_INSTALLED) != 0u) {
		gfx_pointer_upload();
	}
	return 0;
}
