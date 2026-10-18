/* libtui transaction boundaries and the dirty/run output backend.
 *
 * tui_begin() increases a bounded nesting count.  tui_end() decreases it
 * and flushes only when it closes the outermost transaction.  tui_flush()
 * always flushes explicitly, inside a transaction too.
 *
 * A flush scans the dirty map in row-major order, coalesces adjacent
 * dirty cells of one attribute into a horizontal run, and emits one
 * cursor position, one SGR only when the physical attribute changes, and
 * the run as one console write.  It then synchronizes the logical cursor
 * and visibility and finishes with one console_flush.  Dirty state and
 * the tracked physical state are kept on failure.
 */

#include "tui_internal.h"
#include "kos/console.h"

#define TUI_SGR_NORMAL "\033[0m"
#define TUI_SGR_REVERSE "\033[7m"
#define TUI_CURSOR_SHOW "\033[?25h"
#define TUI_CURSOR_HIDE "\033[?25l"

/* Append a small decimal value (row or column, at most two digits). */
static uint8_t tui_append_dec(uint8_t at, uint8_t value)
{
	uint8_t tens = 0u;

	while (value >= 10u) {
		value = (uint8_t)(value - 10u);
		++tens;
	}
	if (tens != 0u) {
		tui_run[at] = (uint8_t)('0' + tens);
		++at;
	}
	tui_run[at] = (uint8_t)('0' + value);
	++at;
	return at;
}

/* Append one 1-based CSI row;column H to the run buffer. */
static uint8_t tui_append_cursor(uint8_t at, uint8_t row, uint8_t col)
{
	tui_run[at++] = 0x1Bu;
	tui_run[at++] = (uint8_t)'[';
	at = tui_append_dec(at, row);
	tui_run[at++] = (uint8_t)';';
	at = tui_append_dec(at, col);
	tui_run[at++] = (uint8_t)'H';
	return at;
}

static uint8_t tui_append_text(uint8_t at, const char *text,
			       uint8_t length)
{
	uint8_t i;

	for (i = 0u; i < length; ++i) {
		tui_run[at++] = (uint8_t)text[i];
	}
	return at;
}

/* Emit one horizontal run and advance the tracked physical cursor. */
static int tui_emit_run(uint8_t row, uint8_t col, uint8_t length,
			uint8_t attr, uint8_t *phys_x, uint8_t *phys_y,
			uint8_t *phys_attr)
{
	uint16_t index = TUI_CELL_INDEX(col, row);
	uint8_t at = 0u;
	uint8_t i;
	int result;

	at = tui_append_cursor(at, (uint8_t)(row + 1u),
			       (uint8_t)(col + 1u));
	if (attr != *phys_attr) {
		if (attr == 0u) {
			at = tui_append_text(at, TUI_SGR_NORMAL,
					     sizeof(TUI_SGR_NORMAL) - 1u);
		} else {
			at = tui_append_text(at, TUI_SGR_REVERSE,
					     sizeof(TUI_SGR_REVERSE) - 1u);
		}
		*phys_attr = attr;
	}
	for (i = 0u; i < length; ++i) {
		tui_run[at] = tui_cells[index].ch;
		++at;
		++index;
	}
	result = console_write((const char *)tui_run, at);
	if (result < 0) {
		return result;
	}
	*phys_x = (uint8_t)(col + length);
	if (*phys_x >= TUI_LOGICAL_COLS) {
		*phys_x = TUI_LOGICAL_COLS - 1u;
	}
	*phys_y = row;
	return 0;
}

static void tui_clear_dirty(void)
{
	uint16_t i;

	for (i = 0u; i < TUI_DIRTY_BYTES; ++i) {
		tui_dirty[i] = 0u;
	}
}

int tui_flush(void)
{
	struct tui_rect *window = &tui_state.window;
	uint8_t phys_x;
	uint8_t phys_y;
	uint8_t phys_attr;
	uint8_t phys_visible;
	uint8_t row;
	uint8_t col;
	uint8_t start;
	uint8_t attr;
	uint16_t index;
	int result;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}

	phys_x = tui_state.phys_x;
	phys_y = tui_state.phys_y;
	phys_attr = tui_state.phys_attr;
	phys_visible = tui_state.phys_cursor_visible;

	for (row = 0u; row < tui_state.screen.rows; ++row) {
		col = 0u;
		while (col < tui_state.screen.cols) {
			index = TUI_CELL_INDEX(col, row);
			if (!TUI_DIRTY_TEST(index)) {
				++col;
				continue;
			}
			attr = tui_cells[index].attr;
			start = col;
			do {
				++col;
				if (col >= tui_state.screen.cols) {
					break;
				}
				index = TUI_CELL_INDEX(col, row);
			} while (TUI_DIRTY_TEST(index) &&
				 tui_cells[index].attr == attr);
			result = tui_emit_run(row, start,
					      (uint8_t)(col - start), attr,
					      &phys_x, &phys_y, &phys_attr);
			if (result < 0) {
				return result;
			}
		}
	}

	/* Park the physical cursor on the logical one, clamped where the
	 * model allows a cursor just past the right edge.  Emit visibility
	 * only when it differs. */
	col = tui_state.cursor_x;
	if (col >= window->w) {
		col = (uint8_t)(window->w - 1u);
	}
	row = tui_state.cursor_y;
	if (row >= window->h) {
		row = (uint8_t)(window->h - 1u);
	}
	col = (uint8_t)(window->x + col);
	row = (uint8_t)(window->y + row);
	if (col != phys_x || row != phys_y) {
		uint8_t at = tui_append_cursor(0u, (uint8_t)(row + 1u),
					       (uint8_t)(col + 1u));

		result = console_write((const char *)tui_run, at);
		if (result < 0) {
			return result;
		}
		phys_x = col;
		phys_y = row;
	}
	if (phys_visible != tui_state.cursor_visible) {
		const char *sequence;
		uint8_t length;

		if (tui_state.cursor_visible != 0u) {
			sequence = TUI_CURSOR_SHOW;
			length = sizeof(TUI_CURSOR_SHOW) - 1u;
			phys_visible = 1u;
		} else {
			sequence = TUI_CURSOR_HIDE;
			length = sizeof(TUI_CURSOR_HIDE) - 1u;
			phys_visible = 0u;
		}
		result = console_write(sequence, length);
		if (result < 0) {
			return result;
		}
	}

	result = console_flush();
	if (result < 0) {
		return result;
	}
	tui_clear_dirty();
	tui_state.phys_x = phys_x;
	tui_state.phys_y = phys_y;
	tui_state.phys_attr = phys_attr;
	tui_state.phys_cursor_visible = phys_visible;
	return 0;
}

void tui_begin(void)
{
	if (tui_state.initialized == 0u) {
		return;
	}
	if (tui_state.transaction_depth < TUI_TRANSACTION_MAX) {
		++tui_state.transaction_depth;
	}
}

int tui_end(void)
{
	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	if (tui_state.transaction_depth == 0u) {
		return -KOS_EINVAL;
	}
	--tui_state.transaction_depth;
	if (tui_state.transaction_depth != 0u) {
		return 0;
	}
	return tui_flush();
}
