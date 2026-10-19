/* Menu-bar interaction: left/right navigation, ENTER, declared
 * shortcuts, and ESC. */

#include "widget_internal.h"

tui_id_t tui_menu_run(const struct tui_menu_item *items, uint8_t count)
{
	uint8_t selected = 0u;
	tui_id_t id;
	uint8_t i;
	tui_key_t key;

	if (items == (const struct tui_menu_item *)0 || count == 0u) {
		return TUI_ID_NONE;
	}
	for (i = 0u; i < count; ++i) {
		if (items[i].label == (const char *)0) {
			return TUI_ID_NONE;
		}
	}
	id = items[0].id;
	for (;;) {
		if (tui_menubar(items, count, &id) < 0 ||
		    tui_flush() < 0) {
			return TUI_ID_NONE;
		}
		key = tui_getkey();
		if (key == TUI_KEY_ESC) {
			return TUI_ID_NONE;
		}
		if (key == 0x000Du || key == 0x000Au) {
			return items[selected].id;
		}
		if (key == TUI_KEY_LEFT) {
			if (selected == 0u) {
				selected = (uint8_t)(count - 1u);
			} else {
				--selected;
			}
			id = items[selected].id;
			continue;
		}
		if (key == TUI_KEY_RIGHT) {
			++selected;
			if (selected >= count) {
				selected = 0u;
			}
			id = items[selected].id;
			continue;
		}
		for (i = 0u; i < count; ++i) {
			if (items[i].shortcut != TUI_KEY_NONE &&
			    items[i].shortcut == key) {
				return items[i].id;
			}
		}
	}
}
