/* KonaOS SDK example: symbolic report of every decoded TUI key.
 *
 * Built by the examples Makefile with `LIBS=tui`.  Press keys; ESC exits.
 */

#include "kos.h"
#include "tui.h"

static const char *key_name(tui_key_t key)
{
	switch (key) {
	case TUI_KEY_NONE:
		return "NONE";
	case TUI_KEY_ESC:
		return "ESC";
	case TUI_KEY_UP:
		return "UP";
	case TUI_KEY_DOWN:
		return "DOWN";
	case TUI_KEY_LEFT:
		return "LEFT";
	case TUI_KEY_RIGHT:
		return "RIGHT";
	case TUI_KEY_HOME:
		return "HOME";
	case TUI_KEY_END:
		return "END";
	case TUI_KEY_PGUP:
		return "PGUP";
	case TUI_KEY_PGDN:
		return "PGDN";
	case TUI_KEY_INSERT:
		return "INSERT";
	case TUI_KEY_DELETE:
		return "DELETE";
	case TUI_KEY_F1:
		return "F1";
	case TUI_KEY_F2:
		return "F2";
	case TUI_KEY_F3:
		return "F3";
	case TUI_KEY_F4:
		return "F4";
	case TUI_KEY_F5:
		return "F5";
	case TUI_KEY_F6:
		return "F6";
	case TUI_KEY_F7:
		return "F7";
	case TUI_KEY_F8:
		return "F8";
	case TUI_KEY_F9:
		return "F9";
	case TUI_KEY_F10:
		return "F10";
	case TUI_KEY_F11:
		return "F11";
	case TUI_KEY_F12:
		return "F12";
	default:
		return "BYTE";
	}
}

int main(void)
{
	tui_key_t key;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("TUI KEYS FAIL init %d\r\n", result);
		return 1;
	}
	if (tui_puts_xy(0u, 0u, "Press keys; ESC exits.") != 22 ||
	    tui_flush() < 0) {
		printf("TUI KEYS FAIL draw\r\n");
		tui_shutdown();
		return 1;
	}
	for (;;) {
		key = tui_getkey();
		if (key == TUI_KEY_ESC) {
			break;
		}
		printf("%04x %s\r\n", (unsigned int)key, key_name(key));
	}
	tui_shutdown();
	printf("TUI KEYS PASS\r\n");
	return 0;
}
