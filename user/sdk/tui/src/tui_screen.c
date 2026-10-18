/* libtui cursor, text writing and control-byte handling.
 *
 * Drawing is window-relative and clipped.  Text never wraps or scrolls
 * implicitly: a printable byte past the right edge is consumed without
 * touching another row, and LF clamps at the window's bottom row.  The
 * logical map is the only screen state; nothing is written to the console
 * here.
 */

#include "tui_internal.h"

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
	uint16_t index;
	uint8_t next;

	if (ch >= 0x20u) {
		if (*x < window->w && *y < window->h) {
			index = TUI_CELL_INDEX(
				(uint8_t)(window->x + *x),
				(uint8_t)(window->y + *y));
			if (tui_cells[index].ch != ch ||
			    tui_cells[index].attr != tui_state.attr) {
				tui_cells[index].ch = ch;
				tui_cells[index].attr = tui_state.attr;
				TUI_DIRTY_SET(index);
			}
		}
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
