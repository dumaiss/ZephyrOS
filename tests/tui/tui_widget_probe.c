/* Render-only widget probe used by the archive/link tests.
 *
 * It calls status/label/button but no menu, input or modal member, so its
 * map proves the modal save buffer and interaction members are absent.
 */

#include "kos.h"
#include "tui.h"
#include "tui_widgets.h"

int main(void)
{
	if (tui_init() < 0) {
		printf("tui: widget probe FAIL init\r\n");
		return 1;
	}
	if (tui_statusbar("probe") != 0 ||
	    tui_label(0, 0, "widget probe") != 0 ||
	    tui_button(0, 1, "OK", TUI_ID_OK, 1u) != 0 ||
	    tui_flush() < 0) {
		printf("tui: widget probe FAIL draw\r\n");
		tui_shutdown();
		return 1;
	}
	tui_shutdown();
	printf("tui: widget probe pass\r\n");
	return 0;
}
