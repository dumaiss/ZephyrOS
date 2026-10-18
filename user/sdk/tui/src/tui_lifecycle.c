/* libtui console lifecycle and logical geometry.
 *
 * Initialization makes the terminal and the model agree: normal
 * rendition, clear display, home, and a visible cursor.  Shutdown restores
 * normal rendition and a visible cursor.  Both go through the ordinary
 * console transport and add no driver call, port access, lookup or
 * capability probe.
 */

#include "tui_internal.h"
#include "kos/console.h"

#define TUI_INIT_OUTPUT "\033[0m\033[2J\033[H\033[?25h"
#define TUI_SHUTDOWN_OUTPUT "\033[0m\033[?25h"

int tui_init(void)
{
	int result;

	if (tui_state.initialized != 0u) {
		return -KOS_EINVAL;
	}
	result = console_write(TUI_INIT_OUTPUT, sizeof(TUI_INIT_OUTPUT) - 1u);
	if (result < 0) {
		return result;
	}
	result = console_flush();
	if (result < 0) {
		return result;
	}
	tui_state.screen.cols = TUI_LOGICAL_COLS;
	tui_state.screen.rows = TUI_LOGICAL_ROWS;
	tui_state.transaction_depth = 0u;
	tui_state.initialized = 1u;
	return 0;
}

void tui_shutdown(void)
{
	if (tui_state.initialized == 0u) {
		return;
	}
	(void)console_write(TUI_SHUTDOWN_OUTPUT,
			    sizeof(TUI_SHUTDOWN_OUTPUT) - 1u);
	(void)console_flush();
	tui_state.transaction_depth = 0u;
	tui_state.initialized = 0u;
}

int tui_get_size(struct tui_size *out)
{
	if (out == (struct tui_size *)0) {
		return -KOS_EINVAL;
	}
	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	*out = tui_state.screen;
	return 0;
}
