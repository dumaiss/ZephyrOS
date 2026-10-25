/* GfxDemo: the GFX v1 completion demo (docs/gfx-sdk.md section 23).
 *
 *   gfxdemo
 *
 * GRAPHIC 6: a cleared screen, filled bars, copied rectangles, text in
 * two colour pairs, and an application-defined glyph drawn, restored
 * and drawn again.  The display comes on, a key returns the console.
 *
 * The program holds /dev/video exclusively, so its own lines are
 * printed outside the hold.
 */

#include "kos.h"
#include "kos/gfx.h"

int main(void)
{
	static const struct gfx_text_style white = {0x0Fu, 0x04u};
	static const struct gfx_text_style green = {0x0Au, 0x00u};
	/* A 6x8 "face": the low six bits of each row are the pixels. */
	static const uint8_t face[8] = {
		0x1E, 0x21, 0x21, 0x21, 0x21, 0x12, 0x0C, 0x00
	};
	int result;

	result = gfx_open();
	if (result < 0) {
		printf("gfxdemo: FAIL open %d\r\n", result);
		return 1;
	}
	result = gfx_set_mode(GFX_MODE_512x212_16);
	if (result < 0) {
		gfx_close();
		printf("gfxdemo: FAIL mode %d\r\n", result);
		return 1;
	}
	result = gfx_clear(0x04u);
	if (result < 0) {
		gfx_close();
		printf("gfxdemo: FAIL clear %d\r\n", result);
		return 1;
	}
	/* Filled bars, then a rectangle copied twice. */
	result = gfx_fill_rect(0u, 0u, 512u, 20u, 0x05u);
	result |= gfx_fill_rect(0u, 192u, 512u, 20u, 0x05u);
	result |= gfx_fill_rect(20u, 40u, 40u, 8u, 0x09u);
	result |= gfx_copy_rect(20u, 40u, 80u, 60u, 40u, 8u);
	result |= gfx_copy_rect(20u, 40u, 140u, 80u, 40u, 8u);
	if (result != 0) {
		gfx_close();
		printf("gfxdemo: FAIL prims %d\r\n", result);
		return 1;
	}
	/* Text in two colour pairs. */
	result = gfx_text(20u, 100u, "KonaOS GFX v1", &white);
	result |= gfx_text(20u, 116u, "text colours", &green);
	if (result != 0) {
		gfx_close();
		printf("gfxdemo: FAIL text %d\r\n", result);
		return 1;
	}
	/* The application-defined glyph, drawn, restored and drawn again. */
	result = gfx_font_set_glyph(0x80u, face);
	if (result < 0) {
		gfx_close();
		printf("gfxdemo: FAIL glyph %d\r\n", result);
		return 1;
	}
	result = gfx_text(20u, 140u, "\x80", &white);
	if (result < 0) {
		gfx_close();
		printf("gfxdemo: FAIL text %d\r\n", result);
		return 1;
	}
	result = gfx_font_reset_glyph(0x80u);
	if (result < 0) {
		gfx_close();
		printf("gfxdemo: FAIL glyph %d\r\n", result);
		return 1;
	}
	result = gfx_text(36u, 140u, "\x80", &white);
	if (result < 0) {
		gfx_close();
		printf("gfxdemo: FAIL text %d\r\n", result);
		return 1;
	}
	result = gfx_display_enable(1u);
	if (result < 0) {
		gfx_close();
		printf("gfxdemo: FAIL display %d\r\n", result);
		return 1;
	}
	(void)getchar(); /* the key: the only blocking point */
	gfx_close();
	printf("gfxdemo: done\r\n");
	return 0;
}
