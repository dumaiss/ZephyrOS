/* KonaOS SDK example: the mouse in a TUI program.
 *
 * Built by the examples Makefile with `LIBS=tui_widgets:tui`.  One call,
 * tui_mouse_enable(), shows the pointer; clicks, releases and the wheel
 * then arrive through tui_getkey() as TUI_KEY_MOUSE, and tui_mouse_get()
 * says where.  The form has a menu bar (a click on the bar opens the
 * menu, a click on an item chooses it), a checkbox and a button clicked
 * through tui_mouse_hit(), a list the wheel scrolls and a click selects,
 * and a confirm box on Quit.  The keys work alongside: F10 opens the
 * menu, SPACE toggles the box, ENTER presses the button, the arrows move
 * the list and ESC asks to quit.  Without a mouse (the serial console)
 * the form is keyboard-only.  The status line shows the last event.
 */

#include "kos.h"
#include "tui.h"
#include "tui_widgets.h"

#define ID_FILE 10u
#define ID_VIEW 11u
#define ID_QUIT 12u

#define LIST_COUNT 20u

static const struct tui_menu_item menu_items[] = {
	{"File", 'f', ID_FILE},
	{"View", 'v', ID_VIEW},
	{"Quit", 'q', ID_QUIT}
};

static const char * const list_items[LIST_COUNT] = {
	"Mercury", "Venus", "Earth", "Mars", "Jupiter", "Saturn", "Uranus",
	"Neptune", "Ceres", "Pluto", "Haumea", "Makemake", "Eris", "Moon",
	"Phobos", "Deimos", "Io", "Europa", "Ganymede", "Callisto"
};

static const struct tui_rect bar = {0u, 0u, 85u, 1u};
static const struct tui_rect box_rect = {2u, 3u, 9u, 1u};    /* "[X] Sound" */
static const struct tui_rect button_rect = {2u, 5u, 9u, 1u}; /* "[ Count ]" */
static const struct tui_rect list_rect = {30u, 3u, 24u, 12u};

static const char *const kind_names[] = {
	"none", "press", "release", "wheel", "move"
};

static struct tui_list planets;
static uint8_t checked;
static uint16_t clicks;
static tui_id_t last_menu;
static char status[86];

static void draw(void)
{
	tui_id_t selected = last_menu;

	(void)tui_menubar(menu_items, 3u, &selected);
	(void)tui_checkbox(box_rect.x, box_rect.y, "Sound", &checked);
	(void)tui_button(button_rect.x, button_rect.y, "Count", 1u, 0u);
	(void)tui_printf_xy(2u, 7u, "count %u   ", (unsigned int)clicks);
	(void)tui_listbox(&list_rect, &planets);
	(void)tui_statusbar(status);
	(void)tui_flush();
}

static void list_step(int16_t delta)
{
	while (delta < 0 && planets.selected + 1u < planets.count) {
		++planets.selected;
		++delta;
	}
	while (delta > 0 && planets.selected > 0u) {
		--planets.selected;
		--delta;
	}
}

/* 1 when the user chose to quit. */
static int menu(void)
{
	last_menu = tui_menu_run(menu_items, 3u);
	if (last_menu == ID_QUIT) {
		return tui_confirm("Quit", "Leave the mouse example?") ==
		       TUI_ID_YES;
	}
	return 0;
}

/* 1 when the user chose to quit. */
static int on_mouse(void)
{
	struct tui_mouse ev;

	if (tui_mouse_get(&ev) < 0) {
		return 0;
	}
	snprintf(status, sizeof(status), "%s at %u,%u buttons %u wheel %d",
		kind_names[ev.kind], (unsigned int)ev.col,
		(unsigned int)ev.row, (unsigned int)ev.buttons,
		(int)ev.wheel);
	if (ev.wheel != 0 && tui_mouse_hit(&ev, &list_rect)) {
		list_step(ev.wheel);
	}
	if ((ev.pressed & TUI_MOUSE_LEFT) == 0u) {
		return 0;
	}
	if (tui_mouse_hit(&ev, &bar)) {
		return menu();
	}
	if (tui_mouse_hit(&ev, &box_rect)) {
		checked = (uint8_t)!checked;
	} else if (tui_mouse_hit(&ev, &button_rect)) {
		++clicks;
	} else if (tui_mouse_hit(&ev, &list_rect) &&
		   ev.wrow > (int8_t)list_rect.y &&
		   ev.wrow < (int8_t)(list_rect.y + list_rect.h - 1u)) {
		uint16_t index = (uint16_t)(planets.top + (uint16_t)(ev.wrow -
						  (int8_t)list_rect.y - 1));

		if (index < planets.count) {
			planets.selected = index;
		}
	}
	return 0;
}

int main(void)
{
	tui_key_t key;
	int mouse;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("TUI MOUSE FAIL init %d\r\n", result);
		return 1;
	}
	planets.items = list_items;
	planets.count = LIST_COUNT;
	mouse = tui_mouse_enable(0u);
	if (mouse < 0) {
		snprintf(status, sizeof(status), "no mouse (%d): keys only",
			 mouse);
	} else {
		(void)strlcpy(status, "click, drag or turn the wheel",
			      sizeof(status));
	}
	for (;;) {
		draw();
		key = tui_getkey();
		if (key == TUI_KEY_MOUSE) {
			if (on_mouse()) {
				break;
			}
			continue;
		}
		snprintf(status, sizeof(status), "key %04x", (unsigned int)key);
		if (key == TUI_KEY_F10) {
			if (menu()) {
				break;
			}
		} else if (key == ' ') {
			checked = (uint8_t)!checked;
		} else if (key == 0x000Du) {
			++clicks;
		} else if (key == TUI_KEY_DOWN) {
			list_step(-1);
		} else if (key == TUI_KEY_UP) {
			list_step(1);
		} else if (key == TUI_KEY_ESC) {
			if (tui_confirm("Quit", "Leave the mouse example?") ==
			    TUI_ID_YES) {
				break;
			}
		}
	}
	tui_shutdown();
	printf("TUI MOUSE PASS mouse=%d checked=%u clicks=%u sel=%u menu=%u\r\n",
	       mouse < 0 ? mouse : 1, (unsigned int)checked,
	       (unsigned int)clicks, (unsigned int)planets.selected,
	       (unsigned int)last_menu);
	return 0;
}
