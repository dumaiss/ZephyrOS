#ifndef KONAEDIT_WIDGETS_H
#define KONAEDIT_WIDGETS_H

#include "tui.h"

typedef uint16_t tui_id_t;

#define TUI_ID_NONE    0u
#define TUI_ID_OK      1u
#define TUI_ID_CANCEL  2u
#define TUI_ID_YES     3u
#define TUI_ID_NO      4u
#define TUI_BUTTON(id) ((uint16_t)(1u << (id)))

struct tui_menu_item {
	const char *label;
	tui_key_t shortcut;
	tui_id_t id;
};

int ke_statusbar(const char *text);
int ke_menubar(const struct tui_menu_item *items, uint8_t count,
	       tui_id_t selected);
int ke_button(uint8_t x, uint8_t y, const char *label, uint8_t focused);
int ke_label(uint8_t x, uint8_t y, const char *text);
int ke_input(uint8_t x, uint8_t y, uint8_t width, const char *text,
	     uint16_t capacity);

#endif
