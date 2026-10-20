#include "kos.h"
#include "tui.h"
#include "tui_widgets.h"

#include "kc_protocol.h"
#include "kc_remote.h"
#include "kc_serial.h"
#include "kc_ui.h"

#define SCREEN_COLUMNS 85u
#define PANEL_COLUMNS  42u
#define PANEL_ROWS     20u
#define VIEW_BUFFER    2048u
#define VIEW_CHUNK     700u
#define COPY_CHUNK     700u
#define VIEW_HISTORY   16u
#define HISTORY_DEPTH  12u
#define LOCAL_PATH_MAX 126u
#define MENU_POPUP_WIDTH 20u
#define MENU_POPUP_MAX_HEIGHT 6u

#define PANEL_LOCAL     0u
#define PANEL_REMOTE    1u
#define ENTRY_SIZE_UNKNOWN 0x80u


enum ui_focus {
	FOCUS_MENU,
	FOCUS_LEFT,
	FOCUS_RIGHT,
	FOCUS_HELP,
	FOCUS_VIEW,
	FOCUS_HEX,
	FOCUS_COPY,
	FOCUS_REFRESH,
	FOCUS_QUIT,
	FOCUS_COUNT
};

enum menu_id {
	MENU_FILE = 10u,
	MENU_EDIT,
	MENU_VIEW,
	MENU_HELP
};

enum action_id {
	ACTION_NONE,
	ACTION_OPEN,
	ACTION_COPY,
	ACTION_REFRESH,
	ACTION_TEXT_VIEW,
	ACTION_HEX_VIEW,
	ACTION_HELP,
	ACTION_QUIT
};

struct action_item {
	const char *label;
	unsigned char action;
};

static const struct tui_menu_item main_menu[] = {
	{"File", 'f', MENU_FILE},
	{"Edit", 'e', MENU_EDIT},
	{"View", 'v', MENU_VIEW},
	{"Help", 'h', MENU_HELP}
};

static const struct action_item file_actions[] = {
	{"Open", ACTION_OPEN},
	{"Copy", ACTION_COPY},
	{"Refresh", ACTION_REFRESH},
	{"Quit", ACTION_QUIT}
};

static const struct action_item edit_actions[] = {
	{"Copy", ACTION_COPY}
};

static const struct action_item view_actions[] = {
	{"View text", ACTION_TEXT_VIEW},
	{"View hex", ACTION_HEX_VIEW},
	{"Refresh", ACTION_REFRESH}
};

static const struct action_item help_actions[] = {
	{"Help", ACTION_HELP}
};

struct panel {
	unsigned char source;
	char path[KC_MAX_PATH + 1u];
	struct kc_page page;
	unsigned char selected;
	unsigned char history_depth;
	unsigned long history[HISTORY_DEPTH];
};

static struct panel panels[2];
static unsigned char active_panel;
static unsigned char ui_focus;
static unsigned char remote_online;
static unsigned char remote_capabilities;
static tui_id_t menu_selected;
static char server_name[64];
static char status_text[SCREEN_COLUMNS + 1u];
static unsigned char view_data[VIEW_BUFFER];
static char copy_source[KC_MAX_PATH + 1u];
static char copy_destination[KC_MAX_PATH + 1u];
static char copy_temporary[KC_MAX_PATH + 1u];
static struct kos_ns_entry local_entries[PANEL_ROWS];
static struct kos_ns_entry local_probe;
static struct tui_cell menu_background[MENU_POPUP_WIDTH *
				       MENU_POPUP_MAX_HEIGHT];

static unsigned int bounded_length(const char *text, unsigned int limit)
{
	unsigned int length = 0u;

	while (length < limit && text[length] != '\0') {
		++length;
	}
	return length;
}

static void copy_text(char *target, const char *source, unsigned int capacity)
{
	unsigned int length;

	if (capacity == 0u) {
		return;
	}
	length = bounded_length(source, (unsigned int)(capacity - 1u));
	memcpy(target, source, length);
	target[length] = '\0';
}

static void set_status(const char *text)
{
	copy_text(status_text, text, sizeof(status_text));
}

static void set_error(int error)
{
	set_status(kc_remote_error(error));
	if (error <= KC_LINK_EDEVICE && error >= KC_LINK_ECOBS) {
		remote_online = 0u;
		copy_text(server_name, "PC offline - R reconnect", sizeof(server_name));
	}
}

static void set_local_error(int error)
{
	if (error == -KOS_ENODEV) set_status("KonaOS /fs: no card or filesystem");
	else if (error == -KOS_ENOENT) set_status("KonaOS path not found");
	else if (error == -KOS_ENOTDIR) set_status("KonaOS path is not a directory");
	else if (error == -KOS_EIO) set_status("KonaOS filesystem I/O error");
	else if (error == -KOS_ENOSPC) set_status("KonaOS filesystem is full");
	else if (error == -KOS_EROFS) set_status("KonaOS filesystem is read-only");
	else if (error == -KOS_EMFILE) set_status("KonaOS file table is full");
	else set_status("KonaOS filesystem operation failed");
}

static void out_char(char value)
{
	(void)tui_putc(value);
}

static void out_bytes(const char *text, unsigned int length)
{
	(void)tui_write(text, length);
}

static void out_text(const char *text)
{
	(void)tui_puts(text);
}

static void out_repeat(char value, unsigned int count)
{
	while (count-- != 0u) {
		out_char(value);
	}
}

static void out_fixed(const char *text, unsigned int width)
{
	unsigned int length = bounded_length(text, width);

	out_bytes(text, length);
	out_repeat(' ', (unsigned int)(width - length));
}

static void line_end(unsigned char last)
{
	if (last == 0u) {
		out_char('\r');
		out_char('\n');
	}
}

static void screen_start(void)
{
	tui_begin();
	(void)tui_reset_window();
	(void)tui_cursor_hide();
	(void)tui_set_style(TUI_STYLE_NORMAL);
	(void)tui_gotoxy(0u, 0u);
}

static void out_cursor(unsigned char row, unsigned char column)
{
	if (row != 0u && column != 0u) {
		(void)tui_gotoxy((uint8_t)(column - 1u), (uint8_t)(row - 1u));
	}
}

static void screen_finish(void)
{
	(void)tui_end();
}

static void render_status(void)
{
	tui_begin();
	out_cursor(24u, 1u);
	(void)tui_set_style(TUI_STYLE_STATUS);
	out_fixed(status_text, SCREEN_COLUMNS);
	(void)tui_set_style(TUI_STYLE_NORMAL);
	screen_finish();
}

static void decimal(unsigned long value, char *target, unsigned int width)
{
	char digits[10];
	unsigned int count = 0u;
	unsigned int start;
	unsigned int i;

	do {
		digits[count++] = (char)('0' + value % 10ul);
		value /= 10ul;
	} while (value != 0ul && count < sizeof(digits));
	memset(target, ' ', width);
	start = count < width ? (unsigned int)(width - count) : 0u;
	for (i = 0u; i < count && start + i < width; ++i) {
		target[start + i] = digits[count - i - 1u];
	}
}

static char hex_digit(unsigned char value)
{
	return (char)(value < 10u ? '0' + value : 'A' + value - 10u);
}

static void out_hex32(unsigned long value)
{
	unsigned char shift = 32u;

	while (shift != 0u) {
		shift = (unsigned char)(shift - 4u);
		out_char(hex_digit((unsigned char)((value >> shift) & 15ul)));
	}
}

static void make_cell(const struct panel *panel, unsigned char row,
		      char *cell)
{
	const struct kc_entry *entry;
	unsigned int length;
	unsigned int i;

	memset(cell, ' ', PANEL_COLUMNS);
	if (row >= panel->page.count) {
		return;
	}
	entry = &panel->page.entries[row];
	cell[0] = ' ';
	cell[1] = (entry->flags & KC_ENTRY_DIRECTORY) != 0u ? '/' : ' ';
	length = bounded_length(entry->name, 28u);
	for (i = 0u; i < length; ++i) {
		cell[2u + i] = entry->name[i];
	}
	if ((entry->flags & KC_ENTRY_DIRECTORY) != 0u) {
		memcpy(cell + 34u, "<DIR>", 5u);
	} else if ((entry->flags & ENTRY_SIZE_UNKNOWN) == 0u) {
		decimal(entry->size, cell + 32u, 10u);
	}
}

static void render_panel_row(const struct panel *panel, unsigned char row)
{
	char cell[PANEL_COLUMNS];

	make_cell(panel, row, cell);
	if (row == panel->selected && row < panel->page.count) {
		out_char('>');
		out_bytes(cell + 1u, PANEL_COLUMNS - 1u);
		return;
	}
	out_bytes(cell, PANEL_COLUMNS);
}

static void render_selection_marker(unsigned char pane, unsigned char row,
				    unsigned char visible)
{
	uint8_t x = pane == 0u ? 0u : 43u;
	uint8_t y = (uint8_t)(2u + row);

	(void)tui_set_style(TUI_STYLE_NORMAL);
	(void)tui_putc_xy(x, y, visible != 0u ? '>' : ' ');
}

static void render_pane_path(unsigned char pane)
{
	uint8_t x = pane == 0u ? 0u : 43u;
	uint8_t focused = (uint8_t)((pane == 0u && ui_focus == FOCUS_LEFT) ||
				    (pane == 1u && ui_focus == FOCUS_RIGHT));

	(void)tui_gotoxy(x, 1u);
	(void)tui_set_style(focused != 0u ? TUI_STYLE_SELECTED :
			    TUI_STYLE_NORMAL);
	out_char(pane == 0u ? 'L' : 'R');
	(void)tui_set_style(TUI_STYLE_NORMAL);
	out_char(':');
	out_fixed(panels[pane].path, PANEL_COLUMNS - 2u);
}

static void make_detail(const struct panel *panel, char *detail)
{
	memset(detail, 0, SCREEN_COLUMNS + 1u);
	if (panel->page.count != 0u) {
		const struct kc_entry *entry =
			&panel->page.entries[panel->selected];
		unsigned int length = bounded_length(entry->name, 63u);

		/* The list already shows ordinary names and sizes.  Reserve the
		 * detail row for names that the 28-column list field truncates so
		 * common Up/Down movement changes only the two selection marks. */
		if (length <= 28u) return;
		memcpy(detail, entry->name, length);
		if ((entry->flags & (KC_ENTRY_DIRECTORY | ENTRY_SIZE_UNKNOWN)) == 0u &&
		    length < 72u) {
			detail[length++] = ' ';
			decimal(entry->size, detail + length, 10u);
			detail[length + 10u] = '\0';
		}
	}
}

static void render_menu_bar(void)
{
	tui_id_t shown_menu =
		ui_focus == FOCUS_MENU ? menu_selected : TUI_ID_NONE;

	(void)tui_menubar(main_menu,
		(uint8_t)(sizeof(main_menu) / sizeof(main_menu[0])),
		&shown_menu);
}

static void render_detail_row(void)
{
	char detail[SCREEN_COLUMNS + 1u];

	make_detail(&panels[active_panel], detail);
	(void)tui_gotoxy(0u, 22u);
	(void)tui_set_style(TUI_STYLE_NORMAL);
	out_fixed(detail, SCREEN_COLUMNS);
}

static void render_status_row(void)
{
	(void)tui_gotoxy(0u, 23u);
	(void)tui_set_style(TUI_STYLE_STATUS);
	out_fixed(status_text, SCREEN_COLUMNS);
	(void)tui_set_style(TUI_STYLE_NORMAL);
}

static void render_button_focus(unsigned char focus)
{
	switch (focus) {
	case FOCUS_HELP:
		(void)tui_button(0u, 24u, "Help", 20u,
				 ui_focus == FOCUS_HELP);
		break;
	case FOCUS_VIEW:
		(void)tui_button(10u, 24u, "View", 21u,
				 ui_focus == FOCUS_VIEW);
		break;
	case FOCUS_HEX:
		(void)tui_button(20u, 24u, "Hex", 22u,
				 ui_focus == FOCUS_HEX);
		break;
	case FOCUS_COPY:
		(void)tui_button(29u, 24u, "Copy", 23u,
				 ui_focus == FOCUS_COPY);
		break;
	case FOCUS_REFRESH:
		(void)tui_button(39u, 24u, "Refresh", 24u,
				 ui_focus == FOCUS_REFRESH);
		break;
	case FOCUS_QUIT:
		(void)tui_button(53u, 24u, "Quit", 25u,
				 ui_focus == FOCUS_QUIT);
		break;
	default:
		break;
	}
}

static void render_buttons(void)
{
	struct tui_rect button_row = {0u, 24u, SCREEN_COLUMNS, 1u};
	unsigned char focus;

	(void)tui_fill(&button_row, ' ', TUI_STYLE_NORMAL);
	for (focus = FOCUS_HELP; focus <= FOCUS_QUIT; ++focus) {
		render_button_focus(focus);
	}
	(void)tui_label(63u, 24u, "F1 Menu  Tab Next");
}

static void render_focus_change(unsigned char old_focus,
				unsigned char old_active,
				unsigned char status_changed)
{
	unsigned char pane;

	tui_begin();
	if (old_focus == FOCUS_MENU || ui_focus == FOCUS_MENU) {
		render_menu_bar();
	}
	if ((old_focus >= FOCUS_LEFT && old_focus <= FOCUS_RIGHT) ||
	    (ui_focus >= FOCUS_LEFT && ui_focus <= FOCUS_RIGHT)) {
		for (pane = 0u; pane < 2u; ++pane) {
			render_pane_path(pane);
			if (panels[pane].page.count != 0u) {
				render_selection_marker(pane,
					panels[pane].selected, 1u);
			}
		}
	}
	if (old_focus >= FOCUS_HELP) render_button_focus(old_focus);
	if (ui_focus >= FOCUS_HELP && ui_focus != old_focus)
		render_button_focus(ui_focus);
	if (active_panel != old_active) render_detail_row();
	if (status_changed != 0u) render_status_row();
	(void)tui_end();
}

static void render_navigation(unsigned char old_selected)
{
	struct panel *panel = &panels[active_panel];

	tui_begin();
	if (old_selected < panel->page.count && old_selected != panel->selected) {
		render_selection_marker(active_panel, old_selected, 0u);
	}
	if (panel->selected < panel->page.count) {
		render_selection_marker(active_panel, panel->selected, 1u);
	}
	render_detail_row();
	screen_finish();
}

static void render_commander(void)
{
	unsigned char row;

	screen_start();
	render_menu_bar();
	render_pane_path(0u);
	(void)tui_gotoxy(42u, 1u);
	out_char('|');
	render_pane_path(1u);
	line_end(0u);
	for (row = 0u; row < PANEL_ROWS; ++row) {
		render_panel_row(&panels[0], row);
		out_char('|');
		render_panel_row(&panels[1], row);
		line_end(0u);
	}
	render_detail_row();
	render_status_row();
	render_buttons();
	(void)tui_gotoxy(0u, 25u);
	out_fixed(server_name, SCREEN_COLUMNS);
	screen_finish();
}

static int read_key(void)
{
	return (int)tui_getkey();
}

static int local_list(const char *path, unsigned long cursor,
		      struct kc_page *page)
{
	unsigned int cookie = (unsigned int)cursor;
	unsigned int next;
	int count;
	int probe;
	int index;

	if (cursor > 0xfffful) return -KOS_EINVAL;
	count = list(path, &cookie, local_entries, PANEL_ROWS);
	if (count < 0) return count;
	page->cursor = cursor;
	page->count = (unsigned char)count;
	for (index = 0; index < count; ++index) {
		struct kc_entry *target = &page->entries[index];

		target->flags = ENTRY_SIZE_UNKNOWN;
		if ((local_entries[index].flags & KOS_NS_ENTRY_CONTAINER) != 0u)
			target->flags |= KC_ENTRY_DIRECTORY;
		target->size = 0ul;
		copy_text(target->name, local_entries[index].name,
			  sizeof(target->name));
	}
	page->next_cursor = 0ul;
	if (count == PANEL_ROWS) {
		next = cookie;
		probe = list(path, &cookie, &local_probe, 1u);
		if (probe < 0) return probe;
		if (probe != 0) page->next_cursor = next;
	}
	return 0;
}

static int panel_load(struct panel *panel, unsigned long cursor,
		      unsigned char clear_history)
{
	int result;

	if (panel->source == PANEL_REMOTE)
		result = kc_remote_list(panel->path, cursor, &panel->page);
	else
		result = local_list(panel->path, cursor, &panel->page);

	if (result < 0) {
		panel->page.count = 0u;
		panel->page.next_cursor = 0ul;
		panel->selected = 0u;
		if (panel->source == PANEL_REMOTE) set_error(result);
		else set_local_error(result);
		return result;
	}
	panel->selected = 0u;
	if (clear_history != 0u) {
		panel->history_depth = 0u;
	}
	set_status("Ready");
	return 0;
}

static int connect_remote(void)
{
	char name[64];
	int result;

	set_status("Connecting to PC server...");
	render_commander();
	result = kc_remote_hello(name, sizeof(name), &remote_capabilities);
	if (result < 0) {
		remote_online = 0u;
		remote_capabilities = 0u;
		copy_text(server_name, "PC offline - R reconnect",
			  sizeof(server_name));
		set_status("PC server timeout - start host/cable, press R");
		return result;
	}
	remote_online = 1u;
	copy_text(server_name, name, sizeof(server_name));
	if (panels[0].source == PANEL_REMOTE)
		(void)panel_load(&panels[0], panels[0].page.cursor, 0u);
	if (remote_online != 0u && panels[1].source == PANEL_REMOTE)
		(void)panel_load(&panels[1], panels[1].page.cursor, 0u);
	return remote_online != 0u ? 0 : -1;
}

static int join_path(char *target, const char *directory, const char *name)
{
	unsigned int dir_length = bounded_length(directory, KC_MAX_PATH + 1u);
	unsigned int name_length = bounded_length(name, KC_MAX_NAME + 1u);
	unsigned int used = dir_length;

	if (dir_length == 0u || dir_length > KC_MAX_PATH ||
	    name_length == 0u || name_length > KC_MAX_NAME) {
		return -1;
	}
	memcpy(target, directory, dir_length);
	if (used > 1u) {
		if (used + 1u + name_length > KC_MAX_PATH) return -1;
		target[used++] = '/';
	}
	memcpy(target + used, name, name_length);
	target[used + name_length] = '\0';
	return 0;
}

static void panel_parent(struct panel *panel)
{
	unsigned int length = bounded_length(panel->path, KC_MAX_PATH);

	if (length <= 1u) {
		return;
	}
	while (length > 1u && panel->path[length - 1u] != '/') {
		--length;
	}
	if (length > 1u) --length;
	panel->path[length] = '\0';
	(void)panel_load(panel, 0ul, 1u);
}

static void panel_next(struct panel *panel)
{
	if (panel->page.next_cursor == 0ul) {
		set_status("End of directory");
		return;
	}
	if (panel->history_depth < HISTORY_DEPTH) {
		panel->history[panel->history_depth++] = panel->page.cursor;
	}
	(void)panel_load(panel, panel->page.next_cursor, 0u);
}

static void panel_previous(struct panel *panel)
{
	unsigned long cursor;

	if (panel->history_depth == 0u) {
		if (panel->page.cursor != 0ul) {
			(void)panel_load(panel, 0ul, 0u);
		} else {
			set_status("Start of directory");
		}
		return;
	}
	cursor = panel->history[--panel->history_depth];
	(void)panel_load(panel, cursor, 0u);
}

static void render_help(void)
{
	static const char * const lines[] = {
		"KonaCommander help", "", "F1                focus the menu bar",
		"Left/Right       choose a menu; Enter opens it",
		"Tab              move to the next pane or button",
		"Arrow Up/Down    move the focused pane selection",
		"PageUp/PageDown  previous/next directory page",
		"Enter            activate the focused pane/button",
		"Backspace         parent directory", "F3 or V           view file",
		"F4 or X           toggle text/hex while viewing",
		"F5                copy file to opposite pane",
		"R                 refresh/reconnect", "F10 or Q          quit",
		"", "Only the focused control is highlighted.",
		"PC upload requires host option --write. Transfers ask first.",
		"Host paths stay sandboxed below the directory selected by --root.", "",
		"Press any key to return."
	};
	unsigned int row;

	screen_start();
	for (row = 0u; row < 26u; ++row) {
		if (row < sizeof(lines) / sizeof(lines[0])) {
			out_fixed(lines[row], SCREEN_COLUMNS);
		} else {
			out_repeat(' ', SCREEN_COLUMNS);
		}
		line_end((unsigned char)(row == 25u));
	}
	screen_finish();
	(void)read_key();
}

static unsigned char looks_binary(const unsigned char *data,
				  unsigned int length)
{
	unsigned int controls = 0u;
	unsigned int index;

	for (index = 0u; index < length; ++index) {
		unsigned char value = data[index];

		if (value == 0u) return 1u;
		if (value < 32u && value != '\r' && value != '\n' && value != '\t') {
			++controls;
		}
	}
	return (unsigned char)(length != 0u && controls > length / 8u);
}

static unsigned int render_viewer(const char *path, unsigned long offset,
				  unsigned long size, unsigned char hex_mode,
				  unsigned int count)
{
	unsigned int index = 0u;
	unsigned int row;

	screen_start();
	(void)tui_set_style(TUI_STYLE_TITLE);
	out_fixed(path, SCREEN_COLUMNS);
	(void)tui_set_style(TUI_STYLE_NORMAL);
	line_end(0u);
	if (hex_mode != 0u) {
		for (row = 0u; row < 22u; ++row) {
			unsigned int column;
			unsigned int begin = index;

			if (index < count) out_hex32(offset + index);
			else out_repeat(' ', 8u);
			out_text("  ");
			for (column = 0u; column < 16u; ++column) {
				if (index < count) {
					unsigned char value = view_data[index++];
					out_char(hex_digit((unsigned char)(value >> 4)));
					out_char(hex_digit((unsigned char)(value & 15u)));
				} else out_text("  ");
				out_char(' ');
			}
			out_char(' ');
			for (column = 0u; column < 16u; ++column) {
				if (begin + column < count) {
					unsigned char value = view_data[begin + column];
					out_char(value >= 32u && value != 127u ? (char)value : '.');
				} else out_char(' ');
			}
			out_repeat(' ', 10u);
			line_end(0u);
		}
	} else {
		for (row = 0u; row < 22u; ++row) {
			unsigned int column = 0u;

			while (column < SCREEN_COLUMNS && index < count) {
				unsigned char value = view_data[index++];

				if (value == '\r') continue;
				if (value == '\n') break;
				if (value == '\t') {
					unsigned int spaces = 8u - (column & 7u);
					while (spaces-- != 0u && column < SCREEN_COLUMNS) {
						out_char(' '); ++column;
					}
				} else {
					out_char(value >= 32u && value != 127u ? (char)value : '.');
					++column;
				}
			}
			out_repeat(' ', (unsigned int)(SCREEN_COLUMNS - column));
			line_end(0u);
		}
	}
	out_repeat(' ', SCREEN_COLUMNS); line_end(0u);
	(void)tui_set_style(TUI_STYLE_STATUS);
	out_fixed(hex_mode != 0u ? " HEX viewer" : " TEXT viewer", 14u);
	out_text(" offset "); out_hex32(offset);
	out_text(" / "); out_hex32(size);
	out_repeat(' ', 44u);
	(void)tui_set_style(TUI_STYLE_NORMAL); line_end(0u);
	out_fixed(" PgUp/PgDn or arrows: move   F4/X: text/hex   Esc/Q: return",
		  SCREEN_COLUMNS);
	line_end(1u);
	screen_finish();
	return index;
}

static int read_view_page(unsigned char source, int handle,
			  unsigned long offset, unsigned int *received)
{
	unsigned int total = 0u;

	if (source == PANEL_LOCAL) {
		long position = seek((unsigned int)handle, (long)offset,
				     KFS_SEEK_SET);
		int count;

		if (position < 0L) return (int)position;
		count = read((unsigned int)handle, view_data, VIEW_BUFFER);
		if (count < 0) return count;
		*received = (unsigned int)count;
		return 0;
	}

	while (total < VIEW_BUFFER) {
		unsigned int wanted = (unsigned int)(VIEW_BUFFER - total);
		unsigned int count;
		int result;

		if (wanted > VIEW_CHUNK) wanted = VIEW_CHUNK;
		result = kc_remote_read((unsigned char)handle, offset + total,
					view_data + total,
					wanted, &count);
		if (result < 0) return result;
		total = (unsigned int)(total + count);
		if (count < wanted) break;
	}
	*received = total;
	return 0;
}

static void view_file(unsigned char source, const char *path,
		      unsigned char force_hex)
{
	int handle = 0;
	unsigned long size = 0ul;
	unsigned long offset = 0ul;
	unsigned int received = 0u;
	unsigned int advance = 0u;
	unsigned long history[VIEW_HISTORY];
	unsigned char history_depth = 0u;
	unsigned char hex_mode;
	int result;

	if (source == PANEL_REMOTE) {
		unsigned char remote_handle = 0u;

		result = kc_remote_open(path, &remote_handle, &size);
		handle = remote_handle;
	} else {
		struct kfs_stat information;

		result = stat(path, &information);
		if (result >= 0) {
			size = information.size;
			handle = open(path, KFS_O_READ);
			result = handle < 0 ? handle : 0;
		} else handle = 0;
	}

	if (result < 0) {
		if (source == PANEL_REMOTE) set_error(result);
		else set_local_error(result);
		return;
	}
	result = read_view_page(source, handle, 0ul, &received);
	if (result < 0) {
		if (source == PANEL_REMOTE) {
			set_error(result);
			(void)kc_remote_close((unsigned char)handle);
		} else {
			set_local_error(result);
			(void)close((unsigned int)handle);
		}
		return;
	}
	hex_mode = force_hex != 0u ? 1u : looks_binary(view_data, received);
	for (;;) {
		int key;

		advance = render_viewer(path, offset, size, hex_mode, received);
		key = read_key();
		if (key == TUI_KEY_ESC || key == TUI_KEY_F10 ||
		    key == 'q' || key == 'Q') {
			break;
		}
		if (key == TUI_KEY_F4 || key == 'x' || key == 'X') {
			hex_mode = (unsigned char)(hex_mode == 0u);
			continue;
		}
		if (key == TUI_KEY_PGDN || key == TUI_KEY_DOWN ||
		    key == TUI_KEY_RIGHT ||
		    key == ' ') {
			if (advance != 0u && offset + advance < size) {
				if (history_depth < VIEW_HISTORY)
					history[history_depth++] = offset;
				offset += advance;
			}
			else continue;
		} else if (key == TUI_KEY_PGUP || key == TUI_KEY_UP ||
			   key == TUI_KEY_LEFT) {
			if (history_depth != 0u) offset = history[--history_depth];
			else if (offset != 0ul) offset = 0ul;
			else continue;
		} else continue;
		result = read_view_page(source, handle, offset, &received);
		if (result < 0) {
			if (source == PANEL_REMOTE) set_error(result);
			else set_local_error(result);
			break;
		}
	}
	if (source == PANEL_REMOTE)
		result = kc_remote_close((unsigned char)handle);
	else
		result = close((unsigned int)handle);
	if (result < 0) {
		if (source == PANEL_REMOTE) set_error(result);
		else set_local_error(result);
	}
	else set_status("Ready");
}

static void activate_entry(struct panel *panel, unsigned char view_mode)
{
	struct kc_entry *entry;
	char path[KC_MAX_PATH + 1u];

	if (panel->page.count == 0u) return;
	entry = &panel->page.entries[panel->selected];
	if (join_path(path, panel->path, entry->name) < 0) {
		set_status("Path is too long");
		return;
	}
	if ((entry->flags & KC_ENTRY_DIRECTORY) != 0u) {
		if (view_mode != 0u) {
			set_status("Select a file to view");
			return;
		}
		copy_text(panel->path, path, sizeof(panel->path));
		(void)panel_load(panel, 0ul, 1u);
	} else {
		view_file(panel->source, path, (unsigned char)(view_mode == 2u));
	}
}

static void set_copy_progress(unsigned long done, unsigned long total)
{
	copy_text(status_text, "Copying ", sizeof(status_text));
	decimal(done, status_text + 8u, 10u);
	status_text[18] = ' ';
	status_text[19] = '/';
	status_text[20] = ' ';
	decimal(total, status_text + 21u, 10u);
	status_text[31] = '\0';
	render_status();
}

static unsigned char confirm_copy(const struct panel *source,
				  const struct kc_entry *entry)
{
	unsigned int used;
	const char *destination = source->source == PANEL_LOCAL ? "PC" : "KonaOS";
	tui_id_t answer;

	copy_text(status_text, "Copy ", sizeof(status_text));
	used = bounded_length(status_text, sizeof(status_text));
	copy_text(status_text + used, entry->name,
		  (unsigned int)(sizeof(status_text) - used));
	used = bounded_length(status_text, sizeof(status_text));
	copy_text(status_text + used, " to ",
		  (unsigned int)(sizeof(status_text) - used));
	used = bounded_length(status_text, sizeof(status_text));
	copy_text(status_text + used, destination,
		  (unsigned int)(sizeof(status_text) - used));
	used = bounded_length(status_text, sizeof(status_text));
	copy_text(status_text + used, "? Existing destination will be replaced.",
		  (unsigned int)(sizeof(status_text) - used));
	answer = tui_confirm("Copy file", status_text);
	return (unsigned char)(answer == TUI_ID_YES);
}

static int local_temporary(const char *directory, int *handle)
{
	unsigned char attempt;

	for (attempt = 0u; attempt < 10u; ++attempt) {
		if (join_path(copy_temporary, directory, "KCTMP0.$$$") < 0)
			return -KOS_EINVAL;
		copy_temporary[bounded_length(copy_temporary, KC_MAX_PATH) - 5u] =
			(char)('0' + attempt);
		if (bounded_length(copy_temporary, KC_MAX_PATH + 1u) > LOCAL_PATH_MAX)
			return -KOS_EINVAL;
		if (bounded_length(copy_temporary, KC_MAX_PATH + 1u) ==
		    bounded_length(copy_destination, KC_MAX_PATH + 1u) &&
		    memcmp(copy_temporary, copy_destination,
			   bounded_length(copy_temporary, KC_MAX_PATH + 1u)) == 0)
			continue;
		*handle = open(copy_temporary,
			       KFS_O_WRITE | KFS_O_CREATE | KFS_O_EXCL);
		if (*handle >= 0) return 0;
		if (*handle != -KOS_EEXIST) return *handle;
	}
	return -KOS_EEXIST;
}

static int write_all_local(int handle, const unsigned char *data,
			   unsigned int count)
{
	unsigned int used = 0u;

	while (used < count) {
		int result = write((unsigned int)handle, data + used,
				   (unsigned int)(count - used));
		if (result < 0) return result;
		if (result == 0) return -KOS_EIO;
		used = (unsigned int)(used + result);
	}
	return 0;
}

static int copy_local_to_pc(const struct panel *source,
			    struct panel *destination,
			    const struct kc_entry *entry)
{
	struct kfs_stat information;
	unsigned long offset = 0ul;
	unsigned char remote_handle = 0u;
	int local_handle;
	int result;

	if ((remote_capabilities & KC_CAP_WRITE) == 0u) {
		set_status("PC writes disabled - restart host with --write");
		return -1;
	}
	if (join_path(copy_source, source->path, entry->name) < 0 ||
	    join_path(copy_destination, destination->path, entry->name) < 0) {
		set_status("Copy path is too long");
		return -1;
	}
	result = stat(copy_source, &information);
	if (result < 0) { set_local_error(result); return result; }
	local_handle = open(copy_source, KFS_O_READ);
	if (local_handle < 0) { set_local_error(local_handle); return local_handle; }
	result = kc_remote_create(copy_destination, &remote_handle);
	if (result < 0) {
		(void)close((unsigned int)local_handle);
		set_error(result);
		return result;
	}
	for (;;) {
		int count = read((unsigned int)local_handle, view_data, COPY_CHUNK);

		if (count < 0) { result = count; break; }
		if (count == 0) { result = 0; break; }
		result = kc_remote_write(remote_handle, offset, view_data,
					 (unsigned int)count);
		if (result < 0) break;
		offset += (unsigned int)count;
		set_copy_progress(offset, information.size);
	}
	(void)close((unsigned int)local_handle);
	if (result < 0) {
		(void)kc_remote_abort(remote_handle);
		if (result <= KC_LINK_EDEVICE && result >= KC_LINK_ECOBS)
			set_error(result);
		else if (result <= -KC_REMOTE_STATUS_BASE) set_error(result);
		else set_local_error(result);
		return result;
	}
	result = kc_remote_commit(remote_handle);
	if (result < 0) { set_error(result); return result; }
	(void)panel_load(destination, destination->page.cursor, 0u);
	set_status("Copy to PC complete");
	return 0;
}

static int copy_pc_to_local(const struct panel *source,
			    struct panel *destination,
			    const struct kc_entry *entry)
{
	struct kfs_stat existing;
	unsigned long size = 0ul;
	unsigned long offset = 0ul;
	unsigned char remote_handle = 0u;
	int local_handle = 0;
	int result;

	if (join_path(copy_source, source->path, entry->name) < 0 ||
	    join_path(copy_destination, destination->path, entry->name) < 0 ||
	    bounded_length(copy_destination, KC_MAX_PATH + 1u) > LOCAL_PATH_MAX) {
		set_status("Destination path is too long for KonaOS");
		return -1;
	}
	result = kc_remote_open(copy_source, &remote_handle, &size);
	if (result < 0) { set_error(result); return result; }
	result = local_temporary(destination->path, &local_handle);
	if (result < 0) {
		(void)kc_remote_close(remote_handle);
		set_local_error(result);
		return result;
	}
	for (;;) {
		unsigned int count;

		result = kc_remote_read(remote_handle, offset, view_data,
					COPY_CHUNK, &count);
		if (result < 0) break;
		if (count == 0u) { result = 0; break; }
		result = write_all_local(local_handle, view_data, count);
		if (result < 0) break;
		offset += count;
		set_copy_progress(offset, size);
	}
	(void)kc_remote_close(remote_handle);
	if (result >= 0) result = sync((unsigned int)local_handle);
	if (close((unsigned int)local_handle) < 0 && result >= 0) result = -KOS_EIO;
	if (result < 0) {
		(void)unlink(copy_temporary);
		if (result <= KC_LINK_EDEVICE && result >= KC_LINK_ECOBS)
			set_error(result);
		else if (result <= -KC_REMOTE_STATUS_BASE) set_error(result);
		else set_local_error(result);
		return result;
	}
	result = stat(copy_destination, &existing);
	if (result >= 0) result = unlink(copy_destination);
	else if (result == -KOS_ENOENT) result = 0;
	if (result >= 0) result = rename(copy_temporary, copy_destination);
	if (result < 0) {
		set_local_error(result);
		return result;
	}
	(void)panel_load(destination, destination->page.cursor, 0u);
	set_status("Copy to KonaOS complete");
	return 0;
}

static void copy_selected(struct panel *source)
{
	struct panel *destination = &panels[1u - active_panel];
	struct kc_entry *entry;

	if (source->page.count == 0u) { set_status("Select a file to copy"); return; }
	entry = &source->page.entries[source->selected];
	if ((entry->flags & KC_ENTRY_DIRECTORY) != 0u) {
		set_status("Directory copy is not supported");
		return;
	}
	if (remote_online == 0u) {
		set_status("PC offline - press R to connect");
		return;
	}
	if (source->source == PANEL_LOCAL &&
	    (remote_capabilities & KC_CAP_WRITE) == 0u) {
		set_status("PC writes disabled - restart host with --write");
		return;
	}
	if (confirm_copy(source, entry) == 0u) { set_status("Copy cancelled"); return; }
	if (source->source == PANEL_LOCAL)
		(void)copy_local_to_pc(source, destination, entry);
	else
		(void)copy_pc_to_local(source, destination, entry);
}

static void focus_set(unsigned char focus)
{
	if (focus >= FOCUS_COUNT) {
		focus = FOCUS_MENU;
	}
	ui_focus = focus;
	if (focus == FOCUS_LEFT) {
		active_panel = 0u;
	} else if (focus == FOCUS_RIGHT) {
		active_panel = 1u;
	}
}

static void focus_next(void)
{
	unsigned char next = (unsigned char)(ui_focus + 1u);

	if (next >= FOCUS_COUNT) {
		next = FOCUS_MENU;
	}
	focus_set(next);
}

static void menu_move(int direction)
{
	if (direction < 0) {
		if (menu_selected == MENU_FILE) menu_selected = MENU_HELP;
		else --menu_selected;
	} else {
		if (menu_selected == MENU_HELP) menu_selected = MENU_FILE;
		else ++menu_selected;
	}
}

static const struct action_item *menu_actions(tui_id_t menu,
					       unsigned char *count,
					       const char **title,
					       unsigned char *x)
{
	switch (menu) {
	case MENU_FILE:
		*count = (unsigned char)(sizeof(file_actions) /
					 sizeof(file_actions[0]));
		*title = "File";
		*x = 0u;
		return file_actions;
	case MENU_EDIT:
		*count = (unsigned char)(sizeof(edit_actions) /
					 sizeof(edit_actions[0]));
		*title = "Edit";
		*x = 5u;
		return edit_actions;
	case MENU_VIEW:
		*count = (unsigned char)(sizeof(view_actions) /
					 sizeof(view_actions[0]));
		*title = "View";
		*x = 10u;
		return view_actions;
	default:
		*count = (unsigned char)(sizeof(help_actions) /
					 sizeof(help_actions[0]));
		*title = "Help";
		*x = 15u;
		return help_actions;
	}
}

static void menu_popup_rect(struct tui_rect *box, unsigned char count,
			    unsigned char x)
{
	box->x = x;
	box->y = 1u;
	box->w = MENU_POPUP_WIDTH;
	box->h = (uint8_t)(count + 2u);
}

static void render_popup_item(const struct action_item *items,
			      unsigned char item, unsigned char selected,
			      const struct tui_rect *box)
{
	struct tui_rect line;

	line.x = (uint8_t)(box->x + 1u);
	line.y = (uint8_t)(box->y + 1u + item);
	line.w = (uint8_t)(box->w - 2u);
	line.h = 1u;
	(void)tui_fill(&line, ' ', item == selected ?
			   TUI_STYLE_SELECTED : TUI_STYLE_NORMAL);
	(void)tui_set_style(item == selected ? TUI_STYLE_SELECTED :
			    TUI_STYLE_NORMAL);
	(void)tui_puts_xy((uint8_t)(line.x + 1u), line.y,
			 items[item].label);
}

static void draw_popup(const struct action_item *items,
		       unsigned char count, unsigned char selected,
		       const char *title, const struct tui_rect *box)
{
	unsigned char i;

	(void)tui_fill(box, ' ', TUI_STYLE_NORMAL);
	(void)tui_frame_title(box, TUI_FRAME_SINGLE, TUI_STYLE_TITLE, title);
	for (i = 0u; i < count; ++i) {
		render_popup_item(items, i, selected, box);
	}
	(void)tui_set_style(TUI_STYLE_NORMAL);
}

static void open_popup(const struct action_item *items,
		       unsigned char count, unsigned char selected,
		       const char *title, struct tui_rect *box,
		       unsigned char x)
{
	menu_popup_rect(box, count, x);
	tui_begin();
	(void)tui_save(box, menu_background,
		       (uint16_t)sizeof(menu_background));
	draw_popup(items, count, selected, title, box);
	(void)tui_end();
}

static void close_popup(const struct tui_rect *box)
{
	tui_begin();
	(void)tui_restore(box, menu_background,
			  (uint16_t)sizeof(menu_background));
	(void)tui_end();
}

static void render_popup_selection(const struct action_item *items,
				   unsigned char old_selected,
				   unsigned char selected,
				   const struct tui_rect *box)
{
	tui_begin();
	render_popup_item(items, old_selected, selected, box);
	if (selected != old_selected) {
		render_popup_item(items, selected, selected, box);
	}
	(void)tui_set_style(TUI_STYLE_NORMAL);
	(void)tui_end();
}

static unsigned char run_menu(void)
{
	unsigned char selected = 0u;
	const struct action_item *items;
	const char *title;
	unsigned char count;
	unsigned char x;
	struct tui_rect box;

	items = menu_actions(menu_selected, &count, &title, &x);
	open_popup(items, count, selected, title, &box, x);
	for (;;) {
		int key;
		unsigned char old_selected;

		key = read_key();
		if (key == '\t') {
			close_popup(&box);
			focus_next();
			return ACTION_NONE;
		}
		if (key == TUI_KEY_ESC || key == TUI_KEY_F1) {
			close_popup(&box);
			return ACTION_NONE;
		}
		if (key == TUI_KEY_LEFT) {
			close_popup(&box);
			menu_move(-1);
			selected = 0u;
			items = menu_actions(menu_selected, &count, &title, &x);
			render_menu_bar();
			open_popup(items, count, selected, title, &box, x);
		} else if (key == TUI_KEY_RIGHT) {
			close_popup(&box);
			menu_move(1);
			selected = 0u;
			items = menu_actions(menu_selected, &count, &title, &x);
			render_menu_bar();
			open_popup(items, count, selected, title, &box, x);
		} else if (key == TUI_KEY_UP) {
			old_selected = selected;
			if (selected == 0u) selected = (unsigned char)(count - 1u);
			else --selected;
			render_popup_selection(items, old_selected, selected, &box);
		} else if (key == TUI_KEY_DOWN) {
			old_selected = selected;
			++selected;
			if (selected >= count) selected = 0u;
			render_popup_selection(items, old_selected, selected, &box);
		} else if (key == '\r' || key == '\n') {
			close_popup(&box);
			return items[selected].action;
		}
	}
}

static unsigned char focused_action(void)
{
	switch (ui_focus) {
	case FOCUS_HELP:
		return ACTION_HELP;
	case FOCUS_VIEW:
		return ACTION_TEXT_VIEW;
	case FOCUS_HEX:
		return ACTION_HEX_VIEW;
	case FOCUS_COPY:
		return ACTION_COPY;
	case FOCUS_REFRESH:
		return ACTION_REFRESH;
	case FOCUS_QUIT:
		return ACTION_QUIT;
	default:
		return ACTION_NONE;
	}
}

static unsigned char perform_action(unsigned char action)
{
	struct panel *panel = &panels[active_panel];

	switch (action) {
	case ACTION_OPEN:
		activate_entry(panel, 0u);
		break;
	case ACTION_COPY:
		copy_selected(panel);
		break;
	case ACTION_REFRESH:
		if (panel->source == PANEL_REMOTE && remote_online == 0u)
			(void)connect_remote();
		else
			(void)panel_load(panel, panel->page.cursor, 0u);
		break;
	case ACTION_TEXT_VIEW:
		activate_entry(panel, 1u);
		break;
	case ACTION_HEX_VIEW:
		activate_entry(panel, 2u);
		break;
	case ACTION_HELP:
		render_help();
		break;
	case ACTION_QUIT:
		return 1u;
	default:
		break;
	}
	return 0u;
}

int kc_ui_run(void)
{
	struct panel *panel;
	struct tui_size size;
	unsigned char focus_before_menu = FOCUS_LEFT;
	int key;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("konacmd: TUI initialization failed %d\r\n", result);
		return 1;
	}
	result = tui_get_size(&size);
	if (result < 0 || size.cols < SCREEN_COLUMNS || size.rows < 26u) {
		tui_shutdown();
		printf("konacmd: an 85x26 console is required\r\n");
		return 1;
	}

	copy_text(server_name, "PC offline - R reconnect", sizeof(server_name));
	panels[0].source = PANEL_LOCAL;
	panels[1].source = PANEL_REMOTE;
	copy_text(panels[0].path, "/fs", sizeof(panels[0].path));
	copy_text(panels[1].path, "/", sizeof(panels[1].path));
	panels[0].history_depth = 0u;
	panels[1].history_depth = 0u;
	panels[0].page.cursor = 0ul;
	panels[0].page.next_cursor = 0ul;
	panels[0].page.count = 0u;
	panels[0].selected = 0u;
	panels[1].page.cursor = 0ul;
	panels[1].page.next_cursor = 0ul;
	panels[1].page.count = 0u;
	panels[1].selected = 0u;
	active_panel = 0u;
	ui_focus = FOCUS_LEFT;
	menu_selected = MENU_FILE;
	remote_online = 0u;
	remote_capabilities = 0u;
	(void)panel_load(&panels[0], 0ul, 1u);
	set_status("Starting without PC; automatic connection attempt follows");
	render_commander();
	(void)connect_remote();
	render_commander();
	for (;;) {
		unsigned char old_selected;
		unsigned char action = ACTION_NONE;
		unsigned char old_focus;
		unsigned char old_active;

		key = read_key();
		panel = &panels[active_panel];
		old_selected = panel->selected;
		if (key == TUI_KEY_F10 || key == 'q' || key == 'Q') break;
		if (key == TUI_KEY_F1) {
			old_focus = ui_focus;
			old_active = active_panel;
			if (ui_focus != FOCUS_MENU) focus_before_menu = ui_focus;
			focus_set(FOCUS_MENU);
			render_focus_change(old_focus, old_active, 0u);
			continue;
		}
		if (key == '\t') {
			old_focus = ui_focus;
			old_active = active_panel;
			focus_next();
			render_focus_change(old_focus, old_active, 0u);
			continue;
		}
		if (ui_focus == FOCUS_MENU) {
			old_focus = ui_focus;
			old_active = active_panel;
			if (key == TUI_KEY_LEFT || key == TUI_KEY_RIGHT) {
				menu_move(key == TUI_KEY_LEFT ? -1 : 1);
				tui_begin();
				render_menu_bar();
				(void)tui_end();
				continue;
			}
			else if (key == '\r' || key == '\n' || key == TUI_KEY_DOWN)
				action = run_menu();
			else if (key == TUI_KEY_ESC) {
				focus_set(focus_before_menu);
				render_focus_change(old_focus, old_active, 0u);
				continue;
			}
			if (perform_action(action) != 0u) break;
			render_commander();
			continue;
		}
		if (key == TUI_KEY_F3 || key == 'v' || key == 'V') {
			action = ACTION_TEXT_VIEW;
		} else if (key == TUI_KEY_F4 || key == 'x' || key == 'X') {
			action = ACTION_HEX_VIEW;
		} else if (key == TUI_KEY_F5) {
			action = ACTION_COPY;
		} else if (key == 'h' || key == 'H') {
			action = ACTION_HELP;
		} else if (key == 'r' || key == 'R') {
			action = ACTION_REFRESH;
		} else if ((key == '\r' || key == '\n' || key == ' ') &&
			   ui_focus >= FOCUS_HELP) {
			action = focused_action();
		}
		if (action != ACTION_NONE) {
			if (perform_action(action) != 0u) break;
			render_commander();
			continue;
		}
		if (ui_focus >= FOCUS_HELP) {
			old_focus = ui_focus;
			old_active = active_panel;

			if (key == TUI_KEY_LEFT) {
				focus_set((unsigned char)(ui_focus == FOCUS_HELP ?
						 FOCUS_QUIT : ui_focus - 1u));
			} else if (key == TUI_KEY_RIGHT) {
				focus_set((unsigned char)(ui_focus == FOCUS_QUIT ?
						 FOCUS_HELP : ui_focus + 1u));
			}
			if (ui_focus != old_focus)
				render_focus_change(old_focus, old_active, 0u);
			continue;
		}
		if (panel->source == PANEL_REMOTE && remote_online == 0u) {
			set_status("PC offline - start server/connect cable, press R");
			render_status();
			continue;
		}
		if (key == TUI_KEY_UP) {
			if (panel->selected != 0u) {
				--panel->selected;
				render_navigation(old_selected);
			} else {
				panel_previous(panel);
				render_commander();
			}
		} else if (key == TUI_KEY_DOWN) {
			if (panel->selected + 1u < panel->page.count) {
				++panel->selected;
				render_navigation(old_selected);
			} else {
				panel_next(panel);
				render_commander();
			}
		} else if (key == TUI_KEY_PGUP) {
			panel_previous(panel);
			render_commander();
		} else if (key == TUI_KEY_PGDN) {
			panel_next(panel);
			render_commander();
		} else if (key == 8 || key == 127 || key == TUI_KEY_LEFT) {
			panel_parent(panel);
			render_commander();
		} else if (key == 13 || key == 10 || key == TUI_KEY_RIGHT) {
			activate_entry(panel, 0u);
			render_commander();
		}
	}
	tui_begin();
	(void)tui_clear();
	(void)tui_set_style(TUI_STYLE_NORMAL);
	(void)tui_gotoxy(0u, 0u);
	(void)tui_cursor_show();
	(void)tui_end();
	tui_shutdown();
	return 0;
}

