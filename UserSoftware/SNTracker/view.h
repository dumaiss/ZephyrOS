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
	uint8_t highlighted_row[2];
	uint8_t highlighted_order[2];
	uint8_t highlighted_instrument[2];
};

int view_open(struct view *view, const struct ztrdoc *doc,
	      const uint8_t *order_buffer, uint16_t order);
void view_close(void);
int view_draw_page(struct view *view, uint8_t page, uint16_t first_row);
int view_draw_row(uint8_t page, uint16_t row,
		  const struct ztrdoc_event events[ZTRDOC_CHANNELS]);

#endif
