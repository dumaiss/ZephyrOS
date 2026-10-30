#ifndef SNTRACK_VIEW_H
#define SNTRACK_VIEW_H

#include "ztrdoc.h"

struct view {
	const struct ztrdoc *doc;
	const uint8_t *order_buffer;
	uint16_t order;
	uint16_t first_row;
	uint16_t order_top;
	uint8_t instrument_top;
	uint8_t selected_instrument;
	/* What each page shows, so a page redraws only what changed: the
	 * static parts (`drawn`), its order and orders window; `pending`
	 * while a page is half drawn (a full redraw follows). */
	uint8_t drawn[2];
	uint8_t pending[2];
	/* The title bar's transport: PLAY (1) or STOP and the elapsed
	 * seconds, as set and as each page shows them. */
	uint8_t playing;
	uint16_t seconds;
	uint8_t shown_playing[2];
	uint16_t shown_seconds[2];
	uint16_t drawn_order[2];
	uint16_t drawn_top[2];
};

int view_open(struct view *view, const struct ztrdoc *doc,
	      const uint8_t *order_buffer, uint16_t order);
void view_close(void);
/* The whole page, or the same in steps for drawing between cursor moves:
 * view_begin_page decodes the viewport (the order buffer is free after
 * it), then view_draw_step 0 to VIEW_PANEL_STEPS - 1 draws the title, the
 * orders and the instruments (two halves each), the song panel and the
 * headers, then one row each, and last the rules, status
 * and cursor. */
#define VIEW_PANEL_STEPS 7u
#define VIEW_STEPS (VIEW_PANEL_STEPS + ZTRDOC_VIEW_ROWS + 1u)
/* The orders and instruments panels' visible lines. */
#define VIEW_PANEL_LINES 13u
int view_draw_page(struct view *view, uint8_t page, uint16_t first_row);
int view_begin_page(struct view *view, uint8_t page, uint16_t first_row);
int view_draw_step(struct view *view, uint8_t page, uint8_t step);
/* Redraw the page's transport when it differs from the view's. */
int view_update_transport(struct view *view, uint8_t page);
/* The cursor from viewport row `from` to `to`: two XOR fills. */
int view_move_cursor(uint8_t page, uint8_t from, uint8_t to);
int view_draw_row(uint8_t page, uint16_t row,
		  const struct ztrdoc_event events[ZTRDOC_CHANNELS]);

#endif
