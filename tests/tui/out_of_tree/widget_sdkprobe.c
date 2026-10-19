/* Out-of-tree widget program.
 *
 * Built beside an unpacked standalone SDK with `LIBS=tui_widgets:tui`.
 * It proves the optional archive and header ship, resolve in dependency
 * order, and expose only public calls.
 */

#include "kos.h"
#include "tui.h"
#include "tui_widgets.h"

int main(void)
{
	char name[8] = "SDK";
	uint8_t checked = 1u;

	if (tui_init() < 0) {
		printf("WIDGET SDK FAIL init\r\n");
		return 1;
	}
	if (tui_statusbar("SDK") != 0 ||
	    tui_label(0, 0, "widget") != 0 ||
	    tui_button(0, 1, "OK", TUI_ID_OK, 1u) != 0 ||
	    tui_checkbox(0, 2, "on", &checked) != 0 ||
	    tui_input(0, 3, 8u, name, 8u) != 0 ||
	    tui_flush() < 0) {
		printf("WIDGET SDK FAIL draw\r\n");
		tui_shutdown();
		return 1;
	}
	tui_shutdown();
	printf("WIDGET SDK PASS\r\n");
	return 0;
}
