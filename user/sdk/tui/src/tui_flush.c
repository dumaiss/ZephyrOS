/* libtui transaction boundaries and the dirty/run output backend.
 *
 * tui_begin() increases a bounded nesting count.  tui_end() decreases it
 * and flushes only when it closes the outermost transaction.  tui_flush()
 * always flushes explicitly, inside a transaction too.
 *
 * A flush scans the dirty map in row-major order, coalesces adjacent
 * dirty cells of one attribute into a horizontal run, and emits one
 * cursor position when needed, one SGR only when the physical attribute
 * changes, and the run as one console write.  It then synchronizes cursor
 * visibility and, while visible, its logical position before finishing
 * with one console_flush.  Dirty state and the tracked physical state are
 * kept on failure.
 */

#include "tui_internal.h"
#include "kos/console.h"
#include "kos/console_render.h"

#define TUI_SGR_NORMAL "\033[0m"
#define TUI_SGR_REVERSE "\033[7m"
#define TUI_CURSOR_SHOW "\033[?25h"
#define TUI_CURSOR_HIDE "\033[?25l"

/* Append a decimal row or column (1..255: a 128-column grid's columns
 * have three digits). */
static uint8_t tui_append_dec(uint8_t at, uint8_t value)
{
	uint8_t hundreds = 0u;
	uint8_t tens = 0u;

	while (value >= 100u) {
		value = (uint8_t)(value - 100u);
		++hundreds;
	}
	while (value >= 10u) {
		value = (uint8_t)(value - 10u);
		++tens;
	}
	if (hundreds != 0u) {
		tui_run[at] = (uint8_t)('0' + hundreds);
		++at;
	}
	if (hundreds != 0u || tens != 0u) {
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
		tui_run[at] = tui_chars[index];
		++at;
		++index;
	}
	result = console_write((const char *)tui_run, at);
	if (result < 0) {
		return result;
	}
	*phys_x = (uint8_t)(col + length);
	if (*phys_x >= tui_state.screen.cols) {
		*phys_x = (uint8_t)(tui_state.screen.cols - 1u);
	}
	*phys_y = row;
	return 0;
}

static void tui_clear_dirty(void)
{
	uint16_t i;

	for (i = 0u; i < TUI_DIRTY_BYTES; ++i) {
		tui_dirty[i] = 0u;
		tui_invert[i] = 0u;
	}
	for (i = 0u; i < TUI_MAX_ROWS; ++i) {
		tui_row_pending[i] = 0u;
	}
}

/* Mark every logical cell dirty (and none invert-only): the ANSI
 * backend has never seen a screen the RENDER backend owned, so the
 * first ANSI flush repaints the whole screen. */
static void tui_dirty_all(void)
{
	uint16_t i;

	for (i = 0u; i < TUI_DIRTY_BYTES; ++i) {
		tui_dirty[i] = 0xFFu;
		tui_invert[i] = 0u;
	}
	for (i = 0u; i < TUI_MAX_ROWS; ++i) {
		tui_row_pending[i] = 1u;
	}
}

static void tui_physical_cursor(uint8_t *x, uint8_t *y)
{
	uint8_t col = tui_state.cursor_x;
	uint8_t row = tui_state.cursor_y;

	if (col >= tui_state.window.w) {
		col = (uint8_t)(tui_state.window.w - 1u);
	}
	if (row >= tui_state.window.h) {
		row = (uint8_t)(tui_state.window.h - 1u);
	}
	*x = (uint8_t)(tui_state.window.x + col);
	*y = (uint8_t)(tui_state.window.y + row);
}

static int tui_flush_ansi(void)
{
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
		if (tui_row_pending[row] == 0u) {
			continue;
		}
		col = 0u;
		while (col < tui_state.screen.cols) {
			index = TUI_CELL_INDEX(col, row);
			if (!TUI_PENDING_TEST(index)) {
				++col;
				continue;
			}
			attr = tui_priv_attr(index);
			start = col;
			do {
				++col;
				if (col >= tui_state.screen.cols) {
					break;
				}
				index = TUI_CELL_INDEX(col, row);
			} while (TUI_PENDING_TEST(index) &&
				 tui_priv_attr(index) == attr);
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
	tui_physical_cursor(&col, &row);
	if (col != phys_x || row != phys_y) {
		uint8_t at = tui_append_cursor(0u,
					       (uint8_t)(row + 1u),
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

static uint8_t tui_render_start(void)
{
	tui_render[0] = KCONSOLE_RENDER_VERSION;
	tui_render[1] = 0u;
	tui_render[2] = 0u;
	tui_render[3] = 0u;
	return KCONSOLE_RENDER_HEADER_SIZE;
}

static uint8_t tui_render_add_span(uint8_t at, uint8_t op, uint8_t row,
				    uint8_t col, uint8_t count, uint8_t attr)
{
	uint16_t index = TUI_CELL_INDEX(col, row);
	uint8_t i;

	tui_render[at++] = op;
	tui_render[at++] = (uint8_t)(KCONSOLE_RENDER_SPAN_BASE + count);
	tui_render[at++] = col;
	tui_render[at++] = row;
	tui_render[at++] = attr;
	tui_render[at++] = count;
	for (i = 0u; i < count; ++i) {
		tui_render[at++] = tui_chars[index++];
	}
	return at;
}

/* Clear only records that this successful request committed. */
static void tui_render_commit(uint8_t length)
{
	uint8_t at = KCONSOLE_RENDER_HEADER_SIZE;

	while (at < length) {
		uint8_t op = tui_render[at];
		uint8_t record_length = tui_render[(uint8_t)(at + 1u)];

		if (op == KCONSOLE_RENDER_TEXT ||
		    op == KCONSOLE_RENDER_INVERT) {
			uint8_t col = tui_render[(uint8_t)(at + 2u)];
			uint8_t row = tui_render[(uint8_t)(at + 3u)];
			uint8_t count = tui_render[(uint8_t)(at + 5u)];
			uint16_t index = TUI_CELL_INDEX(col, row);

			while (count-- != 0u) {
				if (op == KCONSOLE_RENDER_TEXT) {
					TUI_DIRTY_CLEAR(index);
				} else {
					TUI_INVERT_CLEAR(index);
				}
				++index;
			}
		}
		at = (uint8_t)(at + record_length);
	}
}

static int tui_render_send(uint8_t at)
{
	uint8_t cursor_x;
	uint8_t cursor_y;
	int result;

	tui_physical_cursor(&cursor_x, &cursor_y);
	tui_render[at++] = KCONSOLE_RENDER_STATE;
	tui_render[at++] = KCONSOLE_RENDER_STATE_SIZE;
	tui_render[at++] = cursor_x;
	tui_render[at++] = cursor_y;
	tui_render[at++] = tui_state.attr;
	tui_render[at++] = tui_state.cursor_visible;
	tui_render[2] = (uint8_t)(at & 0xFFu);
	tui_render[3] = 0u;

	result = console_render(tui_render, at);
	if (result < 0) {
		/* The console's answer changed at run time: tui_flush owns
		 * the switch to the ANSI path (and the full repaint that
		 * follows a 1 -> 2 transition). */
		return result;
	}
	if (result != at) {
		return -KOS_EIO;
	}
	tui_state.render_support = 1u;
	tui_render_commit(at);
	tui_state.phys_x = cursor_x;
	tui_state.phys_y = cursor_y;
	tui_state.phys_attr = tui_state.attr;
	tui_state.phys_cursor_visible = tui_state.cursor_visible;
	return 0;
}

static int tui_flush_render(void)
{
	uint8_t at = tui_render_start();
	uint8_t kind;
	uint8_t row;
	uint8_t col;
	uint8_t cursor_x;
	uint8_t cursor_y;
	int result;

	/* Ordinary redraws first, then attribute-only toggles.  The records are
	 * absolute and non-overlapping, so ordering between the two sets is not
	 * observable; keeping INVERT spans together makes a menu move one call.
	 *
	 * The walk keeps two invariants: `bits` is the bitmap byte that holds
	 * the current cell, and `mask` is that cell's bit within it (the
	 * bitmap is linear over the cells, so both step with the cell).  A
	 * whole clean byte skips eight cells at once. */
	for (kind = KCONSOLE_RENDER_TEXT;
	     kind <= KCONSOLE_RENDER_INVERT; ++kind) {
		for (row = 0u; row < tui_state.screen.rows; ++row) {
			uint8_t *map = kind == KCONSOLE_RENDER_TEXT ?
					tui_dirty : tui_invert;
			uint16_t index = TUI_CELL_INDEX(0u, row);
			uint8_t bits = map[(uint16_t)(index >> 3)];
			uint8_t mask = (uint8_t)(1u <<
						 (uint8_t)(index & 7u));

			if (tui_row_pending[row] == 0u) {
				continue;
			}
			col = 0u;
			while (col < tui_state.screen.cols) {
				uint8_t start;
				uint8_t attr;
				uint8_t count;

				if (mask == 1u && bits == 0u) {
					/* Eight clean cells: the next
					 * byte is eight cells on. */
					col = (uint8_t)(col + 8u);
					index = TUI_CELL_INDEX(col, row);
					bits = map[(uint16_t)(index >> 3)];
					continue;
				}
				if ((bits & mask) == 0u) {
					++col;
					index = (uint16_t)(index + 1u);
					mask = (uint8_t)(mask << 1);
					if (mask == 0u) {
						mask = 1u;
						bits = map[(uint16_t)(
							index >> 3)];
					}
					continue;
				}
				start = col;
				attr = tui_priv_attr(index);
				do {
					++col;
					index = (uint16_t)(index + 1u);
					mask = (uint8_t)(mask << 1);
					if (mask == 0u) {
						mask = 1u;
						bits = map[(uint16_t)(
							index >> 3)];
					}
					if (col >= tui_state.screen.cols) {
						break;
					}
				} while ((bits & mask) != 0u &&
					 tui_priv_attr(index) == attr);
				count = (uint8_t)(col - start);
				if ((uint8_t)(at + KCONSOLE_RENDER_SPAN_BASE +
					      count + KCONSOLE_RENDER_STATE_SIZE) >
				    TUI_RENDER_MAX) {
					result = tui_render_send(at);
					if (result < 0) {
						return result;
					}
					at = tui_render_start();
				}
				at = tui_render_add_span(at, kind, row, start,
							 count, attr);
			}
		}
	}

	tui_physical_cursor(&cursor_x, &cursor_y);
	if (at != KCONSOLE_RENDER_HEADER_SIZE ||
	    cursor_x != tui_state.phys_x || cursor_y != tui_state.phys_y ||
	    tui_state.attr != tui_state.phys_attr ||
	    tui_state.cursor_visible != tui_state.phys_cursor_visible) {
		result = tui_render_send(at);
		if (result < 0) {
			return result;
		}
	}
	/* Every walked row is now committed.  Clear its pending flag so the
	 * next flush skips it; rows at or past the screen edge keep their
	 * flags because their bits stay set. */
	for (row = 0u; row < tui_state.screen.rows; ++row) {
		tui_row_pending[row] = 0u;
	}
	return 0;
}

int tui_flush(void)
{
	int result;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	if (tui_state.render_support == 2u) {
		return tui_flush_ansi();
	}
	result = tui_flush_render();
	if (result == -KOS_ENOTSUP) {
		/* RENDER is gone: switch for good.  When it answered
		 * before, the ANSI backend has not seen this screen, so
		 * mark every cell dirty before it repaints. */
		if (tui_state.render_support == 1u) {
			tui_dirty_all();
		}
		tui_state.render_support = 2u;
		/* The flush's answer is the ANSI path's answer. */
		return tui_flush_ansi();
	}
	return result;
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
