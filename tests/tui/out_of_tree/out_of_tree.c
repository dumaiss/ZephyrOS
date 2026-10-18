/* Out-of-tree TUI program.
 *
 * Built beside an unpacked standalone SDK with LIBS=tui.  It exercises
 * the lifecycle and logical geometry through the shipped header and
 * archive only.
 */

#include "kos.h"
#include "tui.h"

int main(void)
{
	struct tui_size size;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("TUI SDK FAIL init %d\r\n", result);
		return 1;
	}
	result = tui_get_size(&size);
	if (result < 0 || size.cols != 85u || size.rows != 26u) {
		printf("TUI SDK FAIL size\r\n");
		tui_shutdown();
		return 1;
	}
	tui_shutdown();
	printf("TUI SDK PASS\r\n");
	return 0;
}
