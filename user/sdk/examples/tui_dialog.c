/* KonaOS SDK example: menu, list, input, checkbox and a modal confirmation.
 *
 * Built by the examples Makefile with `LIBS=tui_widgets:tui`.  The example
 * drives its own controls through the public widget/core APIs and reports
 * the caller-owned state after shutdown.
 */

#include "kos.h"
#include "tui.h"
#include "tui_widgets.h"

static const struct tui_menu_item menu_items[] = {
	{"File", 'f', 10u},
	{"Edit", 'e', 11u},
	{"Help", 'h', 12u}
};

static const char * const list_items[] = {"Red", "Green", "Blue"};

static int dialog_fail(const char *where, int result)
{
	printf("TUI DIALOG FAIL %s %d\r\n", where, result);
	tui_shutdown();
	return 1;
}

static int draw_form(const struct tui_size *size, char *name,
		     uint8_t checked, struct tui_list *list,
		     tui_id_t menu_id)
{
	struct tui_rect list_rect;
	uint8_t button_y = (uint8_t)(size->rows - 6u);

	list_rect.x = 2u;
	list_rect.y = 6u;
	list_rect.w = 20u;
	list_rect.h = (uint8_t)(size->rows - 16u);
	if (tui_statusbar("TUI dialog") < 0 ||
	    tui_menubar(menu_items, 3u, &menu_id) < 0 ||
	    tui_label(2u, 2u, "Name:") < 0 ||
	    tui_input(10u, 2u, 20u, name, 16u) < 0 ||
	    tui_checkbox(2u, 4u, "Enabled", &checked) < 0 ||
	    tui_listbox(&list_rect, list) < 0 ||
	    tui_button(2u, button_y, "OK", TUI_ID_OK, 1u) < 0 ||
	    tui_button(12u, button_y, "Cancel", TUI_ID_CANCEL, 0u) < 0) {
		return -1;
	}
	return 0;
}

int main(void)
{
	struct tui_size size;
	struct tui_list list;
	struct tui_rect list_rect;
	char name[16];
	tui_id_t menu_id = 10u;
	tui_id_t menu_result;
	tui_id_t confirm_result;
	tui_key_t key;
	volatile uint8_t checked = 1u;
	int input_result;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("TUI DIALOG FAIL init %d\r\n", result);
		return 1;
	}
	result = tui_get_size(&size);
	if (result < 0) {
		return dialog_fail("size", result);
	}
	if (size.cols < 40u || size.rows < 20u) {
		return dialog_fail("geometry", 0);
	}
	name[0] = 'A';
	name[1] = 'd';
	name[2] = 'a';
	name[3] = '\0';
	list.items = list_items;
	list.count = 3u;
	list.selected = 1u;
	list.top = 0u;
	list_rect.x = 2u;
	list_rect.y = 6u;
	list_rect.w = 20u;
	list_rect.h = (uint8_t)(size.rows - 16u);

	if (draw_form(&size, name, checked, &list, menu_id) < 0 ||
	    tui_flush() < 0) {
		return dialog_fail("form", 0);
	}

	menu_result = tui_menu_run(menu_items, 3u);
	input_result = tui_input_run(10u, 2u, 20u, name, 16u);

	for (;;) {
		if (tui_listbox(&list_rect, &list) < 0 ||
		    tui_flush() < 0) {
			return dialog_fail("list", 0);
		}
		key = tui_getkey();
		if (key == 0x000Du || key == 0x000Au || key == TUI_KEY_ESC) {
			break;
		}
		if (key == TUI_KEY_DOWN && list.selected + 1u < list.count) {
			++list.selected;
		} else if (key == TUI_KEY_UP && list.selected > 0u) {
			--list.selected;
		}
	}

	confirm_result = tui_confirm("Confirm", "Save the dialog settings?");
	if (draw_form(&size, name, checked, &list, menu_id) < 0 ||
	    tui_flush() < 0) {
		return dialog_fail("final form", 0);
	}
	if (menu_result != 10u) {
		return dialog_fail("menu state", (int)menu_result);
	}
	if (input_result != 1) {
		return dialog_fail("input state", input_result);
	}
	if (strcmp(name, "Adam") != 0) {
		return dialog_fail("name state", 0);
	}
	if (list.selected != 2u) {
		return dialog_fail("list state", (int)list.selected);
	}
	if (checked != 1u) {
		return dialog_fail("checked state", (int)checked);
	}
	if (confirm_result != TUI_ID_NO) {
		return dialog_fail("confirm state", (int)confirm_result);
	}
	tui_shutdown();
	printf("TUI DIALOG PASS name=%s sel=%u checked=%u menu=%u input=%d confirm=%u\r\n",
	       name, (unsigned int)list.selected, (unsigned int)checked,
	       (unsigned int)menu_result, input_result,
	       (unsigned int)confirm_result);
	return 0;
}
