/* KonaOS TUI SDK: optional procedural widgets.
 *
 * `libtui_widgets` is a separate archive over the public `libtui` core.
 * It has no object model, callbacks, heap, event loop, terminal
 * sequences, CP850 constants, or OS calls.  All coordinates are relative
 * to the active logical window; controls clip to it.
 *
 * Results.  Render-only calls return 0 or a negated KOS_E* value.  A null
 * required pointer or an invalid mask/rectangle is -KOS_EINVAL.
 * `tui_menu_run` returns the chosen item ID or TUI_ID_NONE on ESC or
 * invalid input.  `tui_message_box` and `tui_confirm` return the chosen
 * TUI_ID_* or TUI_ID_NONE when cancelled/refused.  `tui_input_run`
 * returns 1 on ENTER, 0 on ESC, or a negative error.
 *
 * Buttons.  A `buttons` mask has bit TUI_ID_* set for each button, using
 * TUI_BUTTON().  The only valid bits are OK, CANCEL, YES and NO; a zero
 * or foreign-bit mask is invalid.  Buttons are ordered OK, YES, NO,
 * CANCEL.  The first present button has focus; left/right or TAB moves
 * focus, ENTER chooses the focused ID, and ESC chooses CANCEL when
 * present or TUI_ID_NONE otherwise.
 *
 * Navigation.  Menus use left/right, ENTER, their declared shortcut, and
 * ESC.  A list uses arrows, HOME/END and PGUP/PGDN.  Input editing
 * accepts printable bytes, left/right, HOME/END, BS/DELETE; ENTER commits
 * and ESC cancels.  `tui_menubar` treats `selected` as an in/out item ID
 * (not an index); `tui_menu_run` starts on the first item.
 *
 * Modal storage.  Message and confirmation boxes are non-reentrant.  A
 * nested modal call is refused.  The separately linked modal member owns
 * one static 60 x 16 cell save buffer; render-only programs do not link
 * it.  Every modal exit restores its covered region exactly.
 */

#ifndef KONA_TUI_WIDGETS_H
#define KONA_TUI_WIDGETS_H

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

struct tui_list {
	const char * const *items;
	uint16_t count;
	uint16_t selected;
	uint16_t top;
};

/* ---- status / menu -------------------------------------------------- */

int tui_statusbar(const char *text);

int tui_menubar(const struct tui_menu_item *items,
		uint8_t count,
		tui_id_t *selected);

/* ---- simple dialogs ------------------------------------------------ */

tui_id_t tui_message_box(const char *title,
			 const char *message,
			 uint16_t buttons);

tui_id_t tui_confirm(const char *title,
		     const char *message);

/* ---- controls ------------------------------------------------------- */

int tui_button(uint8_t x, uint8_t y,
	       const char *label,
	       tui_id_t id,
	       uint8_t focused);

int tui_label(uint8_t x, uint8_t y,
	      const char *text);

int tui_input(uint8_t x, uint8_t y,
	      uint8_t width,
	      char *buffer,
	      uint16_t buffer_size);

int tui_checkbox(uint8_t x, uint8_t y,
		 const char *label,
		 uint8_t *checked);

int tui_listbox(const struct tui_rect *rect,
		struct tui_list *list);

/* ---- common interaction helpers ------------------------------------ */

tui_id_t tui_menu_run(const struct tui_menu_item *items,
		      uint8_t count);

int tui_input_run(uint8_t x, uint8_t y,
		  uint8_t width,
		  char *buffer,
		  uint16_t buffer_size);

#endif /* KONA_TUI_WIDGETS_H */
