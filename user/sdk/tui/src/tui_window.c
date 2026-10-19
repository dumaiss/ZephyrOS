/* libtui logical windows and whole-region clear/line operations.
 *
 * Windows are absolute rectangles on the logical surface.  Cursor and
 * drawing coordinates are relative to the active window, and every path
 * clips through the screen member's common clipping layer.  Line
 * insertion and deletion shift only the active window; nothing is sent
 * to the console here.
 */

#include "tui_internal.h"

static int tui_rect_fits(const struct tui_rect *rect)
{
	uint16_t right;
	uint16_t bottom;

	if (rect->w == 0u || rect->h == 0u) {
		return 0;
	}
	right = (uint16_t)rect->x + (uint16_t)rect->w;
	bottom = (uint16_t)rect->y + (uint16_t)rect->h;
	return right <= (uint16_t)tui_state.screen.cols &&
	       bottom <= (uint16_t)tui_state.screen.rows;
}

static void tui_install_window(const struct tui_rect *rect)
{
	tui_state.window = *rect;
	tui_state.cursor_x = 0u;
	tui_state.cursor_y = 0u;
}

static void tui_blank_row(uint8_t y)
{
	struct tui_rect row;

	row.x = 0u;
	row.y = y;
	row.w = tui_state.window.w;
	row.h = 1u;
	tui_priv_fill_clipped(&row, 0x20u, tui_state.attr);
}

int tui_window(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
	struct tui_rect rect;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;
	if (!tui_rect_fits(&rect)) {
		return -KOS_EINVAL;
	}
	tui_install_window(&rect);
	return 0;
}

int tui_reset_window(void)
{
	struct tui_rect rect;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	rect.x = 0u;
	rect.y = 0u;
	rect.w = tui_state.screen.cols;
	rect.h = tui_state.screen.rows;
	tui_install_window(&rect);
	return 0;
}

int tui_get_window(struct tui_rect *out)
{
	if (tui_state.initialized == 0u ||
	    out == (struct tui_rect *)0) {
		return -KOS_EINVAL;
	}
	*out = tui_state.window;
	return 0;
}

int tui_push_window(const struct tui_rect *rect)
{
	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	if (rect == (const struct tui_rect *)0 ||
	    !tui_rect_fits(rect)) {
		return -KOS_EINVAL;
	}
	if (tui_state.window_depth >= TUI_WINDOW_STACK_MAX) {
		return -KOS_EINVAL;
	}
	tui_state.window_stack[tui_state.window_depth] = tui_state.window;
	++tui_state.window_depth;
	tui_install_window(rect);
	return 0;
}

int tui_pop_window(void)
{
	if (tui_state.initialized == 0u ||
	    tui_state.window_depth == 0u) {
		return -KOS_EINVAL;
	}
	--tui_state.window_depth;
	tui_install_window(&tui_state.window_stack[tui_state.window_depth]);
	return 0;
}

int tui_clear(void)
{
	struct tui_rect rect;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	rect.x = 0u;
	rect.y = 0u;
	rect.w = tui_state.window.w;
	rect.h = tui_state.window.h;
	tui_priv_fill_clipped(&rect, 0x20u, tui_state.attr);
	tui_state.cursor_x = 0u;
	tui_state.cursor_y = 0u;
	return 0;
}

int tui_clreol(void)
{
	struct tui_rect rect;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	if (tui_state.cursor_x >= tui_state.window.w) {
		return 0;
	}
	rect.x = tui_state.cursor_x;
	rect.y = tui_state.cursor_y;
	rect.w = (uint8_t)(tui_state.window.w - tui_state.cursor_x);
	rect.h = 1u;
	tui_priv_fill_clipped(&rect, 0x20u, tui_state.attr);
	return 0;
}

int tui_clrline(void)
{
	struct tui_rect rect;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	rect.x = 0u;
	rect.y = tui_state.cursor_y;
	rect.w = tui_state.window.w;
	rect.h = 1u;
	tui_priv_fill_clipped(&rect, 0x20u, tui_state.attr);
	return 0;
}

int tui_insert_line(void)
{
	uint8_t x;
	uint8_t y;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	if (tui_state.cursor_y >= tui_state.window.h) {
		return 0;
	}
	y = (uint8_t)(tui_state.window.h - 1u);
	while (y > tui_state.cursor_y) {
		for (x = 0u; x < tui_state.window.w; ++x) {
			uint16_t source = TUI_CELL_INDEX(
				(uint8_t)(tui_state.window.x + x),
				(uint8_t)(tui_state.window.y + y - 1u));
			tui_priv_set_cell(x, y, tui_cells[source].ch,
					  tui_cells[source].attr);
		}
		--y;
	}
	tui_blank_row(tui_state.cursor_y);
	return 0;
}

int tui_delete_line(void)
{
	uint8_t x;
	uint8_t y;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	if (tui_state.cursor_y >= tui_state.window.h) {
		return 0;
	}
	y = tui_state.cursor_y;
	while ((uint8_t)(y + 1u) < tui_state.window.h) {
		for (x = 0u; x < tui_state.window.w; ++x) {
			uint16_t source = TUI_CELL_INDEX(
				(uint8_t)(tui_state.window.x + x),
				(uint8_t)(tui_state.window.y + y + 1u));
			tui_priv_set_cell(x, y, tui_cells[source].ch,
					  tui_cells[source].attr);
		}
		++y;
	}
	tui_blank_row((uint8_t)(tui_state.window.h - 1u));
	return 0;
}
