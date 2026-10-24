#include "kos.h"
#include "tui.h"

#include "ke_file.h"
#include "ke_history.h"
#include "ke_platform.h"
#include "ke_store.h"
#include "ke_text.h"
#include "ke_ui.h"
#include "ke_view.h"
#include "ke_widgets.h"

#define KE_MIN_COLS 60u
#define KE_MIN_ROWS 16u
#define KE_POPUP_WIDTH 18u
#define KE_POPUP_ROWS 9u
#define KE_DIALOG_WIDTH 36u
#define KE_DIALOG_HEIGHT 18u
#define KE_DIR_PAGE 8u

enum ke_action {
	ACT_NONE,
	ACT_NEW,
	ACT_OPEN,
	ACT_SAVE,
	ACT_SAVE_AS,
	ACT_EXIT,
	ACT_UNDO,
	ACT_REDO,
	ACT_CUT,
	ACT_COPY,
	ACT_PASTE,
	ACT_DELETE,
	ACT_SELECT_ALL,
	ACT_FIND,
	ACT_FIND_NEXT,
	ACT_REPLACE,
	ACT_GOTO,
	ACT_KEYS,
	ACT_ABOUT,
	ACT_MENU_LEFT = 250,
	ACT_MENU_RIGHT
};

/* What a key did to the document lines on screen. */
enum ke_view_edit {
	VIEW_NONE,
	VIEW_INLINE,   /* one byte added, removed or replaced in a line */
	VIEW_NEWLINE,  /* a newline inserted on the cursor's row */
	VIEW_JOIN,     /* the newline ending a row's line removed */
	VIEW_REBUILD   /* anything else */
};

struct ke_action_item {
	const char *label;
	uint8_t action;
};

static const struct tui_menu_item menu_bar[] = {
	{"File", TUI_KEY_NONE, 10u},
	{"Edit", TUI_KEY_NONE, 11u},
	{"Search", TUI_KEY_NONE, 12u},
	{"Help", TUI_KEY_NONE, 13u}
};
static const struct ke_action_item file_menu[] = {
	{"New", ACT_NEW}, {"Open        F3", ACT_OPEN},
	{"Save        F2", ACT_SAVE}, {"Save As", ACT_SAVE_AS},
	{"Exit       F10", ACT_EXIT}
};
static const struct ke_action_item edit_menu[] = {
	{"Undo", ACT_UNDO}, {"Redo", ACT_REDO}, {"Cut", ACT_CUT},
	{"Copy        F8", ACT_COPY}, {"Paste       F9", ACT_PASTE},
	{"Delete", ACT_DELETE}, {"Select All", ACT_SELECT_ALL}
};
static const struct ke_action_item search_menu[] = {
	{"Find        F4", ACT_FIND}, {"Find Next   F5", ACT_FIND_NEXT},
	{"Replace", ACT_REPLACE}, {"Go To Line", ACT_GOTO}
};
static const struct ke_action_item help_menu[] = {
	{"Keys", ACT_KEYS}, {"About", ACT_ABOUT}
};

static struct ke_store document;
static struct ke_history history;
static struct tui_size screen;
/* Modal surfaces never nest: a menu is restored before its action opens a
 * dialog, and dialogs close before an error/confirmation box is shown. */
static union {
	struct tui_cell popup[KE_POPUP_WIDTH * KE_POPUP_ROWS];
	struct tui_cell dialog[KE_DIALOG_WIDTH * KE_DIALOG_HEIGHT];
} modal_saved;
#define popup_saved modal_saved.popup
#define dialog_saved modal_saved.dialog
static struct ke_dir_entry directory_entries[KE_DIR_PAGE];
static char current_path[KE_PATH_MAX];
static char dialog_path[KE_PATH_MAX];
static char dialog_name[KE_PATH_MAX];
static char recovery_path[KE_PATH_MAX];
static char find_text[96];
static char replace_text[96];
static char status_text[129];
/* What the status row shows: rebuilt only when one of these changes. */
static uint32_t status_line;
static uint32_t status_column;
static uint8_t status_flags;
static uint8_t status_valid;
static uint8_t attr_status;
static ke_pos_t cursor_position;
static ke_pos_t mark_anchor;
static ke_pos_t match_position;
static uint16_t match_length;
static uint8_t modified;
static uint8_t insert_mode;
static uint8_t mark_active;
static uint8_t have_match;
static uint8_t match_case;

static void set_text(char *target, const char *source, uint16_t capacity)
{
	(void)strlcpy(target, source, capacity);
}

static ke_pos_t next_line(ke_pos_t position)
{
	ke_pos_t end = ke_line_end(&document, position);

	return end < ke_store_length(&document) ? end + 1ul : end;
}

static ke_pos_t previous_line(ke_pos_t position)
{
	ke_pos_t start = ke_line_start(&document, position);

	if (start == 0ul) return 0ul;
	return ke_line_start(&document, start - 1ul);
}

static void selection(ke_pos_t *first, ke_pos_t *last)
{
	if (mark_active == 0u || mark_anchor == cursor_position) {
		*first = cursor_position;
		*last = cursor_position;
	} else if (mark_anchor < cursor_position) {
		*first = mark_anchor;
		*last = cursor_position;
	} else {
		*first = cursor_position;
		*last = mark_anchor;
	}
}

/* The line number of `position`'s line, counted from the start: only a
 * jump without a known line needs it. */
static uint32_t count_line(ke_pos_t position)
{
	return ke_store_count(&document, 0ul,
			      ke_line_start(&document, position), 10u);
}

/* The view with its top kept (snapped, its number counted again), as an
 * action that may have changed anything leaves it. */
static void rebuild_view(void)
{
	ke_pos_t top = ke_view.row_start[0];

	if (top == KE_POS_NONE || top > ke_store_length(&document))
		top = ke_store_length(&document);
	top = ke_line_start(&document, top);
	ke_view_rebuild(top, count_line(top));
}

/* Bring the cursor's line into the view: one line past either edge
 * scrolls, anything further rebuilds the view with that line on top.
 * Returns the cursor's row. */
static uint8_t follow_cursor(void)
{
	uint8_t row = ke_view_row_of(cursor_position);
	ke_pos_t below = ke_view.row_start[ke_view.rows];
	ke_pos_t top = ke_view.row_start[0];

	if (row != KE_VIEW_NONE) return row;
	if (below != KE_POS_NONE && cursor_position >= below) {
		ke_pos_t after = ke_view_next_start(below);

		if ((after == KE_POS_NONE || cursor_position < after) &&
		    ke_view_scroll(1)) return (uint8_t)(ke_view.rows - 1u);
	} else if (top != KE_POS_NONE && top != 0ul && cursor_position < top &&
		   cursor_position >= ke_line_start(&document, top - 1ul) &&
		   ke_view_scroll(-1)) {
		return 0u;
	}
	ke_view_rebuild(cursor_position, count_line(cursor_position));
	row = ke_view_row_of(cursor_position);
	return row == KE_VIEW_NONE ? 0u : row;
}

/* Keep the cursor's column inside the window; a horizontal move redraws
 * every row. */
static void follow_column(uint32_t column)
{
	uint32_t left = ke_view.left_column;

	if (column < left) left = column;
	if (column >= left + screen.cols) left = column - screen.cols + 1ul;
	if (left != ke_view.left_column) {
		ke_view.left_column = left;
		ke_view_draw_rows(0u, (uint8_t)(ke_view.rows - 1u));
	}
}

static const uint32_t decimal_places[] = {
	1000000000ul, 100000000ul, 10000000ul, 1000000ul, 100000ul,
	10000ul, 1000ul, 100ul, 10ul
};

static char *put_text(char *at, const char *text)
{
	while (*text != '\0') *at++ = *text++;
	return at;
}

/* Decimal digits without division: subtract each power of ten. */
static char *put_number(char *at, uint32_t value)
{
	uint8_t place;
	uint8_t started = 0u;

	for (place = 0u; place < sizeof(decimal_places) /
				 sizeof(decimal_places[0]); ++place) {
		char digit = '0';

		while (value >= decimal_places[place]) {
			value -= decimal_places[place];
			++digit;
		}
		if (digit != '0' || started) {
			*at++ = digit;
			started = 1u;
		}
	}
	*at++ = (char)('0' + (uint8_t)value);
	return at;
}

/* The status tail is at most 42 bytes: the name keeps the rest. */
#define STATUS_NAME_MAX 46u

/* The status row, written only when what it shows changed (an action
 * that renames the document redraws everything: render_all). */
static void render_status(uint32_t cursor_line, uint32_t column)
{
	char *at = status_text;
	uint8_t cols = ke_view.cols;
	uint8_t flags = (uint8_t)(modified | (insert_mode << 1) |
				  (mark_active << 2));

	if (status_valid && cursor_line == status_line &&
	    column == status_column && flags == status_flags) return;
	status_line = cursor_line;
	status_column = column;
	status_flags = flags;
	status_valid = 1u;
	at += strlcpy(status_text, current_path[0] != '\0' ?
		      (const char *)current_path : "Untitled", STATUS_NAME_MAX);
	if (at > status_text + STATUS_NAME_MAX - 1u)
		at = status_text + STATUS_NAME_MAX - 1u;
	if (modified != 0u) at = put_text(at, " *");
	at = put_number(put_text(at, "  Ln "), cursor_line + 1ul);
	at = put_number(put_text(at, " Col "), column + 1ul);
	at = put_text(at, insert_mode != 0u ? "  INS" : "  OVR");
	if (mark_active != 0u) at = put_text(at, " MARK");
	while (at < status_text + cols) *at++ = ' ';
	(void)tui_put_text(0u, (uint8_t)(screen.rows - 1u), status_text, cols,
			   attr_status);
}

static void place_cursor(uint8_t row, uint32_t column)
{
	uint32_t x = column < ke_view.left_column ? 0ul :
		     column - ke_view.left_column;

	if (x >= screen.cols) x = screen.cols - 1u;
	(void)tui_gotoxy((uint8_t)x, (uint8_t)(ke_view.first_row + row));
	(void)tui_cursor_show();
}

/* The status row and the cursor for the cursor's current place. */
static void show_cursor(uint8_t row)
{
	uint32_t column = ke_visual_column(&document, cursor_position);

	follow_column(column);
	render_status(ke_view.top_line + row, column);
	place_cursor(row, column);
}

static void set_view_selection(void)
{
	selection(&ke_view.select_first, &ke_view.select_last);
}

/* The whole screen: the menu bar, every row, the status row.  Used at
 * start and after actions; modal surfaces restore what they covered, so
 * only changed cells reach the console.  KonaEdit never narrows the
 * window, so the window is always the whole screen. */
static void render_all(void)
{
	tui_id_t none = TUI_ID_NONE;

	tui_begin();
	(void)tui_set_style(TUI_STYLE_NORMAL);
	(void)ke_menubar(menu_bar, 4u, none);
	set_view_selection();
	rebuild_view();
	status_valid = 0u;
	show_cursor(follow_cursor());
	(void)tui_end();
}

static tui_id_t message_box(const char *title, const char *message,
			    uint16_t buttons)
{
	struct tui_rect box;
	tui_id_t ids[4];
	uint8_t count = 0u;
	uint8_t focus = 0u;
	uint16_t need;

	if (buttons & TUI_BUTTON(TUI_ID_OK)) ids[count++] = TUI_ID_OK;
	if (buttons & TUI_BUTTON(TUI_ID_YES)) ids[count++] = TUI_ID_YES;
	if (buttons & TUI_BUTTON(TUI_ID_NO)) ids[count++] = TUI_ID_NO;
	if (buttons & TUI_BUTTON(TUI_ID_CANCEL)) ids[count++] = TUI_ID_CANCEL;
	if (count == 0u) return TUI_ID_NONE;
	box.w = screen.cols > KE_DIALOG_WIDTH ? KE_DIALOG_WIDTH : screen.cols;
	box.h = 10u;
	box.x = (uint8_t)((screen.cols - box.w) / 2u);
	box.y = (uint8_t)((screen.rows - box.h) / 2u);
	need = tui_saved_size(&box);
	if (need == 0u || need > sizeof(dialog_saved) ||
	    tui_save(&box, dialog_saved, sizeof(dialog_saved)) < 0)
		return TUI_ID_NONE;
	for (;;) {
		uint8_t i;
		tui_key_t key;

		tui_begin();
		(void)tui_fill(&box, ' ', TUI_STYLE_NORMAL);
		(void)tui_frame_title(&box, TUI_FRAME_SINGLE, TUI_STYLE_TITLE,
				      title);
		(void)ke_label((uint8_t)(box.x + 2u), (uint8_t)(box.y + 3u),
				message);
		for (i = 0u; i < count; ++i) {
			const char *label = ids[i] == TUI_ID_OK ? "OK" :
				ids[i] == TUI_ID_YES ? "YES" :
				ids[i] == TUI_ID_NO ? "NO" : "CANCEL";
			(void)ke_button((uint8_t)(box.x + 3u + i * 10u),
					 (uint8_t)(box.y + 7u), label,
					 i == focus);
		}
		(void)tui_cursor_hide();
		(void)tui_end();
		key = tui_getkey();
		if (key == 13u || key == 10u) {
			tui_id_t result = ids[focus];
			(void)tui_restore(&box, dialog_saved, sizeof(dialog_saved));
			return result;
		}
		if (key == TUI_KEY_ESC) {
			(void)tui_restore(&box, dialog_saved, sizeof(dialog_saved));
			return (buttons & TUI_BUTTON(TUI_ID_CANCEL)) ?
				TUI_ID_CANCEL : TUI_ID_NONE;
		}
		if (key == TUI_KEY_LEFT) focus = focus == 0u ?
					   (uint8_t)(count - 1u) :
					   (uint8_t)(focus - 1u);
		else if (key == TUI_KEY_RIGHT || key == 9u)
			focus = (uint8_t)((focus + 1u) % count);
	}
}

static tui_id_t confirm_box(const char *title, const char *message)
{
	return message_box(title, message,
		TUI_BUTTON(TUI_ID_YES) | TUI_BUTTON(TUI_ID_NO));
}

static int prompt_text(const char *title, char *buffer, uint16_t capacity)
{
	struct tui_rect box;
	uint16_t need;
	uint16_t length = strlen(buffer);

	box.w = screen.cols > KE_DIALOG_WIDTH ? KE_DIALOG_WIDTH : screen.cols;
	box.h = 7u;
	box.x = (uint8_t)((screen.cols - box.w) / 2u);
	box.y = (uint8_t)((screen.rows - box.h) / 2u);
	need = tui_saved_size(&box);
	if (need == 0u || need > sizeof(dialog_saved) ||
	    tui_save(&box, dialog_saved, sizeof(dialog_saved)) < 0) return 0;
	tui_begin();
	(void)tui_fill(&box, ' ', TUI_STYLE_NORMAL);
	(void)tui_frame_title(&box, TUI_FRAME_SINGLE, TUI_STYLE_TITLE, title);
	(void)ke_label((uint8_t)(box.x + 2u), (uint8_t)(box.y + 2u),
			"Enter value:");
	(void)ke_input((uint8_t)(box.x + 2u), (uint8_t)(box.y + 4u),
			(uint8_t)(box.w - 4u), buffer, capacity);
	(void)tui_end();
	for (;;) {
		tui_key_t key;

		tui_begin();
		(void)ke_input((uint8_t)(box.x + 2u), (uint8_t)(box.y + 4u),
				(uint8_t)(box.w - 4u), buffer, capacity);
		(void)tui_gotoxy((uint8_t)(box.x + 2u +
			(length < box.w - 5u ? length : box.w - 5u)),
			(uint8_t)(box.y + 4u));
		(void)tui_cursor_show();
		(void)tui_end();
		key = tui_getkey();
		if (key == TUI_KEY_ESC) {
			(void)tui_restore(&box, dialog_saved, sizeof(dialog_saved));
			return 0;
		}
		if (key == 13u || key == 10u) {
			(void)tui_restore(&box, dialog_saved, sizeof(dialog_saved));
			return 1;
		}
		if ((key == 8u || key == 127u) && length != 0u)
			buffer[--length] = '\0';
		else if (key >= 32u && key <= 255u && length + 1u < capacity) {
			buffer[length++] = (char)key;
			buffer[length] = '\0';
		}
	}
}

static int join_path(char *target, const char *directory, const char *name)
{
	uint16_t length;

	if (strlcpy(target, directory, KE_PATH_MAX) >= KE_PATH_MAX)
		return 0;
	length = strlen(target);
	if (length == 0u || target[length - 1u] != '/') {
		if (strlcat(target, "/", KE_PATH_MAX) >= KE_PATH_MAX) return 0;
	}
	return strlcat(target, name, KE_PATH_MAX) < KE_PATH_MAX;
}

static void parent_path(char *path)
{
	char *slash;

	if (strcmp(path, "/") == 0) return;
	slash = strrchr(path, '/');
	if (slash == path) path[1] = '\0';
	else if (slash != (char *)0) *slash = '\0';
}

static int load_directory(const char *path, uint16_t *cookie, uint8_t *count)
{
	int result = ke_fs_list(path, cookie, directory_entries, KE_DIR_PAGE);

	if (result < 0) return result;
	*count = (uint8_t)result;
	return 0;
}

static void draw_directory_row(const struct tui_rect *box, uint8_t row,
			       uint8_t focus, uint8_t selected_entry,
			       uint8_t entry_count)
{
	struct tui_rect line;
	tui_style_t style = focus == 0u && row == selected_entry ?
			    TUI_STYLE_SELECTED : TUI_STYLE_NORMAL;
	uint8_t length = 0u;

	line.x = (uint8_t)(box->x + 3u);
	line.y = (uint8_t)(box->y + 4u + row);
	line.w = (uint8_t)(box->w - 6u);
	line.h = 1u;
	(void)tui_fill(&line, ' ', style);
	if (row >= entry_count) return;
	(void)tui_set_style(style);
	(void)tui_puts_xy(line.x, line.y, directory_entries[row].name);
	while (length + 1u < line.w &&
	       directory_entries[row].name[length] != '\0') ++length;
	if (directory_entries[row].container != 0u)
		(void)tui_putc_xy((uint8_t)(line.x + length), line.y, '/');
}

static void draw_file_input(const struct tui_rect *box)
{
	(void)ke_input((uint8_t)(box->x + 8u), (uint8_t)(box->y + 14u),
		(uint8_t)(box->w - 10u), dialog_name, sizeof(dialog_name));
}

static void draw_file_dialog(const struct tui_rect *box, uint8_t focus,
			     uint8_t selected_entry, uint8_t entry_count,
			     uint8_t saving)
{
	struct tui_rect list_box;
	uint8_t row;

	(void)tui_fill(box, ' ', TUI_STYLE_NORMAL);
	(void)tui_frame_title(box, TUI_FRAME_DOUBLE, TUI_STYLE_TITLE,
			      saving != 0u ? "Save As" : "Open");
	(void)ke_label((uint8_t)(box->x + 2u), (uint8_t)(box->y + 1u),
			dialog_path);
	list_box.x = (uint8_t)(box->x + 2u);
	list_box.y = (uint8_t)(box->y + 3u);
	list_box.w = (uint8_t)(box->w - 4u);
	list_box.h = 10u;
	(void)tui_frame(&list_box, TUI_FRAME_SINGLE, TUI_STYLE_NORMAL);
	for (row = 0u; row < KE_DIR_PAGE; ++row)
		draw_directory_row(box, row, focus, selected_entry, entry_count);
	(void)ke_label((uint8_t)(box->x + 2u), (uint8_t)(box->y + 14u),
			"Name:");
	draw_file_input(box);
	(void)ke_button((uint8_t)(box->x + 6u), (uint8_t)(box->y + 16u),
			saving != 0u ? "Save" : "Open",
			focus == 2u);
	(void)ke_button((uint8_t)(box->x + 21u), (uint8_t)(box->y + 16u),
			"Cancel", focus == 3u);
}

static int file_dialog(uint8_t saving, char *result_path)
{
	struct tui_rect box;
	uint16_t cookies[8];
	uint8_t page = 0u;
	uint16_t cookie = 0u;
	uint8_t count = 0u;
	uint8_t selected_entry = 0u;
	uint8_t focus = 0u;
	uint16_t name_length = 0u;
	uint16_t need;

	set_text(dialog_path, "/fs", sizeof(dialog_path));
	dialog_name[0] = '\0';
	cookies[0] = 0u;
	if (load_directory(dialog_path, &cookie, &count) < 0) return 0;
	box.w = screen.cols > KE_DIALOG_WIDTH ? KE_DIALOG_WIDTH : screen.cols;
	box.h = screen.rows > KE_DIALOG_HEIGHT ? KE_DIALOG_HEIGHT : screen.rows;
	box.x = (uint8_t)((screen.cols - box.w) / 2u);
	box.y = (uint8_t)((screen.rows - box.h) / 2u);
	need = tui_saved_size(&box);
	if (need == 0u || need > sizeof(dialog_saved) ||
	    tui_save(&box, dialog_saved, sizeof(dialog_saved)) < 0) return 0;
	tui_begin();
	draw_file_dialog(&box, focus, selected_entry, count, saving);
	(void)tui_cursor_hide();
	(void)tui_end();
	for (;;) {
		tui_key_t key;
		uint8_t old_selected = selected_entry;
		uint8_t redraw = 0u;

		key = tui_getkey();
		if (key == TUI_KEY_ESC) break;
		if (key == 9u) {
			focus = (uint8_t)((focus + 1u) % 4u);
			redraw = 2u;
		}
		if (focus == 0u) {
			if (key == TUI_KEY_PGDN && count != 0u) {
				selected_entry = (uint8_t)(count - 1u);
				key = TUI_KEY_DOWN;
			} else if (key == TUI_KEY_PGUP && page != 0u) {
				selected_entry = 0u;
				key = TUI_KEY_UP;
			}
			if (key == TUI_KEY_UP && selected_entry != 0u) --selected_entry;
			else if (key == TUI_KEY_UP && page != 0u) {
				--page; cookie = cookies[page];
				(void)load_directory(dialog_path, &cookie, &count);
				selected_entry = count != 0u ? (uint8_t)(count - 1u) : 0u;
				redraw = 2u;
			}
			else if (key == TUI_KEY_DOWN && selected_entry + 1u < count)
				++selected_entry;
			else if (key == TUI_KEY_DOWN && count == KE_DIR_PAGE &&
				 page + 1u < 8u) {
				++page; cookies[page] = cookie; selected_entry = 0u;
				(void)load_directory(dialog_path, &cookie, &count);
				if (count == 0u) {
					--page; cookie = cookies[page];
					(void)load_directory(dialog_path, &cookie, &count);
					selected_entry = (uint8_t)(count - 1u);
				}
				redraw = 2u;
			} else if (key == 8u || key == 127u) {
				parent_path(dialog_path); page = 0u; cookie = 0u;
				selected_entry = 0u;
				(void)load_directory(dialog_path, &cookie, &count);
				redraw = 2u;
			} else if ((key == 13u || key == 10u) && count != 0u) {
				if (directory_entries[selected_entry].container != 0u) {
					if (join_path(result_path, dialog_path,
						      directory_entries[selected_entry].name))
						set_text(dialog_path, result_path,
							 sizeof(dialog_path));
					page = 0u; cookie = 0u; selected_entry = 0u;
					(void)load_directory(dialog_path, &cookie, &count);
					redraw = 2u;
				} else if (saving == 0u) {
					if (join_path(result_path, dialog_path,
						      directory_entries[selected_entry].name)) {
						(void)tui_restore(&box, dialog_saved,
								  sizeof(dialog_saved));
						return 1;
					}
				} else {
					set_text(dialog_name,
						 directory_entries[selected_entry].name,
						 sizeof(dialog_name));
					name_length = strlen(dialog_name);
					focus = 1u;
					redraw = 2u;
				}
			}
			if (redraw == 0u && selected_entry != old_selected) redraw = 1u;
		} else if (focus == 1u) {
			if ((key == 8u || key == 127u) && name_length != 0u) {
				dialog_name[--name_length] = '\0';
				redraw = 3u;
			} else if (key >= 32u && key <= 255u &&
				   name_length + 1u < sizeof(dialog_name)) {
				dialog_name[name_length++] = (char)key;
				dialog_name[name_length] = '\0';
				redraw = 3u;
			} else if (key == 13u || key == 10u) {
				focus = 2u; redraw = 2u;
			}
		} else if ((key == 13u || key == 10u) && focus == 2u) {
			if (dialog_name[0] != '\0' &&
			    join_path(result_path, dialog_path, dialog_name)) {
				(void)tui_restore(&box, dialog_saved, sizeof(dialog_saved));
				return 1;
			}
		} else if ((key == 13u || key == 10u) && focus == 3u) break;
		if (redraw != 0u) {
			tui_begin();
			if (redraw == 1u) {
				draw_directory_row(&box, old_selected, focus,
					selected_entry, count);
				draw_directory_row(&box, selected_entry, focus,
					selected_entry, count);
			} else if (redraw == 3u) {
				draw_file_input(&box);
			} else draw_file_dialog(&box, focus, selected_entry, count,
					      saving);
			(void)tui_cursor_hide();
			(void)tui_end();
		}
	}
	(void)tui_restore(&box, dialog_saved, sizeof(dialog_saved));
	return 0;
}

static uint8_t action_enabled(uint8_t action)
{
	ke_pos_t first;
	ke_pos_t last;

	selection(&first, &last);
	if (action == ACT_UNDO) return history.cursor != 0u;
	if (action == ACT_REDO) return history.cursor < history.count;
	if (action == ACT_COPY || action == ACT_CUT || action == ACT_DELETE)
		return first != last;
	if (action == ACT_PASTE) return history.clipboard_length != 0ul;
	if (action == ACT_FIND_NEXT || action == ACT_REPLACE)
		return find_text[0] != '\0';
	return 1u;
}

static void popup_items(uint8_t menu, const struct ke_action_item **items,
			uint8_t *count)
{
	if (menu == 0u) { *items = file_menu; *count = 5u; }
	else if (menu == 1u) { *items = edit_menu; *count = 7u; }
	else if (menu == 2u) { *items = search_menu; *count = 4u; }
	else { *items = help_menu; *count = 2u; }
}

static void draw_popup_row(const struct tui_rect *box,
			   const struct ke_action_item *item, uint8_t row,
			   uint8_t selected_row)
{
	struct tui_rect line;
	tui_style_t style = row == selected_row ? TUI_STYLE_SELECTED :
			    (action_enabled(item->action) ? TUI_STYLE_NORMAL :
			     TUI_STYLE_DISABLED);

	line.x = (uint8_t)(box->x + 1u);
	line.y = (uint8_t)(box->y + 1u + row);
	line.w = (uint8_t)(box->w - 2u);
	line.h = 1u;
	(void)tui_fill(&line, ' ', style);
	(void)tui_set_style(style);
	(void)tui_puts_xy(line.x, line.y, item->label);
}

static uint8_t run_popup(uint8_t menu)
{
	const struct ke_action_item *items;
	struct tui_rect box;
	uint8_t count;
	uint8_t selected_row = 0u;
	uint16_t need;

	popup_items(menu, &items, &count);
	while (selected_row < count && !action_enabled(items[selected_row].action))
		++selected_row;
	box.x = (uint8_t)(menu * 8u);
	box.y = 1u;
	box.w = KE_POPUP_WIDTH;
	box.h = (uint8_t)(count + 2u);
	need = tui_saved_size(&box);
	if (need == 0u || need > sizeof(popup_saved) ||
	    tui_save(&box, popup_saved, sizeof(popup_saved)) < 0) return ACT_NONE;
	tui_begin();
	(void)tui_fill(&box, ' ', TUI_STYLE_NORMAL);
	(void)tui_frame(&box, TUI_FRAME_SINGLE, TUI_STYLE_NORMAL);
	{
		uint8_t row;
		for (row = 0u; row < count; ++row)
			draw_popup_row(&box, &items[row], row, selected_row);
	}
	(void)tui_end();
	for (;;) {
		tui_key_t key = tui_getkey();
		uint8_t old = selected_row;

		if (key == TUI_KEY_ESC) { selected_row = ACT_NONE; break; }
		if (key == TUI_KEY_LEFT) { selected_row = ACT_MENU_LEFT; break; }
		if (key == TUI_KEY_RIGHT) { selected_row = ACT_MENU_RIGHT; break; }
		if (key == TUI_KEY_UP || key == TUI_KEY_DOWN) {
			do {
				if (key == TUI_KEY_UP)
					selected_row = selected_row == 0u ?
						(uint8_t)(count - 1u) :
						(uint8_t)(selected_row - 1u);
				else selected_row = (uint8_t)((selected_row + 1u) % count);
			} while (!action_enabled(items[selected_row].action));
			tui_begin();
			draw_popup_row(&box, &items[old], old, selected_row);
			draw_popup_row(&box, &items[selected_row], selected_row,
				       selected_row);
			(void)tui_end();
		} else if ((key == 13u || key == 10u) &&
			   action_enabled(items[selected_row].action)) {
			uint8_t action = items[selected_row].action;
			(void)tui_restore(&box, popup_saved, sizeof(popup_saved));
			return action;
		}
	}
	(void)tui_restore(&box, popup_saved, sizeof(popup_saved));
	return selected_row;
}

static uint8_t run_menu(void)
{
	uint8_t menu = 0u;

	for (;;) {
		tui_id_t selected_id = menu_bar[menu].id;
		tui_key_t key;

		tui_begin();
		(void)ke_menubar(menu_bar, 4u, selected_id);
		(void)tui_cursor_hide();
		(void)tui_end();
		key = tui_getkey();
		if (key == TUI_KEY_ESC || key == TUI_KEY_F1) break;
		if (key == TUI_KEY_LEFT) menu = menu == 0u ? 3u : (uint8_t)(menu - 1u);
		else if (key == TUI_KEY_RIGHT) menu = (uint8_t)((menu + 1u) % 4u);
		else if (key == TUI_KEY_DOWN || key == 13u || key == 10u) {
			uint8_t action = run_popup(menu);
			if (action == ACT_MENU_LEFT) menu = menu == 0u ? 3u :
							 (uint8_t)(menu - 1u);
			else if (action == ACT_MENU_RIGHT) menu = (uint8_t)((menu + 1u) % 4u);
			else if (action != ACT_NONE) return action;
		}
	}
	{
		tui_id_t none = TUI_ID_NONE;

		tui_begin();
		(void)ke_menubar(menu_bar, 4u, none);
		show_cursor(follow_cursor());
		(void)tui_end();
	}
	return ACT_NONE;
}

static int save_current(uint8_t force_dialog)
{
	char chosen[KE_PATH_MAX];
	struct ke_file_info existing;
	const char *target = current_path;
	const char *failure;

	if (force_dialog != 0u || current_path[0] == '\0') {
		if (!file_dialog(1u, chosen)) return 0;
		if (ke_fs_stat(chosen, &existing) >= 0 &&
		    confirm_box("Replace", "Replace the existing file?") !=
			TUI_ID_YES) return 0;
		target = chosen;
	}
	if (ke_file_save(&document, target, recovery_path,
			 sizeof(recovery_path)) < 0) {
		failure = recovery_path[0] != '\0' ?
			  (const char *)recovery_path : "The file was not saved.";
		(void)message_box("Save failed",
			failure, TUI_BUTTON(TUI_ID_OK));
		return 0;
	}
	if (target == (const char *)chosen)
		set_text(current_path, chosen, sizeof(current_path));
	modified = 0u;
	if (recovery_path[0] != '\0')
		(void)message_box("Save warning", recovery_path,
			TUI_BUTTON(TUI_ID_OK));
	return 1;
}

static int confirm_discard(void)
{
	tui_id_t answer;

	if (modified == 0u) return 1;
	answer = message_box("Modified document",
		"YES saves, NO discards, \nCANCEL returns to the editor.",
		TUI_BUTTON(TUI_ID_YES) | TUI_BUTTON(TUI_ID_NO) |
		TUI_BUTTON(TUI_ID_CANCEL));
	if (answer == TUI_ID_YES) return save_current(0u);
	return answer == TUI_ID_NO;
}

static void reset_position(void)
{
	cursor_position = 0ul;
	ke_view.row_start[0] = 0ul;
	ke_view.top_line = 0ul;
	ke_view.left_column = 0ul;
	mark_active = 0u;
	have_match = 0u;
}

static int replace_document(const char *path)
{
	struct ke_store fresh;
	int result;

	ke_history_dispose(&history);
	result = ke_store_init(&fresh);
	if (result == KE_STORE_OK) result = ke_file_load(&fresh, path);
	if (result != KE_FILE_OK) {
		ke_store_dispose(&fresh);
		if (ke_history_init(&history) != KE_HISTORY_OK) return -1;
		(void)message_box("Open failed", "The document was not replaced.",
				      TUI_BUTTON(TUI_ID_OK));
		return 0;
	}
	ke_store_dispose(&document);
	document = fresh;
	if (ke_history_init(&history) != KE_HISTORY_OK) return -1;
	set_text(current_path, path, sizeof(current_path));
	modified = 0u;
	reset_position();
	return 1;
}

static void erase_selection(uint8_t copy_first)
{
	ke_pos_t first;
	ke_pos_t last;
	int result;

	selection(&first, &last);
	if (first == last) return;
	if (copy_first != 0u) result = ke_history_cut(&history, &document, first,
						 last - first);
	else result = ke_history_edit(&history, &document, first, last - first,
				      (const uint8_t *)0, 0u);
	if (result >= 0) {
		cursor_position = first;
		mark_active = 0u;
		modified = 1u;
		have_match = 0u;
	}
}

static int insert_byte(uint8_t byte)
{
	ke_pos_t first;
	ke_pos_t last;
	ke_pos_t remove = 0ul;

	selection(&first, &last);
	if (first != last) {
		remove = last - first;
		cursor_position = first;
		mark_active = 0u;
	} else if (insert_mode == 0u && cursor_position < ke_store_length(&document)) {
		uint8_t old;
		if (ke_store_byte(&document, cursor_position, &old) == KE_STORE_OK &&
		    old != 10u) remove = 1ul;
	}
	if ((first == last ?
	     ke_history_edit_grouped(&history, &document, cursor_position,
		remove, &byte, 1u, KE_GROUP_TYPING) :
	     ke_history_edit(&history, &document, cursor_position, remove, &byte,
			     1u)) == KE_HISTORY_OK) {
		++cursor_position;
		modified = 1u;
		have_match = 0u;
		return 1;
	}
	return 0;
}

static void do_find(uint8_t ask)
{
	ke_pos_t found;
	ke_pos_t start;

	if (ask != 0u) {
		if (!prompt_text("Find", find_text, sizeof(find_text))) return;
		match_case = (uint8_t)(confirm_box("Find", "Match case?") ==
					 TUI_ID_YES);
	}
	if (find_text[0] == '\0') return;
	start = have_match != 0u ? match_position + 1ul : cursor_position;
	if (!ke_find(&document, start, (const uint8_t *)find_text,
		     strlen(find_text), match_case, &found)) {
		if (start != 0ul && confirm_box("Find", "End reached. Wrap search?") ==
			TUI_ID_YES &&
		    !ke_find(&document, 0ul, (const uint8_t *)find_text,
			     strlen(find_text), match_case, &found)) {
			(void)message_box("Find", "Text not found.",
					      TUI_BUTTON(TUI_ID_OK));
			return;
		} else if (start == 0ul) {
			(void)message_box("Find", "Text not found.",
					      TUI_BUTTON(TUI_ID_OK));
			return;
		}
	}
	match_position = found;
	match_length = strlen(find_text);
	have_match = 1u;
	mark_anchor = found;
	cursor_position = found + match_length;
	mark_active = 1u;
}

static uint8_t perform_action(uint8_t action)
{
	char chosen[KE_PATH_MAX];
	ke_pos_t first;
	ke_pos_t last;

	if (action == ACT_NEW) {
		if (confirm_discard()) {
			ke_history_clear(&history);
			(void)ke_store_delete(&document, 0ul,
					      ke_store_length(&document));
			current_path[0] = '\0'; modified = 0u; reset_position();
		}
	} else if (action == ACT_OPEN) {
		if (confirm_discard() && file_dialog(0u, chosen))
			if (replace_document(chosen) < 0) return 1u;
	} else if (action == ACT_SAVE) (void)save_current(0u);
	else if (action == ACT_SAVE_AS) (void)save_current(1u);
	else if (action == ACT_EXIT) return confirm_discard() ? 1u : 0u;
	else if (action == ACT_UNDO && ke_history_undo(&history, &document) == 0) {
		modified = 1u; if (cursor_position > ke_store_length(&document))
			cursor_position = ke_store_length(&document);
	} else if (action == ACT_REDO && ke_history_redo(&history, &document) == 0) {
		modified = 1u; if (cursor_position > ke_store_length(&document))
			cursor_position = ke_store_length(&document);
	} else if (action == ACT_COPY) {
		selection(&first, &last);
		(void)ke_history_copy(&history, &document, first, last - first);
	} else if (action == ACT_CUT) erase_selection(1u);
	else if (action == ACT_DELETE) erase_selection(0u);
	else if (action == ACT_PASTE) {
		erase_selection(0u);
		if (ke_history_paste(&history, &document, cursor_position) == 0) {
			cursor_position += history.clipboard_length;
			modified = 1u;
		}
	} else if (action == ACT_SELECT_ALL) {
		mark_anchor = 0ul; cursor_position = ke_store_length(&document);
		mark_active = 1u;
	} else if (action == ACT_FIND) do_find(1u);
	else if (action == ACT_FIND_NEXT) do_find(0u);
	else if (action == ACT_REPLACE) {
		do_find(0u);
		if (have_match != 0u && prompt_text("Replace with", replace_text,
						       sizeof(replace_text))) {
			selection(&first, &last);
			if (ke_history_edit(&history, &document, first, last - first,
					    (const uint8_t *)replace_text,
					    strlen(replace_text)) == 0) {
				cursor_position = first + strlen(replace_text);
				mark_active = 0u; modified = 1u; have_match = 0u;
			}
		}
	} else if (action == ACT_GOTO) {
		char number[12] = "1";
		if (prompt_text("Go To Line", number, sizeof(number))) {
			uint32_t wanted = 0ul;
			uint16_t i = 0u;
			while (number[i] >= '0' && number[i] <= '9')
				wanted = (wanted << 3) + (wanted << 1) +
					 (uint32_t)(number[i++] - '0');
			cursor_position = 0ul;
			while (wanted > 1ul && cursor_position < ke_store_length(&document)) {
				cursor_position = next_line(cursor_position); --wanted;
			}
		}
	} else if (action == ACT_KEYS) {
		(void)message_box("Keys",
			"F1 menus  F2 Save  F3 Open  F4 Find  F5 Next  F7 Mark  F8 Copy  F9 Paste  F10 Exit",
			TUI_BUTTON(TUI_ID_OK));
	} else if (action == ACT_ABOUT) {
		(void)message_box("KonaEdit",
			"CP850 editor. LF files, literal TABs, 64 KiB XMEM document.",
			TUI_BUTTON(TUI_ID_OK));
	}
	return 0u;
}

static void move_vertical(int direction)
{
	uint32_t column = ke_visual_column(&document, cursor_position);
	ke_pos_t target;

	if (direction < 0) target = previous_line(cursor_position);
	else target = next_line(cursor_position);
	cursor_position = ke_position_for_column(&document, target, column);
}

/* Page Up/Down: the cursor and the view move by the same number of lines,
 * so the view's line count stays exact without a recount. */
static void move_page(int direction)
{
	uint8_t moved = 0u;
	uint32_t column = ke_visual_column(&document, cursor_position);
	ke_pos_t line = ke_line_start(&document, cursor_position);
	ke_pos_t top = ke_view.row_start[0];

	while (moved < ke_view.rows) {
		ke_pos_t target;

		if (direction > 0) target = ke_view_next_start(line);
		else target = line == 0ul ? KE_POS_NONE : previous_line(line);
		if (target == KE_POS_NONE) break;
		line = target;
		++moved;
		/* The top follows while it can. */
		if (direction > 0) {
			if (ke_view.row_start[1] == KE_POS_NONE) continue;
			top = ke_view.row_start[1];
			ke_view.row_start[1] = ke_view_next_start(top);
			++ke_view.top_line;
		} else if (top != 0ul) {
			top = ke_line_start(&document, top - 1ul);
			--ke_view.top_line;
		}
		ke_view.row_start[0] = top;
	}
	cursor_position = ke_position_for_column(&document, line, column);
	if (moved == 0u) return;
	set_view_selection();
	ke_view_rebuild(top, ke_view.top_line);
}

int ke_ui_run(const char *initial_path)
{
	int result;

	insert_mode = 1u;
	current_path[0] = '\0';
	find_text[0] = '\0';
	replace_text[0] = '\0';
	if (ke_history_init(&history) != KE_HISTORY_OK) return 1;
	result = ke_store_init(&document);
	if (result != KE_STORE_OK) {
		ke_history_dispose(&history);
		return 1;
	}
	if (tui_init() < 0) {
		ke_history_dispose(&history);
		ke_store_dispose(&document);
		return 1;
	}
	if (tui_get_size(&screen) < 0) {
		tui_shutdown();
		ke_history_dispose(&history);
		ke_store_dispose(&document);
		return 1;
	}
	if (screen.cols < KE_MIN_COLS || screen.rows < KE_MIN_ROWS) {
		/* tui_init cleared the screen. */
		tui_begin();
		(void)ke_label(0u, 0u, "KonaEdit: console grid is too small.");
		(void)tui_end();
		tui_shutdown();
		ke_history_dispose(&history);
		ke_store_dispose(&document);
		return 1;
	}
	ke_view_setup(&document, 1u, (uint8_t)(screen.rows - 2u), screen.cols);
	(void)tui_set_style(TUI_STYLE_STATUS);
	attr_status = tui_get_attr();
	(void)tui_set_style(TUI_STYLE_NORMAL);
	reset_position();
	if (initial_path != (const char *)0 && initial_path[0] != '\0') {
		if (ke_file_load(&document, initial_path) == KE_FILE_OK)
			set_text(current_path, initial_path, sizeof(current_path));
		else {
			(void)ke_store_delete(&document, 0ul,
					      ke_store_length(&document));
			(void)message_box("Open failed",
				"Starting with an empty document.",
				TUI_BUTTON(TUI_ID_OK));
		}
	}
	/* tui_init cleared the screen; render_all writes every cell. */
	render_all();
	for (;;) {
		tui_key_t key = tui_getkey();
		uint8_t row = ke_view_row_of(cursor_position);
		ke_pos_t old_cursor = cursor_position;
		ke_pos_t old_first;
		ke_pos_t old_last;
		uint8_t action = ACT_NONE;
		uint8_t view_edit = VIEW_NONE;
		int8_t delta = 0;
		ke_pos_t at = cursor_position;

		selection(&old_first, &old_last);
		if (!((key >= 32u && key <= 255u) || key == 8u || key == 127u ||
		      key == TUI_KEY_DELETE || key == 9u || key == 10u ||
		      key == 13u))
			ke_history_break_group(&history);

		if (key >= 0x100u) {
			/* The named keys share the high byte: one switch on
			 * the low one. */
			switch ((uint8_t)key) {
			case (uint8_t)TUI_KEY_F1: action = run_menu(); break;
			case (uint8_t)TUI_KEY_F2: action = ACT_SAVE; break;
			case (uint8_t)TUI_KEY_F3: action = ACT_OPEN; break;
			case (uint8_t)TUI_KEY_F4: action = ACT_FIND; break;
			case (uint8_t)TUI_KEY_F5: action = ACT_FIND_NEXT; break;
			case (uint8_t)TUI_KEY_F7:
				if (mark_active != 0u) mark_active = 0u;
				else { mark_active = 1u; mark_anchor = cursor_position; }
				break;
			case (uint8_t)TUI_KEY_F8: action = ACT_COPY; break;
			case (uint8_t)TUI_KEY_F9: action = ACT_PASTE; break;
			case (uint8_t)TUI_KEY_F10: action = ACT_EXIT; break;
			case (uint8_t)TUI_KEY_LEFT:
				if (cursor_position != 0ul) --cursor_position;
				break;
			case (uint8_t)TUI_KEY_RIGHT:
				if (cursor_position < ke_store_length(&document))
					++cursor_position;
				break;
			case (uint8_t)TUI_KEY_UP: move_vertical(-1); break;
			case (uint8_t)TUI_KEY_DOWN: move_vertical(1); break;
			case (uint8_t)TUI_KEY_HOME:
				cursor_position = ke_line_start(&document,
								cursor_position);
				break;
			case (uint8_t)TUI_KEY_END:
				cursor_position = ke_line_end(&document,
							      cursor_position);
				break;
			case (uint8_t)TUI_KEY_PGUP: move_page(-1); break;
			case (uint8_t)TUI_KEY_PGDN: move_page(1); break;
			case (uint8_t)TUI_KEY_INSERT:
				insert_mode = (uint8_t)!insert_mode;
				break;
			case (uint8_t)TUI_KEY_DELETE:
				if (old_first != old_last) {
					erase_selection(0u);
					view_edit = VIEW_REBUILD;
				} else if (cursor_position < ke_store_length(&document)) {
					uint8_t removed = 0u;
					(void)ke_store_byte(&document, cursor_position,
							    &removed);
					if (ke_history_edit_grouped(&history, &document,
						cursor_position, 1ul, (const uint8_t *)0,
						0u, KE_GROUP_DELETE) == 0) {
						modified = 1u;
						view_edit = removed == 10u ? VIEW_JOIN :
							    VIEW_INLINE;
						delta = -1;
					}
				}
				break;
			default:
				break;
			}
		} else if (key == TUI_KEY_ESC) {
			mark_active = 0u;
		} else if ((key == 8u || key == 127u) && cursor_position != 0ul) {
			ke_pos_t changed = cursor_position - 1ul;
			uint8_t removed = 0u;
			(void)ke_store_byte(&document, changed, &removed);
			if (ke_history_edit_grouped(&history, &document, changed, 1ul,
				    (const uint8_t *)0, 0u,
				    KE_GROUP_BACKSPACE) == 0) {
				cursor_position = changed;
				modified = 1u;
				if (removed != 10u) {
					view_edit = VIEW_INLINE;
					delta = -1;
				} else if (row != 0u && row != KE_VIEW_NONE) {
					view_edit = VIEW_JOIN;
					--row;
				} else {
					view_edit = VIEW_REBUILD;
				}
			}
		} else if (key == 13u || key == 10u || key == 9u || key >= 32u) {
			uint8_t byte = key == 13u ? 10u : (uint8_t)key;
			ke_pos_t length = ke_store_length(&document);

			if (insert_byte(byte)) {
				if (old_first != old_last) view_edit = VIEW_REBUILD;
				else if (byte == 10u) {
					view_edit = VIEW_NEWLINE;
					if (ke_store_length(&document) == length)
						view_edit = VIEW_REBUILD;
				} else {
					view_edit = VIEW_INLINE;
					delta = (int8_t)(ke_store_length(&document) -
							 length);
				}
			}
		}
		if (action != ACT_NONE) {
			if (perform_action(action) != 0u) break;
			render_all();
			continue;
		}
		tui_begin();
		set_view_selection();
		if (row == KE_VIEW_NONE && view_edit != VIEW_NONE)
			view_edit = VIEW_REBUILD;
		if (view_edit == VIEW_INLINE) ke_view_edit_inline(row, delta);
		else if (view_edit == VIEW_NEWLINE) ke_view_newline(row, at);
		else if (view_edit == VIEW_JOIN) ke_view_join(row);
		else if (view_edit == VIEW_REBUILD) rebuild_view();
		row = follow_cursor();
		if (old_first != old_last && mark_active == 0u) {
			/* The selection is gone: every row may have shown it. */
			ke_view_draw_rows(0u, (uint8_t)(ke_view.rows - 1u));
		} else if (mark_active != 0u) {
			/* The anchor stays: only the rows the cursor crossed,
			 * found again in the view as it is now. */
			uint8_t old_row = ke_view_row_of(old_cursor);

			if (old_row == KE_VIEW_NONE)
				old_row = old_cursor < ke_view.row_start[0] ? 0u :
					  (uint8_t)(ke_view.rows - 1u);
			ke_view_draw_rows(old_row < row ? old_row : row,
					  old_row < row ? row : old_row);
		}
		show_cursor(row);
		(void)tui_end();
	}
	ke_history_dispose(&history);
	ke_store_dispose(&document);
	tui_shutdown();
	return 0;
}
