#include "kos.h"
#include "tui.h"

#include "ke_text.h"
#include "ke_view.h"

struct ke_view ke_view;

/* One row's glyphs, and its selected cells [select_from, select_to): a
 * selection is contiguous, so a row shows at most one selected span. */
static uint8_t row_cells[128];
static uint8_t select_from;
static uint8_t select_to;

void ke_view_setup(struct ke_store *store, uint8_t first_row, uint8_t rows,
		   uint8_t cols)
{
	ke_view.store = store;
	ke_view.first_row = first_row;
	ke_view.rows = rows > KE_VIEW_ROWS_MAX ? KE_VIEW_ROWS_MAX : rows;
	ke_view.cols = cols > sizeof(row_cells) ? sizeof(row_cells) : cols;
	ke_view.select_first = 0ul;
	ke_view.select_last = 0ul;
	ke_view.left_column = 0ul;
	(void)tui_set_style(TUI_STYLE_SELECTED);
	ke_view.attr_selected = tui_get_attr();
	(void)tui_set_style(TUI_STYLE_NORMAL);
	ke_view.attr_normal = tui_get_attr();
}

ke_pos_t ke_view_next_start(ke_pos_t start)
{
	ke_pos_t end;

	if (start == KE_POS_NONE) return KE_POS_NONE;
	end = ke_line_end(ke_view.store, start);
	return end < ke_store_length(ke_view.store) ? end + 1ul : KE_POS_NONE;
}

/* Glyphs of the line starting at `start`, from the left column on: TAB
 * expands to the next multiple of eight, a byte shows as itself.  The
 * visible part is walked with byte-sized columns; a horizontal scroll
 * first walks to the left column. */
static uint8_t build_row(ke_pos_t start)
{
	struct ke_store *store = ke_view.store;
	ke_pos_t at = start;
	ke_pos_t length = ke_store_length(store);
	ke_pos_t first = ke_view.select_first;
	ke_pos_t last = ke_view.select_last;
	uint8_t cols = ke_view.cols;
	uint8_t column = 0u;
	uint8_t any_selected = 0u;

	memset(row_cells, ' ', cols);
	if (start == KE_POS_NONE) return 0u;
	if (ke_view.left_column != 0ul) {
		uint32_t visual;

		at = ke_walk_line(store, start, length, ke_view.left_column,
				  &visual);
		if (visual < ke_view.left_column) {
			/* A TAB straddles the left edge: its rest shows. */
			column = (uint8_t)(((visual + 8ul) & ~7ul) -
					   ke_view.left_column);
			if (first != last && at >= first && at < last) {
				select_from = 0u;
				select_to = column;
				any_selected = 1u;
			}
			++at;
		}
	}
	while (at < length && column < cols) {
		const uint8_t *page;
		uint16_t offset;
		uint16_t page_length;

		if (ke_store_span(store, at, &page, &offset, &page_length) !=
		    KE_STORE_OK) break;
		for (; offset < page_length; ++offset, ++at) {
			uint8_t byte = page[offset];
			uint8_t next;

			if (byte == 10u) return any_selected;
			if (byte == 9u) {
				next = (uint8_t)((column + 8u) & ~7u);
				if (next > cols) next = cols;
			} else {
				row_cells[column] = byte;
				next = (uint8_t)(column + 1u);
			}
			if (first != last && at >= first && at < last) {
				if (!any_selected) select_from = column;
				select_to = next;
				any_selected = 1u;
			}
			column = next;
			if (column >= cols) return any_selected;
		}
	}
	return any_selected;
}

static void draw_row(uint8_t row)
{
	uint8_t y = (uint8_t)(ke_view.first_row + row);
	uint8_t cols = ke_view.cols;

	if (!build_row(ke_view.row_start[row])) {
		(void)tui_put_text(0u, y, (const char *)row_cells, cols,
				   ke_view.attr_normal);
		return;
	}
	(void)tui_put_text(0u, y, (const char *)row_cells, select_from,
			   ke_view.attr_normal);
	(void)tui_put_text(select_from, y, (const char *)row_cells + select_from,
			   (uint8_t)(select_to - select_from),
			   ke_view.attr_selected);
	(void)tui_put_text(select_to, y, (const char *)row_cells + select_to,
			   (uint8_t)(cols - select_to), ke_view.attr_normal);
}

void ke_view_draw_rows(uint8_t first, uint8_t last)
{
	if (last >= ke_view.rows) last = (uint8_t)(ke_view.rows - 1u);
	for (; first <= last; ++first) draw_row(first);
}

void ke_view_rebuild(ke_pos_t top, uint32_t top_line)
{
	uint8_t i;

	ke_view.row_start[0] = ke_line_start(ke_view.store, top);
	ke_view.top_line = top_line;
	for (i = 0u; i < ke_view.rows; ++i)
		ke_view.row_start[i + 1u] =
			ke_view_next_start(ke_view.row_start[i]);
	ke_view_draw_rows(0u, (uint8_t)(ke_view.rows - 1u));
}

uint8_t ke_view_row_of(ke_pos_t position)
{
	uint8_t row;

	for (row = 0u; row < ke_view.rows; ++row) {
		ke_pos_t start = ke_view.row_start[row];
		ke_pos_t next = ke_view.row_start[row + 1u];

		if (start == KE_POS_NONE || position < start) break;
		if (next == KE_POS_NONE || position < next) return row;
	}
	return KE_VIEW_NONE;
}

/* The screen region of body rows [first, last] moved one row; 0 when the
 * console scrolled it.  The console restores every screen row outside the
 * region with a VDP copy, so callers pick the side of the cursor that
 * leaves fewer rows outside. */
static int scroll_region(uint8_t first, uint8_t last, uint8_t direction)
{
	return tui_scroll_rows((uint8_t)(ke_view.first_row + first),
			       (uint8_t)(last - first + 1u), direction);
}

/* The rows outside [first, last]: the menu row, those above and below,
 * the status row. */
static uint8_t outside(uint8_t first, uint8_t last)
{
	return (uint8_t)(ke_view.first_row + first + ke_view.rows - last);
}

static void shift_starts(uint8_t first, int8_t delta)
{
	for (; first <= ke_view.rows; ++first)
		if (ke_view.row_start[first] != KE_POS_NONE)
			ke_view.row_start[first] += (ke_pos_t)(int32_t)delta;
}

uint8_t ke_view_scroll(int8_t direction)
{
	ke_pos_t *start = ke_view.row_start;
	uint8_t rows = ke_view.rows;
	uint8_t row = 0u;

	if (direction > 0) {
		if (start[1] == KE_POS_NONE) return 0u;
		memmove(start, start + 1, rows * sizeof(start[0]));
		start[rows] = ke_view_next_start(start[rows - 1u]);
		++ke_view.top_line;
		row = (uint8_t)(rows - 1u);
	} else {
		if (start[0] == 0ul) return 0u;
		memmove(start + 1, start, rows * sizeof(start[0]));
		start[0] = ke_line_start(ke_view.store, start[1] - 1ul);
		--ke_view.top_line;
	}
	if (scroll_region(0u, (uint8_t)(rows - 1u),
			  direction > 0 ? TUI_SCROLL_UP : TUI_SCROLL_DOWN) == 0)
		draw_row(row);
	else
		ke_view_draw_rows(0u, (uint8_t)(rows - 1u));
	return 1u;
}

void ke_view_edit_inline(uint8_t row, int8_t delta)
{
	if (delta != 0) shift_starts((uint8_t)(row + 1u), delta);
	draw_row(row);
}

void ke_view_newline(uint8_t row, ke_pos_t at)
{
	ke_pos_t *start = ke_view.row_start;
	uint8_t rows = ke_view.rows;
	uint8_t last = (uint8_t)(rows - 1u);

	if (row != 0u && outside(0u, row) < outside((uint8_t)(row + 1u), last)) {
		/* The rows above move up: the top advances one line and the
		 * split line's halves end on rows row - 1 and row. */
		memmove(start, start + 1, row * sizeof(start[0]));
		start[row] = at + 1ul;
		shift_starts((uint8_t)(row + 1u), 1);
		++ke_view.top_line;
		if (scroll_region(0u, row, TUI_SCROLL_UP) == 0) {
			draw_row((uint8_t)(row - 1u));
			draw_row(row);
		} else {
			ke_view_draw_rows(0u, last);
		}
		return;
	}
	/* The rows below move down. */
	memmove(start + row + 2, start + row + 1,
		(uint8_t)(rows - row - 1u) * sizeof(start[0]));
	start[row + 1u] = at + 1ul;
	shift_starts((uint8_t)(row + 2u), 1);
	if (row >= last) {
		draw_row(row);
	} else if (scroll_region((uint8_t)(row + 1u), last,
				 TUI_SCROLL_DOWN) == 0) {
		draw_row(row);
		draw_row((uint8_t)(row + 1u));
	} else {
		ke_view_draw_rows(row, last);
	}
}

void ke_view_join(uint8_t row)
{
	ke_pos_t *start = ke_view.row_start;
	uint8_t rows = ke_view.rows;
	uint8_t last = (uint8_t)(rows - 1u);

	if (row < last && start[0] != 0ul &&
	    outside(0u, (uint8_t)(row + 1u)) < outside((uint8_t)(row + 1u), last)) {
		/* The rows above move down: the top steps back one line and
		 * the joined line lands on row + 1. */
		memmove(start + 1, start, (uint8_t)(row + 1u) * sizeof(start[0]));
		start[0] = ke_line_start(ke_view.store, start[1] - 1ul);
		shift_starts((uint8_t)(row + 2u), -1);
		--ke_view.top_line;
		if (scroll_region(0u, (uint8_t)(row + 1u), TUI_SCROLL_DOWN) == 0) {
			draw_row(0u);
			draw_row((uint8_t)(row + 1u));
		} else {
			ke_view_draw_rows(0u, last);
		}
		return;
	}
	/* The rows below move up. */
	memmove(start + row + 1, start + row + 2,
		(uint8_t)(rows - row - 1u) * sizeof(start[0]));
	shift_starts((uint8_t)(row + 1u), -1);
	start[rows] = ke_view_next_start(start[last]);
	if (row >= last) {
		draw_row(row);
	} else if (scroll_region((uint8_t)(row + 1u), last,
				 TUI_SCROLL_UP) == 0) {
		draw_row(row);
		draw_row(last);
	} else {
		ke_view_draw_rows(row, last);
	}
}
