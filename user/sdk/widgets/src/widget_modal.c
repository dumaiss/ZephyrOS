/* Separately linked modal member: message and confirmation boxes.
 *
 * The member owns the only hidden widget backing store, one static 60 x
 * 16 cell buffer.  It saves the covered region before drawing and
 * restores it on every exit path.  Modal calls are non-reentrant.
 */

#include "widget_internal.h"

#define W_MODAL_COLS 60u
#define W_MODAL_ROWS 16u

static struct tui_cell w_modal_cells[W_MODAL_COLS * W_MODAL_ROWS];
static uint8_t w_modal_active;

static const tui_id_t w_button_order[4] = {
	TUI_ID_OK, TUI_ID_YES, TUI_ID_NO, TUI_ID_CANCEL
};

static const char *w_button_label(tui_id_t id)
{
	switch (id) {
	case TUI_ID_OK:
		return "OK";
	case TUI_ID_YES:
		return "YES";
	case TUI_ID_NO:
		return "NO";
	case TUI_ID_CANCEL:
		return "CANCEL";
	default:
		return "";
	}
}

static void w_modal_box(struct tui_rect *box)
{
	struct tui_rect window;

	(void)tui_get_window(&window);
	box->w = window.w > W_MODAL_COLS ? W_MODAL_COLS : window.w;
	box->h = window.h > W_MODAL_ROWS ? W_MODAL_ROWS : window.h;
	box->x = (uint8_t)((window.w - box->w) / 2u);
	box->y = (uint8_t)((window.h - box->h) / 2u);
}

static void w_modal_put(uint8_t x, uint8_t y, char ch)
{
	struct tui_rect cell;

	cell.x = x;
	cell.y = y;
	cell.w = 1u;
	cell.h = 1u;
	(void)tui_fill(&cell, ch, TUI_STYLE_NORMAL);
}

static void w_modal_message(const struct tui_rect *box, const char *message)
{
	uint8_t width = (uint8_t)(box->w - 4u);
	uint8_t lines = (uint8_t)(box->h - 4u);
	uint8_t line = 0u;
	uint8_t col = 0u;
	const char *p = message;

	while (*p != '\0' && line < lines) {
		const char *word;
		uint16_t length = 0u;
		uint16_t i = 0u;

		if (*p == '\n') {
			++p;
			++line;
			col = 0u;
			continue;
		}
		if (*p == ' ') {
			++p;
			continue;
		}
		word = p;
		while (word[length] != '\0' && word[length] != ' ' &&
		       word[length] != '\n') {
			++length;
		}
		if (col != 0u) {
			if (col >= width) {
				++line;
				col = 0u;
				if (line >= lines) {
					break;
				}
			} else {
				w_modal_put((uint8_t)(box->x + 2u + col),
					    (uint8_t)(box->y + 1u + line), ' ');
				++col;
			}
		}
		while (i < length && line < lines) {
			if (col >= width) {
				++line;
				col = 0u;
				if (line >= lines) {
					break;
				}
			}
			w_modal_put((uint8_t)(box->x + 2u + col),
				    (uint8_t)(box->y + 1u + line),
				    word[i]);
			++col;
			++i;
		}
		p = word + length;
	}
}

static void w_modal_buttons(const struct tui_rect *box,
			    const tui_id_t *ids, uint8_t count,
			    uint8_t focus)
{
	uint16_t total = 0u;
	uint16_t start;
	uint8_t i;

	for (i = 0u; i < count; ++i) {
		total = (uint16_t)(total + w_strlen(w_button_label(ids[i])) +
				   5u);
	}
	if (total > box->w) {
		total = box->w;
	}
	start = (uint16_t)box->x + (uint16_t)((box->w - total) / 2u);
	if (start <= (uint16_t)box->x) {
		start = (uint16_t)box->x + 1u;
	}
	for (i = 0u; i < count; ++i) {
		const char *label = w_button_label(ids[i]);
		uint16_t width = (uint16_t)(w_strlen(label) + 4u);

		if (start >= (uint16_t)(box->x + box->w)) {
			break;
		}
		(void)tui_button((uint8_t)start,
				 (uint8_t)(box->y + box->h - 2u),
				 label, ids[i], i == focus);
		start = (uint16_t)(start + width + 1u);
	}
}

static tui_id_t w_modal_run(const char *title, const char *message,
			    uint16_t buttons)
{
	struct tui_rect box;
	tui_id_t present[4];
	uint8_t count = 0u;
	uint8_t focus = 0u;
	uint8_t i;
	tui_id_t result = TUI_ID_NONE;
	tui_key_t key;

	if (title == (const char *)0 || message == (const char *)0 ||
	    !w_button_mask_valid(buttons) || w_modal_active != 0u) {
		return TUI_ID_NONE;
	}
	if (tui_get_window(&box) < 0) {
		return TUI_ID_NONE;
	}
	w_modal_box(&box);
	if (box.w < 5u || box.h < 5u) {
		return TUI_ID_NONE;
	}
	for (i = 0u; i < 4u; ++i) {
		if ((uint16_t)(buttons & TUI_BUTTON(w_button_order[i])) != 0u) {
			present[count] = w_button_order[i];
			++count;
		}
	}
	if (count == 0u) {
		return TUI_ID_NONE;
	}
	if (tui_saved_size(&box) == 0u ||
	    tui_saved_size(&box) > (uint16_t)sizeof(w_modal_cells) ||
	    tui_save(&box, w_modal_cells,
		     (uint16_t)sizeof(w_modal_cells)) < 0) {
		return TUI_ID_NONE;
	}
	w_modal_active = 1u;

	w_blank(box.x, box.y, box.w, box.h, TUI_STYLE_NORMAL);
	if (tui_frame_title(&box, TUI_FRAME_SINGLE, TUI_STYLE_TITLE,
			    title) < 0) {
		goto done;
	}
	w_modal_message(&box, message);
	w_modal_buttons(&box, present, count, focus);
	if (tui_flush() < 0) {
		goto done;
	}

	for (;;) {
		key = tui_getkey();
		if (key == TUI_KEY_ESC) {
			for (i = 0u; i < count; ++i) {
				if (present[i] == TUI_ID_CANCEL) {
					result = TUI_ID_CANCEL;
					break;
				}
			}
			break;
		}
		if (key == 0x000Du || key == 0x000Au) {
			result = present[focus];
			break;
		}
		if (key == TUI_KEY_LEFT || key == TUI_KEY_RIGHT ||
		    key == 0x0009u) {
			if (key == TUI_KEY_LEFT) {
				if (focus == 0u) {
					focus = (uint8_t)(count - 1u);
				} else {
					--focus;
				}
			} else {
				++focus;
				if (focus >= count) {
					focus = 0u;
				}
			}
			w_modal_buttons(&box, present, count, focus);
			if (tui_flush() < 0) {
				goto done;
			}
		}
	}

done:
	if (tui_restore(&box, w_modal_cells,
			(uint16_t)sizeof(w_modal_cells)) < 0) {
		result = TUI_ID_NONE;
	}
	w_modal_active = 0u;
	(void)tui_flush();
	return result;
}

tui_id_t tui_message_box(const char *title, const char *message,
			 uint16_t buttons)
{
	return w_modal_run(title, message, buttons);
}

tui_id_t tui_confirm(const char *title, const char *message)
{
	return w_modal_run(title, message,
			   (uint16_t)(TUI_BUTTON(TUI_ID_YES) |
				      TUI_BUTTON(TUI_ID_NO)));
}
