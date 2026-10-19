/* Render-only widgets: status bar, menu bar, label, button, input,
 * checkbox and list box.  Every control clips to the active window and
 * writes only its own cells.  Caller-owned values and list state remain
 * caller-owned.
 */

#include "widget_internal.h"

static uint8_t w_min_u8(uint16_t a, uint16_t b)
{
	if (a < b) {
		return (uint8_t)a;
	}
	return (uint8_t)b;
}

int tui_statusbar(const char *text)
{
	struct tui_rect window;

	if (text == (const char *)0 ||
	    tui_get_window(&window) < 0) {
		return -KOS_EINVAL;
	}
	w_blank(0u, (uint8_t)(window.h - 1u), window.w, 1u,
		TUI_STYLE_STATUS);
	(void)w_text(0u, (uint8_t)(window.h - 1u), text, window.w,
		     TUI_STYLE_STATUS);
	return 0;
}

int tui_menubar(const struct tui_menu_item *items, uint8_t count,
		tui_id_t *selected)
{
	struct tui_rect window;
	uint16_t x = 0u;
	uint8_t i;

	if (selected == (tui_id_t *)0 ||
	    tui_get_window(&window) < 0) {
		return -KOS_EINVAL;
	}
	if (items == (const struct tui_menu_item *)0 && count != 0u) {
		return -KOS_EINVAL;
	}
	for (i = 0u; i < count; ++i) {
		if (items[i].label == (const char *)0) {
			return -KOS_EINVAL;
		}
	}
	w_blank(0u, 0u, window.w, 1u, TUI_STYLE_NORMAL);
	for (i = 0u; i < count && x < (uint16_t)window.w; ++i) {
		tui_style_t style = items[i].id == *selected ?
				    TUI_STYLE_SELECTED : TUI_STYLE_NORMAL;
		uint16_t length = w_strlen(items[i].label);
		uint8_t room = w_min_u8(length,
					(uint16_t)((uint16_t)window.w - x));

		(void)w_text((uint8_t)x, 0u, items[i].label, room, style);
		x = (uint16_t)(x + room);
		if (room < length) {
			break;
		}
		if (x < (uint16_t)window.w) {
			struct tui_rect cell;

			cell.x = (uint8_t)x;
			cell.y = 0u;
			cell.w = 1u;
			cell.h = 1u;
			(void)tui_fill(&cell, ' ', TUI_STYLE_NORMAL);
			++x;
		}
	}
	return 0;
}

int tui_button(uint8_t x, uint8_t y, const char *label, tui_id_t id,
	       uint8_t focused)
{
	struct tui_rect window;
	tui_style_t style;
	uint16_t length;
	uint16_t total;
	uint8_t width;
	uint8_t drawn;

	(void)id;
	if (label == (const char *)0 ||
	    tui_get_window(&window) < 0) {
		return -KOS_EINVAL;
	}
	if (x >= window.w || y >= window.h) {
		return 0;
	}
	length = w_strlen(label);
	total = (uint16_t)(length + 4u);
	width = w_min_u8(total, (uint16_t)(window.w - x));
	style = focused != 0u ? TUI_STYLE_SELECTED : TUI_STYLE_NORMAL;
	w_blank(x, y, width, 1u, style);
	if (width == 0u) {
		return 0;
	}
	{
		struct tui_rect cell;

		cell.x = x;
		cell.y = y;
		cell.w = 1u;
		cell.h = 1u;
		(void)tui_fill(&cell, '[', style);
	}
	drawn = 0u;
	if (width > 1u) {
		struct tui_rect cell;

		cell.x = (uint8_t)(x + 1u);
		cell.y = y;
		cell.w = 1u;
		cell.h = 1u;
		(void)tui_fill(&cell, ' ', style);
	}
	if (width > 2u) {
		drawn = w_text((uint8_t)(x + 2u), y, label,
			       (uint8_t)(width - 3u), style);
	}
	if ((uint8_t)(x + 2u + drawn) < (uint8_t)(x + width)) {
		struct tui_rect cell;

		cell.x = (uint8_t)(x + 2u + drawn);
		cell.y = y;
		cell.w = 1u;
		cell.h = 1u;
		(void)tui_fill(&cell, ' ', style);
	}
	if ((uint8_t)(x + 3u + drawn) < (uint8_t)(x + width)) {
		struct tui_rect cell;

		cell.x = (uint8_t)(x + 3u + drawn);
		cell.y = y;
		cell.w = 1u;
		cell.h = 1u;
		(void)tui_fill(&cell, ']', style);
	}
	return 0;
}

int tui_label(uint8_t x, uint8_t y, const char *text)
{
	struct tui_rect window;
	uint8_t room;

	if (text == (const char *)0 ||
	    tui_get_window(&window) < 0) {
		return -KOS_EINVAL;
	}
	if (x >= window.w || y >= window.h) {
		return 0;
	}
	room = (uint8_t)(window.w - x);
	(void)w_text(x, y, text, room, TUI_STYLE_NORMAL);
	return 0;
}

int tui_input(uint8_t x, uint8_t y, uint8_t width, char *buffer,
	      uint16_t buffer_size)
{
	struct tui_rect window;
	uint16_t length = 0u;
	uint8_t room;

	if (buffer == (char *)0 || buffer_size == 0u || width == 0u ||
	    tui_get_window(&window) < 0) {
		return -KOS_EINVAL;
	}
	while ((uint16_t)(length + 1u) < buffer_size &&
	       buffer[length] != '\0') {
		++length;
	}
	if (x >= window.w || y >= window.h) {
		return 0;
	}
	room = w_min_u8(width, (uint16_t)(window.w - x));
	w_blank(x, y, room, 1u, TUI_STYLE_INPUT);
	(void)w_text(x, y, buffer, w_min_u8(length, room),
		     TUI_STYLE_INPUT);
	return 0;
}

int tui_checkbox(uint8_t x, uint8_t y, const char *label,
		 uint8_t *checked)
{
	struct tui_rect window;
	uint16_t length;
	uint16_t total;
	uint8_t room;
	struct tui_rect cell;

	if (label == (const char *)0 || checked == (uint8_t *)0 ||
	    tui_get_window(&window) < 0) {
		return -KOS_EINVAL;
	}
	if (x >= window.w || y >= window.h) {
		return 0;
	}
	length = w_strlen(label);
	total = (uint16_t)(length + 4u);
	room = w_min_u8(total, (uint16_t)(window.w - x));
	w_blank(x, y, room, 1u, TUI_STYLE_NORMAL);
	if (room > 0u) {
		cell.x = x;
		cell.y = y;
		cell.w = 1u;
		cell.h = 1u;
		(void)tui_fill(&cell, '[', TUI_STYLE_NORMAL);
	}
	if (room > 1u) {
		cell.x = (uint8_t)(x + 1u);
		(void)tui_fill(&cell, *checked != 0u ? 'X' : ' ',
			       TUI_STYLE_NORMAL);
	}
	if (room > 2u) {
		cell.x = (uint8_t)(x + 2u);
		(void)tui_fill(&cell, ']', TUI_STYLE_NORMAL);
	}
	if (room > 3u) {
		cell.x = (uint8_t)(x + 3u);
		(void)tui_fill(&cell, ' ', TUI_STYLE_NORMAL);
	}
	if (room > 4u) {
		(void)w_text((uint8_t)(x + 4u), y, label,
			     (uint8_t)(room - 4u), TUI_STYLE_NORMAL);
	}
	return 0;
}

int tui_listbox(const struct tui_rect *rect, struct tui_list *list)
{
	struct tui_rect clipped;
	uint16_t visible;
	uint16_t row;
	uint16_t index;
	uint16_t selected;

	if (rect == (const struct tui_rect *)0 ||
	    list == (struct tui_list *)0) {
		return -KOS_EINVAL;
	}
	if (list->count != 0u &&
	    list->items == (const char * const *)0) {
		return -KOS_EINVAL;
	}
	for (index = 0u; index < list->count; ++index) {
		if (list->items[index] == (const char *)0) {
			return -KOS_EINVAL;
		}
	}
	if (rect->w == 0u || rect->h == 0u) {
		return 0;
	}
	if (w_clip(rect->x, rect->y, rect->w, rect->h, &clipped) < 0) {
		return -KOS_EINVAL;
	}
	if (clipped.w == 0u || clipped.h == 0u) {
		return 0;
	}
	w_blank(clipped.x, clipped.y, clipped.w, clipped.h,
		TUI_STYLE_NORMAL);
	if (clipped.w < 3u || clipped.h < 3u) {
		return 0;
	}
	if (tui_frame(&clipped, TUI_FRAME_SINGLE, TUI_STYLE_NORMAL) < 0) {
		return -KOS_EINVAL;
	}
	if (list->count == 0u) {
		list->selected = 0u;
		list->top = 0u;
		return 0;
	}
	selected = list->selected;
	if (selected >= list->count) {
		selected = (uint16_t)(list->count - 1u);
	}
	visible = (uint16_t)(clipped.h - 2u);
	if (list->top > selected) {
		list->top = selected;
	}
	if ((uint16_t)(list->top + visible) <= selected) {
		list->top = (uint16_t)(selected - visible + 1u);
	}
	if ((uint16_t)(list->top + visible) > list->count) {
		list->top = list->count > visible ?
			    (uint16_t)(list->count - visible) : 0u;
	}
	list->selected = selected;
	for (row = 0u; row < visible; ++row) {
		index = (uint16_t)(list->top + row);
		tui_style_t style = index == selected ?
				    TUI_STYLE_SELECTED : TUI_STYLE_NORMAL;

		if (index >= list->count) {
			break;
		}
		if (style == TUI_STYLE_SELECTED) {
			w_blank((uint8_t)(clipped.x + 1u),
				(uint8_t)(clipped.y + 1u + row),
				(uint8_t)(clipped.w - 2u), 1u, style);
		}
		(void)w_text((uint8_t)(clipped.x + 1u),
			     (uint8_t)(clipped.y + 1u + row),
			     list->items[index],
			     (uint8_t)(clipped.w - 2u), style);
	}
	return 0;
}
