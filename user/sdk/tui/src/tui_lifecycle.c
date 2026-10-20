/* libtui console lifecycle and logical geometry.
 *
 * Initialization makes the terminal and the model agree: normal
 * rendition, clear display, home, auto-wrap disabled (the model never
 * wraps), and a visible cursor; the cell map is blank and clean.  Shutdown
 * restores normal rendition, auto-wrap and a visible cursor.  Both go
 * through the ordinary console transport and add no driver call, port
 * access, lookup or capability probe.
 */

#include "tui_internal.h"
#include "kos/console.h"

#define TUI_INIT_OUTPUT "\033[0m\033[2J\033[H\033[?7l\033[?25h"
#define TUI_SHUTDOWN_OUTPUT "\033[0m\033[?7h\033[?25h"

static void tui_reset_model(void)
{
	uint16_t i;

	for (i = 0u; i < TUI_CELL_COUNT; ++i) {
		tui_cells[i].ch = 0x20u;
		tui_cells[i].attr = 0u;
	}
	for (i = 0u; i < TUI_DIRTY_BYTES; ++i) {
		tui_dirty[i] = 0u;
		tui_invert[i] = 0u;
	}
	tui_state.screen.cols = TUI_LOGICAL_COLS;
	tui_state.screen.rows = TUI_LOGICAL_ROWS;
	tui_state.window.x = 0u;
	tui_state.window.y = 0u;
	tui_state.window.w = TUI_LOGICAL_COLS;
	tui_state.window.h = TUI_LOGICAL_ROWS;
	tui_state.cursor_x = 0u;
	tui_state.cursor_y = 0u;
	tui_state.cursor_visible = 1u;
	tui_state.style = TUI_STYLE_NORMAL;
	tui_state.attr = 0u;
	tui_state.transaction_depth = 0u;
	tui_state.window_depth = 0u;
	++tui_state.key_generation;
	tui_state.phys_x = 0u;
	tui_state.phys_y = 0u;
	tui_state.phys_attr = 0u;
	tui_state.phys_cursor_visible = 1u;
	tui_state.render_support = 0u;
}

int tui_init(void)
{
	int result;

	if (tui_state.initialized != 0u) {
		return -KOS_EINVAL;
	}
	tui_reset_model();
	result = console_write(TUI_INIT_OUTPUT, sizeof(TUI_INIT_OUTPUT) - 1u);
	if (result < 0) {
		return result;
	}
	result = console_flush();
	if (result < 0) {
		return result;
	}
	tui_state.initialized = 1u;
	return 0;
}

void tui_shutdown(void)
{
	if (tui_state.initialized == 0u) {
		return;
	}
	/* Best effort: pending cells still reach the terminal before the
	 * restoration sequence, and a failure never strands the library
	 * initialized. */
	(void)tui_flush();
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
