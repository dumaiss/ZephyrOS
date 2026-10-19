/* libtui semantic styles and clipped drawing primitives.
 *
 * One resolved cell attribute (normal or reverse) is carried by the
 * state.  Semantic styles select it; explicit tui_set_attr is the
 * low-level escape hatch.  The CP850 box-drawing bytes are private to
 * this member.  Every primitive clips through the screen member's common
 * rectangle layer and only dirties cells whose value changes.
 */

#include "tui_internal.h"

#define TUI_BOX_SINGLE_H 0xC4u
#define TUI_BOX_SINGLE_V 0xB3u
#define TUI_BOX_SINGLE_TL 0xDAu
#define TUI_BOX_SINGLE_TR 0xBFu
#define TUI_BOX_SINGLE_BL 0xC0u
#define TUI_BOX_SINGLE_BR 0xD9u

#define TUI_BOX_DOUBLE_H 0xCDu
#define TUI_BOX_DOUBLE_V 0xBAu
#define TUI_BOX_DOUBLE_TL 0xC9u
#define TUI_BOX_DOUBLE_TR 0xBBu
#define TUI_BOX_DOUBLE_BL 0xC8u
#define TUI_BOX_DOUBLE_BR 0xBCu

static uint8_t tui_style_attr(tui_style_t style)
{
	switch (style) {
	case TUI_STYLE_TITLE:
	case TUI_STYLE_SELECTED:
	case TUI_STYLE_STATUS:
		return 1u;
	default:
		return 0u;
	}
}

int tui_set_style(tui_style_t style)
{
	if (tui_state.initialized == 0u ||
	    style >= (tui_style_t)TUI_STYLE_COUNT) {
		return -KOS_EINVAL;
	}
	tui_state.style = style;
	tui_state.attr = tui_style_attr(style);
	return 0;
}

tui_style_t tui_get_style(void)
{
	return tui_state.style;
}

int tui_set_attr(uint8_t attr)
{
	if (tui_state.initialized == 0u || attr > 1u) {
		return -KOS_EINVAL;
	}
	tui_state.attr = attr;
	return 0;
}

uint8_t tui_get_attr(void)
{
	return tui_state.attr;
}

int tui_fill(const struct tui_rect *rect, char ch, tui_style_t style)
{
	struct tui_rect clipped;
	int result;

	if (tui_state.initialized == 0u ||
	    style >= (tui_style_t)TUI_STYLE_COUNT) {
		return -KOS_EINVAL;
	}
	result = tui_priv_clip_rect(rect, &clipped);
	if (result < 0) {
		return result;
	}
	tui_priv_fill_clipped(&clipped, (uint8_t)ch, tui_style_attr(style));
	return 0;
}

static uint8_t tui_frame_h(tui_frame_style_t style)
{
	if (style == TUI_FRAME_DOUBLE) {
		return TUI_BOX_DOUBLE_H;
	}
	return TUI_BOX_SINGLE_H;
}

static uint8_t tui_frame_v(tui_frame_style_t style)
{
	if (style == TUI_FRAME_DOUBLE) {
		return TUI_BOX_DOUBLE_V;
	}
	return TUI_BOX_SINGLE_V;
}

int tui_hline(uint8_t x, uint8_t y, uint8_t width,
	      tui_frame_style_t style)
{
	uint16_t i;

	if (tui_state.initialized == 0u ||
	    style > (tui_frame_style_t)TUI_FRAME_DOUBLE) {
		return -KOS_EINVAL;
	}
	if (style == TUI_FRAME_NONE || width == 0u ||
	    y >= tui_state.window.h) {
		return 0;
	}
	for (i = 0u; i < width; ++i) {
		uint16_t at = (uint16_t)x + i;

		if (at >= tui_state.window.w) {
			break;
		}
		tui_priv_set_cell((uint8_t)at, y, tui_frame_h(style),
				  tui_state.attr);
	}
	return 0;
}

int tui_vline(uint8_t x, uint8_t y, uint8_t height,
	      tui_frame_style_t style)
{
	uint16_t i;

	if (tui_state.initialized == 0u ||
	    style > (tui_frame_style_t)TUI_FRAME_DOUBLE) {
		return -KOS_EINVAL;
	}
	if (style == TUI_FRAME_NONE || height == 0u ||
	    x >= tui_state.window.w) {
		return 0;
	}
	for (i = 0u; i < height; ++i) {
		uint16_t at = (uint16_t)y + i;

		if (at >= tui_state.window.h) {
			break;
		}
		tui_priv_set_cell(x, (uint8_t)at, tui_frame_v(style),
				  tui_state.attr);
	}
	return 0;
}

int tui_frame(const struct tui_rect *rect, tui_frame_style_t style,
	      tui_style_t text_style)
{
	struct tui_rect clipped;
	uint16_t right;
	uint16_t bottom;
	uint8_t x;
	uint8_t y;
	uint8_t attr;
	int result;

	if (tui_state.initialized == 0u ||
	    style > (tui_frame_style_t)TUI_FRAME_DOUBLE ||
	    text_style >= (tui_style_t)TUI_STYLE_COUNT) {
		return -KOS_EINVAL;
	}
	result = tui_priv_clip_rect(rect, &clipped);
	if (result < 0) {
		return result;
	}
	if (style == TUI_FRAME_NONE || clipped.w == 0u || clipped.h == 0u) {
		return 0;
	}
	attr = tui_style_attr(text_style);
	right = (uint16_t)rect->x + (uint16_t)rect->w - 1u;
	bottom = (uint16_t)rect->y + (uint16_t)rect->h - 1u;
	for (y = 0u; y < clipped.h; ++y) {
		uint16_t at_y = (uint16_t)clipped.y + y;
		int top = at_y == (uint16_t)rect->y;
		int bottom_edge = at_y == bottom;

		for (x = 0u; x < clipped.w; ++x) {
			uint16_t at_x = (uint16_t)clipped.x + x;
			int left = at_x == (uint16_t)rect->x;
			int right_edge = at_x == right;
			uint8_t ch;

			if (top && left) {
				ch = style == TUI_FRAME_DOUBLE ?
				     TUI_BOX_DOUBLE_TL :
				     TUI_BOX_SINGLE_TL;
			} else if (top && right_edge) {
				ch = style == TUI_FRAME_DOUBLE ?
				     TUI_BOX_DOUBLE_TR :
				     TUI_BOX_SINGLE_TR;
			} else if (bottom_edge && left) {
				ch = style == TUI_FRAME_DOUBLE ?
				     TUI_BOX_DOUBLE_BL :
				     TUI_BOX_SINGLE_BL;
			} else if (bottom_edge && right_edge) {
				ch = style == TUI_FRAME_DOUBLE ?
				     TUI_BOX_DOUBLE_BR :
				     TUI_BOX_SINGLE_BR;
			} else if (top || bottom_edge) {
				ch = tui_frame_h(style);
			} else if (left || right_edge) {
				ch = tui_frame_v(style);
			} else {
				continue;
			}
			tui_priv_set_cell((uint8_t)at_x, (uint8_t)at_y, ch,
					  attr);
		}
	}
	return 0;
}

int tui_frame_title(const struct tui_rect *rect,
		    tui_frame_style_t style, tui_style_t text_style,
		    const char *title)
{
	uint16_t at;
	uint8_t limit;
	uint8_t i;
	int result;

	if (tui_state.initialized == 0u ||
	    style > (tui_frame_style_t)TUI_FRAME_DOUBLE ||
	    text_style >= (tui_style_t)TUI_STYLE_COUNT ||
	    title == (const char *)0 ||
	    rect == (const struct tui_rect *)0) {
		return -KOS_EINVAL;
	}
	/* A zero-area titled frame is a primitive no-op even though its
	 * visible y coordinate and width could otherwise reach the title
	 * path. */
	if (rect->w == 0u || rect->h == 0u) {
		return 0;
	}
	result = tui_frame(rect, style, text_style);
	if (result < 0) {
		return result;
	}
	if (rect->w < 5u || rect->y >= tui_state.window.h) {
		return 0;
	}
	/* One blank separates the title from each corner; the text never
	 * reaches a corner. */
	if (title[0] != '\0') {
		at = (uint16_t)rect->x + 1u;
		if (at < tui_state.window.w) {
			tui_priv_set_cell((uint8_t)at, rect->y, 0x20u,
					  tui_style_attr(text_style));
		}
		at = (uint16_t)rect->x + (uint16_t)rect->w - 2u;
		if (at < tui_state.window.w) {
			tui_priv_set_cell((uint8_t)at, rect->y, 0x20u,
					  tui_style_attr(text_style));
		}
	}
	at = (uint16_t)rect->x + 2u;
	limit = (uint8_t)(rect->w - 4u);
	for (i = 0u; i < limit && title[i] != '\0'; ++i) {
		uint16_t column = (uint16_t)(at + i);

		if (column >= tui_state.window.w) {
			break;
		}
		tui_priv_set_cell((uint8_t)column, rect->y,
				  (uint8_t)title[i],
				  tui_style_attr(text_style));
	}
	return 0;
}
