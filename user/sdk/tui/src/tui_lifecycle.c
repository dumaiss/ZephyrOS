/* libtui console lifecycle and logical geometry.
 *
 * Initialization makes the terminal and the model agree: normal
 * rendition, clear display, home, auto-wrap disabled (the model never
 * wraps), and a visible cursor; the cell map is blank and clean.  The
 * screen is the grid the console reports (KCONSOLE_OP_INFO, one call at
 * init), clamped to the compiled capacity: on a larger console the
 * program uses the top-left part.  A zero dimension is refused.  Shutdown
 * restores normal rendition, auto-wrap and a visible cursor.  Both go
 * through the ordinary console transport and add no port access or
 * lookup.
 */

#include "tui_internal.h"
#include "kos/console.h"

#define TUI_INIT_OUTPUT "\033[0m\033[2J\033[H\033[?7l\033[?25h"
#define TUI_SHUTDOWN_OUTPUT "\033[0m\033[?7h\033[?25h"

static void tui_reset_model(uint8_t cols, uint8_t rows)
{
	uint16_t i;

	for (i = 0u; i < TUI_CELL_COUNT; ++i) {
		tui_chars[i] = 0x20u;
	}
	for (i = 0u; i < TUI_ATTR_BYTES; ++i) {
		tui_attrs[i] = 0u;
	}
	for (i = 0u; i < TUI_DIRTY_BYTES; ++i) {
		tui_dirty[i] = 0u;
		tui_invert[i] = 0u;
	}
	for (i = 0u; i < TUI_MAX_ROWS; ++i) {
		tui_row_pending[i] = 0u;
	}
	tui_state.screen.cols = cols;
	tui_state.screen.rows = rows;
	tui_state.window.x = 0u;
	tui_state.window.y = 0u;
	tui_state.window.w = cols;
	tui_state.window.h = rows;
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
	tui_state.model_only = 0u;
	tui_state.stride = TUI_STRIDE;
}

int tui_init(void)
{
	struct kconsole_info info;
	uint8_t cols;
	uint8_t rows;
	int result;

	if (tui_state.initialized != 0u) {
		return -KOS_EINVAL;
	}
	result = console_info(&info);
	if (result < 0) {
		return result;
	}
	cols = info.cols < TUI_MAX_COLS ? info.cols : TUI_MAX_COLS;
	rows = info.rows < TUI_MAX_ROWS ? info.rows : TUI_MAX_ROWS;
	if (cols == 0u || rows == 0u) {
		return -KOS_ENOTSUP;
	}
	tui_reset_model(cols, rows);
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
