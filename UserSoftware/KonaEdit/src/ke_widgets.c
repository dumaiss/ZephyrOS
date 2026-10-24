#include "kos.h"
#include "tui.h"

#include "ke_widgets.h"

static uint8_t draw_text(uint8_t x, uint8_t y, const char *text,
			 uint8_t limit, tui_style_t style)
{
	uint8_t drawn = 0u;

	(void)tui_set_style(style);
	while (drawn < limit && text[drawn] != '\0') {
		(void)tui_putc_xy((uint8_t)(x + drawn), y, text[drawn]);
		++drawn;
	}
	return drawn;
}

int ke_menubar(const struct tui_menu_item *items, uint8_t count,
	       tui_id_t selected)
{
	struct tui_size size;
	struct tui_rect row;
	uint8_t x = 0u;
	uint8_t i;

	if (tui_get_size(&size) < 0) return -KOS_EINVAL;
	row.x = 0u; row.y = 0u; row.w = size.cols; row.h = 1u;
	(void)tui_fill(&row, ' ', TUI_STYLE_NORMAL);
	for (i = 0u; i < count && x < size.cols; ++i) {
		uint8_t room = (uint8_t)(size.cols - x);
		uint8_t used = draw_text(x, 0u, items[i].label, room,
			items[i].id == selected ? TUI_STYLE_SELECTED :
			TUI_STYLE_NORMAL);
		x = (uint8_t)(x + used);
		if (x < size.cols) ++x;
	}
	return 0;
}

int ke_button(uint8_t x, uint8_t y, const char *label, uint8_t focused)
{
	struct tui_rect area;
	tui_style_t style = focused != 0u ? TUI_STYLE_SELECTED : TUI_STYLE_NORMAL;
	uint8_t length = 0u;

	while (label[length] != '\0' && length < 250u) ++length;
	area.x = x; area.y = y; area.w = (uint8_t)(length + 4u); area.h = 1u;
	(void)tui_fill(&area, ' ', style);
	(void)tui_set_style(style);
	(void)tui_putc_xy(x, y, '[');
	(void)draw_text((uint8_t)(x + 2u), y, label, length, style);
	(void)tui_putc_xy((uint8_t)(x + length + 3u), y, ']');
	return 0;
}

int ke_label(uint8_t x, uint8_t y, const char *text)
{
	struct tui_size size;

	if (tui_get_size(&size) < 0 || x >= size.cols) return -KOS_EINVAL;
	(void)draw_text(x, y, text, (uint8_t)(size.cols - x), TUI_STYLE_NORMAL);
	return 0;
}

int ke_input(uint8_t x, uint8_t y, uint8_t width, const char *text,
	     uint16_t capacity)
{
	struct tui_rect area;
	uint8_t limit = width;

	if (capacity < (uint16_t)limit) limit = (uint8_t)capacity;
	area.x = x; area.y = y; area.w = width; area.h = 1u;
	(void)tui_fill(&area, ' ', TUI_STYLE_INPUT);
	(void)draw_text(x, y, text, limit, TUI_STYLE_INPUT);
	return 0;
}
