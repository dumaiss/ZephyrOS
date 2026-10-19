/* Core-only TUI probe used by the link-map tests.
 *
 * It uses lifecycle, geometry, text and flush but never the key decoder,
 * save/restore region, style primitives or widget archive.  Its map must
 * therefore omit those members.
 */

#include "kos.h"
#include "tui.h"

int main(void)
{
	struct tui_size size;

	if (tui_init() < 0) {
		printf("tui: core probe FAIL init\r\n");
		return 1;
	}
	if (tui_get_size(&size) < 0) {
		printf("tui: core probe FAIL size\r\n");
		tui_shutdown();
		return 1;
	}
	if (tui_puts_xy(0, 0, "core probe") != 10 ||
	    tui_flush() < 0) {
		printf("tui: core probe FAIL draw\r\n");
		tui_shutdown();
		return 1;
	}
	tui_shutdown();
	printf("tui: core probe pass\r\n");
	return 0;
}
