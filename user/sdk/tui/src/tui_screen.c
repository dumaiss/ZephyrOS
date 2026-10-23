/* libtui cursor, text writing and control-byte handling.
 *
 * Drawing is window-relative and clipped.  Text never wraps or scrolls
 * implicitly: a printable byte past the right edge is consumed without
 * touching another row, and LF clamps at the window's bottom row.  The
 * logical map is the only screen state; nothing is written to the console
 * here.
 */

#include "tui_internal.h"

int tui_priv_clip_rect(const struct tui_rect *rect,
		       struct tui_rect *clipped)
{
	uint8_t w;
	uint8_t h;

	if (rect == (const struct tui_rect *)0 ||
	    clipped == (struct tui_rect *)0) {
		return -KOS_EINVAL;
	}
	clipped->x = 0u;
	clipped->y = 0u;
	clipped->w = 0u;
	clipped->h = 0u;
	if (rect->w == 0u || rect->h == 0u) {
		return 0;
	}
	if (rect->x >= tui_state.window.w || rect->y >= tui_state.window.h) {
		return 0;
	}
	w = (uint8_t)(tui_state.window.w - rect->x);
	if (rect->w < w) {
		w = rect->w;
	}
	h = (uint8_t)(tui_state.window.h - rect->y);
	if (rect->h < h) {
		h = rect->h;
	}
	clipped->x = rect->x;
	clipped->y = rect->y;
	clipped->w = w;
	clipped->h = h;
	return 0;
}

void tui_priv_set_cell(uint8_t x, uint8_t y, uint8_t ch, uint8_t attr)
{
	uint16_t index;

	if (x >= tui_state.window.w || y >= tui_state.window.h) {
		return;
	}
	index = TUI_CELL_INDEX((uint8_t)(tui_state.window.x + x),
			       (uint8_t)(tui_state.window.y + y));
	if (tui_chars[index] == ch && tui_priv_attr(index) == attr) {
		return;
	}
	if (TUI_DIRTY_TEST(index)) {
		tui_priv_store(index, ch, attr);
		return;
	}
	if (tui_chars[index] != ch) {
		tui_priv_store(index, ch, attr);
		TUI_INVERT_CLEAR(index);
		TUI_DIRTY_SET(index);
		return;
	}
	/* With the two-valued attribute model, an attr-only change is its own
	 * inverse.  Two changes before a flush cancel without touching VRAM. */
	tui_priv_store(index, ch, attr);
	if (TUI_INVERT_TEST(index)) {
		TUI_INVERT_CLEAR(index);
	} else {
		TUI_INVERT_SET(index);
	}
}

void tui_priv_fill_clipped(const struct tui_rect *clipped, uint8_t ch,
			   uint8_t attr)
{
	uint8_t x;
	uint8_t y;

	if (clipped == (const struct tui_rect *)0 || clipped->w == 0u ||
	    clipped->h == 0u) {
		return;
	}
	for (y = 0u; y < clipped->h; ++y) {
		for (x = 0u; x < clipped->w; ++x) {
			tui_priv_set_cell(
				(uint8_t)(clipped->x + x),
				(uint8_t)(clipped->y + y), ch, attr);
		}
	}
}

/* Move the local cursor one cell right, clamping at the window edge. */
static void tui_advance(uint8_t *x)
{
	if (*x < tui_state.window.w) {
		++*x;
	}
}

void tui_priv_put_byte(uint8_t *x, uint8_t *y, uint8_t ch)
{
	struct tui_rect *window = &tui_state.window;
	uint8_t next;

	if (ch >= 0x20u) {
		tui_priv_set_cell(*x, *y, ch, tui_state.attr);
		tui_advance(x);
		return;
	}

	switch (ch) {
	case 0x08u: /* BS */
		if (*x > 0u) {
			--*x;
		}
		break;
	case 0x09u: /* TAB */
		if (*x < window->w) {
			next = (uint8_t)(((*x >> 3) + 1u) << 3);
			if (next >= window->w) {
				next = (uint8_t)(window->w - 1u);
			}
			*x = next;
		}
		break;
	case 0x0Au: /* LF */
		if ((uint8_t)(*y + 1u) < window->h) {
			++*y;
		} else {
			*y = (uint8_t)(window->h - 1u);
		}
		break;
	case 0x0Du: /* CR */
		*x = 0u;
		break;
	default:
		break;
	}
}

int tui_gotoxy(uint8_t x, uint8_t y)
{
	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	if (x >= tui_state.window.w || y >= tui_state.window.h) {
		return -KOS_EINVAL;
	}
	tui_state.cursor_x = x;
	tui_state.cursor_y = y;
	return 0;
}

uint8_t tui_wherex(void)
{
	return tui_state.cursor_x;
}

uint8_t tui_wherey(void)
{
	return tui_state.cursor_y;
}

int tui_cursor_show(void)
{
	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	tui_state.cursor_visible = 1u;
	return 0;
}

int tui_cursor_hide(void)
{
	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	tui_state.cursor_visible = 0u;
	return 0;
}

int tui_putc(char ch)
{
	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	tui_priv_put_byte(&tui_state.cursor_x, &tui_state.cursor_y,
			  (uint8_t)ch);
	return 1;
}

int tui_puts(const char *text)
{
	int count = 0;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	if (text == (const char *)0) {
		return -KOS_EINVAL;
	}
	while (*text != '\0') {
		tui_priv_put_byte(&tui_state.cursor_x, &tui_state.cursor_y,
				  (uint8_t)*text);
		++text;
		++count;
	}
	return count;
}

int tui_write(const char *text, uint16_t length)
{
	uint16_t i;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	if (length == 0u) {
		return 0;
	}
	if (text == (const char *)0) {
		return -KOS_EINVAL;
	}
	for (i = 0u; i < length; ++i) {
		tui_priv_put_byte(&tui_state.cursor_x, &tui_state.cursor_y,
				  (uint8_t)text[i]);
	}
	return (int)length;
}

int tui_putc_xy(uint8_t x, uint8_t y, char ch)
{
	uint8_t draw_x = x;
	uint8_t draw_y = y;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	tui_priv_put_byte(&draw_x, &draw_y, (uint8_t)ch);
	return 1;
}

int tui_puts_xy(uint8_t x, uint8_t y, const char *text)
{
	uint8_t draw_x = x;
	uint8_t draw_y = y;
	int count = 0;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	if (text == (const char *)0) {
		return -KOS_EINVAL;
	}
	while (*text != '\0') {
		tui_priv_put_byte(&draw_x, &draw_y, (uint8_t)*text);
		++text;
		++count;
	}
	return count;
}
