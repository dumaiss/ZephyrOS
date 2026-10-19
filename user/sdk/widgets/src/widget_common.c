/* Shared bounded string, clipping and drawing helpers for widgets. */

#include "widget_internal.h"

uint16_t w_strlen(const char *text)
{
	uint16_t length = 0u;

	while (text[length] != '\0') {
		++length;
	}
	return length;
}

int w_clip(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
	   struct tui_rect *out)
{
	struct tui_rect window;
	uint8_t clipped_w;
	uint8_t clipped_h;

	if (out == (struct tui_rect *)0 || w == 0u || h == 0u) {
		return -KOS_EINVAL;
	}
	if (tui_get_window(&window) < 0) {
		return -KOS_EINVAL;
	}
	out->x = 0u;
	out->y = 0u;
	out->w = 0u;
	out->h = 0u;
	if (x >= window.w || y >= window.h) {
		return 0;
	}
	clipped_w = (uint8_t)(window.w - x);
	if (w < clipped_w) {
		clipped_w = w;
	}
	clipped_h = (uint8_t)(window.h - y);
	if (h < clipped_h) {
		clipped_h = h;
	}
	out->x = x;
	out->y = y;
	out->w = clipped_w;
	out->h = clipped_h;
	return 0;
}

uint8_t w_text(uint8_t x, uint8_t y, const char *text, uint8_t max,
	       tui_style_t style)
{
	struct tui_rect cell;
	uint8_t at = 0u;

	while (at < max && text[at] != '\0') {
		cell.x = (uint8_t)(x + at);
		cell.y = y;
		cell.w = 1u;
		cell.h = 1u;
		if (tui_fill(&cell, text[at], style) < 0) {
			break;
		}
		++at;
	}
	return at;
}

void w_blank(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
	     tui_style_t style)
{
	struct tui_rect rect;

	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;
	(void)tui_fill(&rect, ' ', style);
}

int w_button_mask_valid(uint16_t mask)
{
	uint16_t allowed = (uint16_t)(TUI_BUTTON(TUI_ID_OK) |
				      TUI_BUTTON(TUI_ID_CANCEL) |
				      TUI_BUTTON(TUI_ID_YES) |
				      TUI_BUTTON(TUI_ID_NO));

	return mask != 0u && (uint16_t)(mask & (uint16_t)~allowed) == 0u;
}
