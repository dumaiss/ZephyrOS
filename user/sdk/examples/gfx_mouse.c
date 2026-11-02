/* KonaOS SDK example: the libgfx pointer driven by the mouse.
 *
 * Built by the examples Makefile with `LIBS=gfx`.  The program holds the
 * display in the 512 x 212 16-colour mode and shows libgfx's own pointer
 * (gfx_pointer_show): the mouse is tracked over 512 x 212, the left
 * button paints under the pointer, the right button switches to the
 * 256 x 212 256-colour mode and back (the pointer follows: there its x
 * is the tracked x / 2), and any key exits.  The mode set takes the
 * pointer down with the band its tables use; the next show puts it back.
 */

#include "kos.h"
#include "kos/gfx.h"
#include "kos/mouse.h"

#define GFX_MOUSE_WIDTH  512
#define GFX_MOUSE_HEIGHT 212
#define GFX_MOUSE_BRUSH  4

static const struct gfx_text_style style_g6 = {0x0Fu, 0x04u, 0u, 0u};
static const struct gfx_text_style style_g7 = {0xFFu, 0x00u, 0u, 0u};

/* Set the mode, clear it and title it; the display comes on last. */
static int gfx_mouse_mode(uint8_t g7)
{
	int result;

	result = gfx_set_mode(g7 != 0u ? GFX_MODE_256x212_256 :
			      GFX_MODE_512x212_16);
	if (result < 0) {
		return result;
	}
	(void)gfx_clear(g7 != 0u ? 0x00u : 0x04u);
	(void)gfx_text(4u, 2u, "left paints, right: mode, a key exits",
		       g7 != 0u ? &style_g7 : &style_g6);
	return gfx_display_enable(1u);
}

int main(void)
{
	struct mouse m;
	uint8_t g7 = 0u;
	int handle;
	int result;
	int width;
	int x;
	int y;

	handle = mouse_open();
	if (handle < 0) {
		printf("gfx_mouse: no mouse %d\r\n", handle);
		return 1;
	}
	result = gfx_open();
	if (result < 0) {
		printf("gfx_mouse: open %d\r\n", result);
		return 1;
	}
	result = gfx_mouse_mode(g7);
	if (result < 0) {
		gfx_close();
		printf("gfx_mouse: mode %d\r\n", result);
		return 1;
	}
	mouse_init(&m, handle, GFX_MOUSE_WIDTH, GFX_MOUSE_HEIGHT);
	while (console_poll() == 0) {
		if (mouse_track(&m) < 0) {
			break;
		}
		if ((m.pressed & MOUSE_RIGHT) != 0u) {
			g7 ^= 1u;
			if (gfx_mouse_mode(g7) < 0) {
				break;
			}
		}
		width = g7 != 0u ? GFX_MOUSE_WIDTH / 2 : GFX_MOUSE_WIDTH;
		x = g7 != 0u ? m.x >> 1 : m.x;
		y = m.y;
		if ((m.buttons & MOUSE_LEFT) != 0u) {
			(void)gfx_fill_rect(
				(uint16_t)(x < width - GFX_MOUSE_BRUSH ?
					   x : width - GFX_MOUSE_BRUSH),
				(uint16_t)(y < GFX_MOUSE_HEIGHT - GFX_MOUSE_BRUSH ?
					   y : GFX_MOUSE_HEIGHT - GFX_MOUSE_BRUSH),
				GFX_MOUSE_BRUSH, GFX_MOUSE_BRUSH,
				g7 != 0u ? 0xFCu : 0x0Au, GFX_OP_COPY);
		}
		m.pressed = 0u;
		(void)gfx_pointer_show(x, y);
	}
	if (console_poll() > 0) {
		(void)getchar();
	}
	gfx_close();
	printf("gfx_mouse: done\r\n");
	return 0;
}
