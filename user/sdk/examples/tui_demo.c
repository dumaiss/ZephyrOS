/* KonaOS SDK example: framed panels, a nested window, styles and a key loop.
 *
 * Built by the examples Makefile with `LIBS=tui` and the shipped SDK only.
 * Run it, then press q or ESC.  Layout comes from tui_get_size().
 */

#include "kos.h"
#include "tui.h"

static int demo_fail(const char *where, int result)
{
	printf("TUI DEMO FAIL %s %d\r\n", where, result);
	tui_shutdown();
	return 1;
}

int main(void)
{
	struct tui_size size;
	struct tui_rect frame;
	struct tui_rect inner;
	tui_key_t key;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("TUI DEMO FAIL init %d\r\n", result);
		return 1;
	}
	result = tui_get_size(&size);
	if (result < 0) {
		return demo_fail("size", result);
	}
	if (size.cols < 40u || size.rows < 16u) {
		return demo_fail("geometry", 0);
	}

	frame.x = 0u;
	frame.y = (uint8_t)(size.rows - 1u);
	frame.w = size.cols;
	frame.h = 1u;
	if (tui_fill(&frame, ' ', TUI_STYLE_STATUS) < 0 ||
	    tui_puts_xy(0u, frame.y, "TUI SDK demo") != 12) {
		return demo_fail("status", 0);
	}
	frame.x = 2u;
	frame.y = 2u;
	frame.w = (uint8_t)(size.cols - 4u);
	frame.h = (uint8_t)(size.rows - 5u);
	if (tui_frame_title(&frame, TUI_FRAME_DOUBLE, TUI_STYLE_TITLE,
			    "KonaOS TUI") < 0) {
		return demo_fail("frame", 0);
	}
	if (tui_hline((uint8_t)(frame.x + 2u),
		      (uint8_t)(frame.y + frame.h - 2u),
		      (uint8_t)(frame.w - 4u), TUI_FRAME_SINGLE) < 0) {
		return demo_fail("rule", 0);
	}

	inner.x = (uint8_t)(frame.x + 2u);
	inner.y = (uint8_t)(frame.y + 2u);
	inner.w = (uint8_t)(frame.w - 4u);
	inner.h = (uint8_t)(frame.h - 4u);
	if (tui_push_window(&inner) < 0) {
		return demo_fail("push", 0);
	}
	if (tui_set_style(TUI_STYLE_SELECTED) < 0 ||
	    tui_puts_xy(0u, 0u, "TUI SDK v1") != 10 ||
	    tui_set_style(TUI_STYLE_NORMAL) < 0 ||
	    tui_puts_xy(0u, 2u, "Frames, windows, styles and keys.") != 33 ||
	    tui_puts_xy(0u, 4u, "Press q or ESC to exit.") != 23) {
		return demo_fail("content", 0);
	}
	if (tui_pop_window() < 0) {
		return demo_fail("pop", 0);
	}
	if (tui_flush() < 0) {
		return demo_fail("flush", 0);
	}
	printf("TUI DEMO READY\r\n");

	for (;;) {
		key = tui_getkey();
		if (key == TUI_KEY_ESC || key == (tui_key_t)'q' ||
		    key == (tui_key_t)'Q') {
			break;
		}
	}
	tui_shutdown();
	printf("TUI DEMO PASS\r\n");
	return 0;
}
