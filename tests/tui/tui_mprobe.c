/* The TUI mouse outcome program for tests/test_tui_sim.py.
 *
 * It enables the mouse and records what the key loop returns, in phases
 * the replay feeds one at a time (each phase draws its name on the bottom
 * row first, "phase=<name>"):
 *
 *   queued  in a 20 x 10 window at (10, 5): waits for a pending key, lets
 *           the rest of the typed keys arrive, then takes three keys -
 *           the typed ones first, then the mouse event;
 *   events  every key until a right press (the press is recorded);
 *   motion  the same with TUI_MOUSE_MOTION;
 *   menu    full screen: tui_menu_run twice;
 *   box     tui_message_box with OK and CANCEL;
 *   wait    tui_getkey() until a keyboard key (the release of the last
 *           click comes first), which the replay sends.
 *
 * After tui_shutdown it prints one line per record and "mprobe: done".
 * Without a mouse (the serial console) it prints the enable and get
 * results, takes one key and prints it.
 */

#include "kos.h"
#include "tui.h"
#include "tui_widgets.h"

#define PROBE_RECORDS 40u

struct probe_record {
	tui_key_t key;
	struct tui_mouse ev;
};

static struct probe_record records[PROBE_RECORDS];
static uint8_t record_count;
static tui_id_t menu_results[2];
static tui_id_t box_result;

static const struct tui_menu_item menu_items[] = {
	{"File", 'f', 10u},
	{"Edit", 'e', 11u},
	{"Help", 'h', 12u}
};

/* The phase name on the bottom row as "phase=<name>": the row is dashed
 * and flushed first, so every cell of the name is drawn again and the
 * replay sees the whole word. */
static void phase(const char *name)
{
	static const struct tui_rect screen = {0u, 0u, 85u, 26u};
	static const struct tui_rect row = {0u, 25u, 20u, 1u};

	(void)tui_push_window(&screen);
	(void)tui_fill(&row, '-', TUI_STYLE_NORMAL);
	(void)tui_flush();
	(void)tui_puts_xy(0u, 25u, "phase=");
	(void)tui_puts_xy(6u, 25u, name);
	(void)tui_flush();
	(void)tui_pop_window();
}

static tui_key_t take(void)
{
	tui_key_t key = tui_getkey();
	struct probe_record *r = &records[record_count];

	if (record_count < PROBE_RECORDS) {
		r->key = key;
		if (key == TUI_KEY_MOUSE) {
			(void)tui_mouse_get(&r->ev);
		}
		++record_count;
	}
	return key;
}

static void until_right_press(void)
{
	for (;;) {
		if (take() == TUI_KEY_MOUSE &&
		    (records[record_count - 1u].ev.pressed &
		     TUI_MOUSE_RIGHT) != 0u) {
			return;
		}
	}
}

static int keyboard_only(int enabled)
{
	struct tui_mouse ev;
	int got = tui_mouse_get(&ev);
	tui_key_t key = tui_getkey();

	tui_shutdown();
	printf("mprobe: enable %d get %d key %04x\r\n", enabled, got,
	       (unsigned int)key);
	return 0;
}

int main(void)
{
	static const struct tui_rect inner = {10u, 5u, 20u, 10u};
	uint8_t i;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("mprobe: FAIL init %d\r\n", result);
		return 1;
	}
	result = tui_mouse_enable(0u);
	if (result < 0) {
		return keyboard_only(result);
	}

	(void)tui_push_window(&inner);
	phase("queued");
	while (tui_key_pending() <= 0) {
		idle();
	}
	for (i = 0u; i < 16u; ++i) {
		idle();
	}
	(void)take();
	(void)take();
	(void)take();

	phase("events");
	until_right_press();
	(void)tui_mouse_enable(TUI_MOUSE_MOTION);
	phase("motion");
	until_right_press();
	(void)tui_mouse_enable(0u);
	(void)tui_pop_window();

	phase("menu1");
	menu_results[0] = tui_menu_run(menu_items, 3u);
	phase("menu2");
	menu_results[1] = tui_menu_run(menu_items, 3u);
	phase("box");
	box_result = tui_message_box("Box", "Click a button",
				     (uint16_t)(TUI_BUTTON(TUI_ID_OK) |
						TUI_BUTTON(TUI_ID_CANCEL)));
	phase("wait");
	while (tui_getkey() == TUI_KEY_MOUSE) {
	}
	phase("end");
	tui_shutdown();

	for (i = 0u; i < record_count; ++i) {
		const struct probe_record *r = &records[i];

		if (r->key != TUI_KEY_MOUSE) {
			printf("mprobe: key %04x\r\n", (unsigned int)r->key);
			continue;
		}
		printf("mprobe: mouse %u %u %u %d %d %u %u %u %d\r\n",
		       (unsigned int)r->ev.kind, (unsigned int)r->ev.col,
		       (unsigned int)r->ev.row, (int)r->ev.wcol,
		       (int)r->ev.wrow, (unsigned int)r->ev.buttons,
		       (unsigned int)r->ev.pressed,
		       (unsigned int)r->ev.released, (int)r->ev.wheel);
	}
	printf("mprobe: menu %u %u box %u\r\n",
	       (unsigned int)menu_results[0], (unsigned int)menu_results[1],
	       (unsigned int)box_result);
	printf("mprobe: done\r\n");
	return 0;
}
