#ifndef KONAEDIT_VIEW_H
#define KONAEDIT_VIEW_H

#include "ke_store.h"

/* The view: the document lines on the screen's body rows.  It keeps each
 * visible row's line start and the top row's line number, so the cursor's
 * line is top_line + its row and nothing counts lines except a rebuild.
 * Rows are drawn from page spans with one tui_put_text per attribute run;
 * the store is read only for the rows that change or scroll in. */

#define KE_VIEW_ROWS_MAX 32u
#define KE_VIEW_NONE 0xFFu

struct ke_view {
	struct ke_store *store;
	/* Row i's line start, or KE_POS_NONE past the document's end;
	 * [rows] is the line after the bottom row. */
	ke_pos_t row_start[KE_VIEW_ROWS_MAX + 1u];
	uint32_t top_line;
	uint32_t left_column;
	ke_pos_t select_first;  /* the selection [first, last) */
	ke_pos_t select_last;
	uint8_t rows;
	uint8_t cols;
	uint8_t first_row;      /* the screen row of body row 0 */
	uint8_t attr_normal;
	uint8_t attr_selected;
};

extern struct ke_view ke_view;
/* A row's worth of scratch, filled and written out at once. */
extern uint8_t ke_view_cells[128];

void ke_view_setup(struct ke_store *store, uint8_t first_row, uint8_t rows,
		   uint8_t cols);
/* The view from `top` (snapped to its line start), whose line number is
 * `top_line`; every row is drawn. */
void ke_view_rebuild(ke_pos_t top, uint32_t top_line);
void ke_view_draw_rows(uint8_t first, uint8_t last);
/* The row holding `position`, or KE_VIEW_NONE. */
uint8_t ke_view_row_of(ke_pos_t position);
/* One line down (+1: the rows move up) or up (-1); 0 at a document end.
 * The new row is drawn. */
uint8_t ke_view_scroll(int8_t direction);
/* After an edit inside row `row`'s line that added (delta 1) or removed
 * (delta -1) one non-newline byte, or replaced one (delta 0): the row
 * starts below shift and the row is drawn. */
void ke_view_edit_inline(uint8_t row, int8_t delta);
/* After a newline was inserted at `at` inside row `row`. */
void ke_view_newline(uint8_t row, ke_pos_t at);
/* After the newline ending row `row`'s line was deleted (row + 1 joined
 * it). */
void ke_view_join(uint8_t row);
/* The start of the line after the one starting at `start`, or
 * KE_POS_NONE. */
ke_pos_t ke_view_next_start(ke_pos_t start);

#endif
