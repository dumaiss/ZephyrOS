/* ZephyrOS ESC c display-reset check (work package 42e, test-only).
 *
 * The console integration image boots this tiny program before the
 * monitor (ZOS_BOOT_PROGRAM=/SYSTEM/escc.zex).  It writes ESC `c` - the
 * V9958 display reset, which re-runs the whole bring-up including the
 * font reload - and then a line of text through the console, so the owner
 * watching the board sees the display reset and the new line, and the
 * simulator replay checks the same bytes.  It then hands the console to
 * the unchanged monitor.
 *
 * This file is acceptance content: nothing packs it into the production
 * store.
 */

#include "zos.h"

int main(void)
{
	/* ESC c through the console output: the production console is
	 * `console-v9958-hid`, so the VIDEO driver's parser sees it and
	 * resets the display. */
	zos_puts("\x1b" "c");
	zos_puts("escc: display reset\r\n");
	(void)zos_exec("/SYSTEM/monitor.zex", "");
	return 0;
}
