/* Editable input helper: caller-owned buffer, printable editing, and
 * horizontal scrolling to keep the edit position visible. */

#include "widget_internal.h"

static uint16_t w_input_length(const char *buffer, uint16_t size)
{
	uint16_t length = 0u;

	while ((uint16_t)(length + 1u) < size &&
	       buffer[length] != '\0') {
		++length;
	}
	return length;
}

static void w_input_draw(uint8_t x, uint8_t y, uint8_t width,
			 uint16_t offset, const char *buffer,
			 uint16_t length)
{
	struct tui_rect cell;
	uint8_t at;

	w_blank(x, y, width, 1u, TUI_STYLE_INPUT);
	for (at = 0u; at < width; ++at) {
		uint16_t index = (uint16_t)(offset + at);

		if (index >= length) {
			break;
		}
		cell.x = (uint8_t)(x + at);
		cell.y = y;
		cell.w = 1u;
		cell.h = 1u;
		(void)tui_fill(&cell, buffer[index], TUI_STYLE_INPUT);
	}
}

int tui_input_run(uint8_t x, uint8_t y, uint8_t width, char *buffer,
		  uint16_t buffer_size)
{
	struct tui_rect window;
	uint16_t length;
	uint16_t position;
	uint16_t offset = 0u;
	uint8_t room = 0u;
	uint8_t changed;
	tui_key_t key;
	uint16_t i;

	if (buffer == (char *)0 || buffer_size == 0u || width == 0u) {
		return -KOS_EINVAL;
	}
	if (tui_get_window(&window) < 0) {
		return -KOS_EINVAL;
	}
	length = w_input_length(buffer, buffer_size);
	buffer[length] = '\0';
	position = length;
	if (x < window.w && y < window.h) {
		room = width;
		if ((uint16_t)room > (uint16_t)(window.w - x)) {
			room = (uint8_t)(window.w - x);
		}
	}
	if (room != 0u && position >= room) {
		offset = (uint16_t)(position - room + 1u);
	}
	w_input_draw(x, y, room, offset, buffer, length);
	if (room != 0u) {
		(void)tui_gotoxy((uint8_t)(x + (position - offset)), y);
		(void)tui_cursor_show();
	}
	if (tui_flush() < 0) {
		return -KOS_EINVAL;
	}

	for (;;) {
		key = tui_getkey();
		changed = 0u;
		if (key == TUI_KEY_ESC) {
			return 0;
		}
		if (key == 0x000Du || key == 0x000Au) {
			return 1;
		}
		if (key >= 0x0020u && key <= 0x007Eu) {
			if ((uint16_t)(length + 1u) < buffer_size) {
				for (i = length; i > position; --i) {
					buffer[i] = buffer[i - 1u];
				}
				buffer[position] = (char)key;
				++position;
				++length;
				buffer[length] = '\0';
				changed = 1u;
			}
		} else if (key == 0x0008u || key == 0x007Fu) {
			if (position > 0u) {
				for (i = (uint16_t)(position - 1u);
				     i < length; ++i) {
					buffer[i] = buffer[i + 1u];
				}
				--position;
				--length;
				changed = 1u;
			}
		} else if (key == TUI_KEY_DELETE) {
			if (position < length) {
				for (i = position; i < length; ++i) {
					buffer[i] = buffer[i + 1u];
				}
				--length;
				changed = 1u;
			}
		} else if (key == TUI_KEY_LEFT) {
			if (position > 0u) {
				--position;
				changed = 1u;
			}
		} else if (key == TUI_KEY_RIGHT) {
			if (position < length) {
				++position;
				changed = 1u;
			}
		} else if (key == TUI_KEY_HOME) {
			if (position != 0u) {
				position = 0u;
				changed = 1u;
			}
		} else if (key == TUI_KEY_END) {
			if (position != length) {
				position = length;
				changed = 1u;
			}
		}
		if (changed == 0u) {
			continue;
		}
		if (room == 0u) {
			offset = 0u;
		} else {
			if (position < offset) {
				offset = position;
			}
			if (position >= (uint16_t)(offset + room)) {
				offset = (uint16_t)(position - room + 1u);
			}
			if (offset > length) {
				offset = length;
			}
		}
		w_input_draw(x, y, room, offset, buffer, length);
		if (room != 0u) {
			(void)tui_gotoxy((uint8_t)(x + (position - offset)), y);
			(void)tui_cursor_show();
		}
		if (tui_flush() < 0) {
			return -KOS_EINVAL;
		}
	}
}
