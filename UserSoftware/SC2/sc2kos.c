#include "sc2kos.h"

#define SCREEN_WIDTH 84u
#define GRID_COLS     8u
#define GRID_ROWS     18u
#define CELL_WIDTH    10u
#define OUTPUT_SIZE   768u

#define KEY_UP       0x100
#define KEY_DOWN     0x101
#define KEY_RIGHT    0x102
#define KEY_LEFT     0x103
#define KEY_HOME     0x104
#define KEY_END      0x105
#define KEY_PGUP     0x106
#define KEY_PGDN     0x107

static unsigned char current_col;
static unsigned char current_row;
static unsigned char view_col;
static unsigned char view_row;
static unsigned char dirty;
static char work_path[SC_PATH_MAX + 1u];
static char message[SCREEN_WIDTH + 1u];
static char output_buffer[OUTPUT_SIZE];
static unsigned int output_length;

static void output_send(void)
{
	if (output_length != 0u) {
		(void)console_write(output_buffer, output_length);
		output_length = 0u;
	}
}

static void output_reset(void)
{
	output_length = 0u;
}

static void output_write(const char *text, unsigned int length)
{
	while (length != 0u) {
		unsigned int room = OUTPUT_SIZE - output_length;
		unsigned int amount = length < room ? length : room;

		if (amount != 0u) {
			memcpy(&output_buffer[output_length], text, amount);
			output_length += amount;
			text += amount;
			length -= amount;
		}
		if (output_length == OUTPUT_SIZE) { output_send(); }
	}
}

static void output_text(const char *text)
{
	output_write(text, strlen(text));
}

static void output_cursor(unsigned int row, unsigned int col)
{
	char number[6];

	output_text("\x1b[");
	utoa(number, row, 10u, 0u, '0');
	output_text(number);
	output_write(";", 1u);
	utoa(number, col, 10u, 0u, '0');
	output_text(number);
	output_write("H", 1u);
}

static unsigned char upper(unsigned char ch)
{
	if (ch >= 'a' && ch <= 'z') { return (unsigned char)(ch - 32u); }
	return ch;
}

static void line_clear(char *line)
{
	memset(line, ' ', SCREEN_WIDTH);
	line[SCREEN_WIDTH] = '\0';
}

static void line_text(char *line, unsigned int at, const char *text)
{
	while (*text != '\0' && at < SCREEN_WIDTH) {
		line[at++] = *text++;
	}
}

static void write_line(const char *line, unsigned char last)
{
	output_write(line, SCREEN_WIDTH);
	if (last == 0u) { output_write("\r\n", 2u); }
}

static void fit_cell(char *dst, const char *text)
{
	unsigned int len = strlen(text);
	unsigned int i;

	memset(dst, ' ', CELL_WIDTH);
	if (len > CELL_WIDTH - 1u) { len = CELL_WIDTH - 1u; }
	for (i = 0u; i < len; ++i) { dst[i] = text[i]; }
}

static void adjust_view(void)
{
	if (current_col < view_col) { view_col = current_col; }
	if ((unsigned int)current_col >= (unsigned int)view_col + GRID_COLS) {
		view_col = (unsigned char)(current_col - GRID_COLS + 1u);
	}
	if (current_row < view_row) { view_row = current_row; }
	if ((unsigned int)current_row >= (unsigned int)view_row + GRID_ROWS) {
		view_row = (unsigned char)(current_row - GRID_ROWS + 1u);
	}
}

static void cell_display(char *dst, unsigned char col, unsigned char row)
{
	struct sc_cell *cell = sheet_find(col, row);
	char value[24];

	if (cell == (struct sc_cell *)0) {
		fit_cell(dst, "");
	} else if (cell->type == SC_CELL_TEXT) {
		fit_cell(dst, sheet_text(cell));
	} else if (cell->flags & SC_FLAG_ERROR) {
		fit_cell(dst, "#ERROR");
	} else {
		sheet_format_value(value, sizeof(value), cell->value, cell->precision);
		fit_cell(dst, value);
	}
}

static void draw_grid_row(unsigned char row)
{
	char base[SCREEN_WIDTH + 1u];
	char output[SCREEN_WIDTH + 12u];
	char number[6];
	unsigned int col;
	unsigned int out = 0u;
	unsigned int selected_at = SCREEN_WIDTH + 1u;

	line_clear(base);
	utoa(number, (unsigned int)row + 1u, 10u, 3u, ' ');
	line_text(base, 0u, number);
	base[3] = ' ';
	for (col = 0u; col < GRID_COLS; ++col) {
		unsigned char actual_col = (unsigned char)(view_col + col);
		if (actual_col >= SC_MAX_COLS) { break; }
		cell_display(&base[4u + col * CELL_WIDTH], actual_col, row);
		if (actual_col == current_col && row == current_row) {
			selected_at = 4u + col * CELL_WIDTH;
		}
	}
	for (col = 0u; col < SCREEN_WIDTH; ++col) {
		if (col == selected_at) {
			memcpy(&output[out], "\x1b[7m", 4u); out += 4u;
		}
		if (col == selected_at + CELL_WIDTH) {
			memcpy(&output[out], "\x1b[27m", 5u); out += 5u;
		}
		output[out++] = base[col];
	}
	if (selected_at + CELL_WIDTH == SCREEN_WIDTH) {
		memcpy(&output[out], "\x1b[27m", 5u); out += 5u;
	}
	output_write(output, out);
	output_write("\r\n", 2u);
}

static void make_title_line(char *line)
{
	char ref[8];

	line_clear(line);
	line_text(line, 0u, "SC2Kos 1.1 - SuperCalc2-style worksheet for KonaOS");
	sheet_ref(ref, current_col, current_row);
	line_text(line, 73u, ref);
	if (dirty != 0u) { line_text(line, 82u, "*"); }
}

static void make_formula_line(char *line)
{
	char ref[8];
	struct sc_cell *cell = sheet_find(current_col, current_row);

	line_clear(line);
	line_text(line, 0u, ">");
	sheet_ref(ref, current_col, current_row);
	line_text(line, 2u, ref);
	line_text(line, 8u, "=");
	if (cell != (struct sc_cell *)0) { line_text(line, 10u, sheet_text(cell)); }
}

static void make_entry_line(char *line)
{
	char temp[32];
	struct sc_cell *cell = sheet_find(current_col, current_row);

	line_clear(line);
	line_text(line, 0u, "Entry: ");
	if (cell != (struct sc_cell *)0) {
		if (cell->flags & SC_FLAG_PROTECTED) { line_text(line, 7u, "[P] "); }
		if (cell->type != SC_CELL_TEXT && !(cell->flags & SC_FLAG_ERROR)) {
			sheet_format_value(temp, sizeof(temp), cell->value, 3u);
			line_text(line, 11u, temp);
		}
	}
}

static void redraw(void)
{
	char line[SCREEN_WIDTH + 1u];
	char ref[8];
	char temp[32];
	unsigned int col;
	unsigned int row;
	unsigned char last_col, last_row;

	adjust_view();
	output_reset();
	output_text("\x1b[?25l\x1b[H");
	make_title_line(line);
	write_line(line, 0u);

	make_formula_line(line);
	write_line(line, 0u);

	line_clear(line);
	for (col = 0u; col < GRID_COLS; ++col) {
		unsigned char actual = (unsigned char)(view_col + col);
		if (actual >= SC_MAX_COLS) { break; }
		sheet_ref(ref, actual, 0u);
		ref[strlen(ref) - 1u] = '\0';
		line_text(line, 8u + col * CELL_WIDTH, ref);
	}
	write_line(line, 0u);

	for (row = 0u; row < GRID_ROWS; ++row) {
		unsigned int actual_row = (unsigned int)view_row + row;
		if (actual_row < SC_MAX_ROWS) {
			draw_grid_row((unsigned char)actual_row);
		} else {
			line_clear(line); write_line(line, 0u);
		}
	}

	make_entry_line(line);
	write_line(line, 0u);

	line_clear(line);
	sheet_used(&last_col, &last_row);
	line_text(line, 0u, "Cells: ");
	utoa(temp, sheet_count(), 10u, 0u, '0'); line_text(line, 7u, temp);
	line_text(line, 15u, "Free: ");
	utoa(temp, SC_MAX_CELLS - sheet_count(), 10u, 0u, '0'); line_text(line, 21u, temp);
	line_text(line, 31u, "Last: ");
	sheet_ref(ref, last_col, last_row); line_text(line, 37u, ref);
	if (work_path[0] != '\0') { line_text(line, 48u, work_path); }
	write_line(line, 0u);

	line_clear(line);
	line_text(line, 0u, "/ B C D E F G I L M O P Q R S U Z    ? Help");
	write_line(line, 0u);

	line_clear(line);
	line_text(line, 0u, message);
	write_line(line, 0u);

	line_clear(line);
	line_text(line, 0u, "Arrows move | type to enter | Enter edits | Ctrl-Z cancels prompts");
	write_line(line, 1u);
	output_send();
	(void)console_flush();
}

static void draw_one_cell(unsigned char col, unsigned char row,
			  unsigned char selected)
{
	char display[CELL_WIDTH];

	if (col < view_col || (unsigned int)col >= (unsigned int)view_col + GRID_COLS ||
	    row < view_row || (unsigned int)row >= (unsigned int)view_row + GRID_ROWS) {
		return;
	}
	output_cursor(4u + (unsigned int)(row - view_row),
		      5u + (unsigned int)(col - view_col) * CELL_WIDTH);
	output_text(selected != 0u ? "\x1b[7m" : "\x1b[27m");
	cell_display(display, col, row);
	output_write(display, CELL_WIDTH);
	if (selected != 0u) { output_text("\x1b[27m"); }
}

static void redraw_navigation(unsigned char old_col, unsigned char old_row)
{
	char line[SCREEN_WIDTH + 1u];
	char location[11];
	char ref[8];

	output_reset();
	if (old_col != current_col || old_row != current_row) {
		draw_one_cell(old_col, old_row, 0u);
	}
	draw_one_cell(current_col, current_row, 1u);

	memset(location, ' ', sizeof(location));
	sheet_ref(ref, current_col, current_row);
	memcpy(location, ref, strlen(ref));
	if (dirty != 0u) { location[9] = '*'; }
	output_cursor(1u, 74u);
	output_write(location, sizeof(location));

	make_formula_line(line);
	output_cursor(2u, 1u);
	output_write(line, SCREEN_WIDTH);
	make_entry_line(line);
	output_cursor(22u, 1u);
	output_write(line, SCREEN_WIDTH);
	line_clear(line);
	line_text(line, 0u, message);
	output_cursor(25u, 1u);
	output_write(line, SCREEN_WIDTH);
	output_send();
	(void)console_flush();
}

static int read_key(void)
{
	int ch = getchar();

	if (ch != 0x1b) { return ch; }
	if (console_poll() <= 0) { return 0x1b; }
	ch = getchar();
	if (ch == 'O') {
		ch = getchar();
		if (ch == 'H') { return KEY_HOME; }
		if (ch == 'F') { return KEY_END; }
		return 0x1b;
	}
	if (ch != '[') { return 0x1b; }
	ch = getchar();
	if (ch == 'A') { return KEY_UP; }
	if (ch == 'B') { return KEY_DOWN; }
	if (ch == 'C') { return KEY_RIGHT; }
	if (ch == 'D') { return KEY_LEFT; }
	if (ch == 'H') { return KEY_HOME; }
	if (ch == 'F') { return KEY_END; }
	if (ch >= '0' && ch <= '9') {
		int number = ch - '0';
		while ((ch = getchar()) >= '0' && ch <= '9') {
			number = number * 10 + ch - '0';
		}
		if (ch == '~' && number == 5) { return KEY_PGUP; }
		if (ch == '~' && number == 6) { return KEY_PGDN; }
	}
	return 0x1b;
}

static int prompt_line(const char *prompt, char *buffer, unsigned int size)
{
	int result;

	console_puts("\x1b[26;1H\x1b[2K\x1b[?25h");
	console_puts(prompt);
	result = console_readline(buffer, size);
	console_write("\x1b[?25l", 6u);
	if (result < 0) { return result; }
	return buffer[0] == 0x1a ? -1 : result;
}

static void set_result_message(int result)
{
	if (result == SC_OK) { strlcpy(message, "Ready", sizeof(message)); }
	else if (result == SC_ERR_FULL) { strlcpy(message, "Memory FULL", sizeof(message)); }
	else if (result == SC_ERR_FORMULA) { strlcpy(message, "Formula ERROR", sizeof(message)); }
	else if (result == SC_ERR_PROTECTED) { strlcpy(message, "Protected Entry", sizeof(message)); }
	else if (result == SC_ERR_RANGE) { strlcpy(message, "Range ERROR", sizeof(message)); }
	else if (result == SC_ERR_FORMAT) { strlcpy(message, "File not loadable", sizeof(message)); }
	else { strlcpy(message, "Disk error", sizeof(message)); }
}

static void edit_cell(int initial)
{
	char buffer[SC_INPUT_MAX + 1u];
	char prompt[16];
	unsigned int length = 0u;
	int ch;
	int result;

	if (initial < 0) {
		struct sc_cell *cell = sheet_find(current_col, current_row);
		if (cell != (struct sc_cell *)0) {
			strlcpy(buffer, sheet_text(cell), sizeof(buffer));
			length = strlen(buffer);
		}
	} else {
		buffer[length++] = (char)initial;
	}
	buffer[length] = '\0';
	sheet_ref(prompt, current_col, current_row);
	strlcat(prompt, "> ", sizeof(prompt));
	console_puts("\x1b[26;1H\x1b[2K\x1b[?25h");
	console_puts(prompt);
	console_puts(buffer);
	(void)console_flush();
	for (;;) {
		ch = getchar();
		if (ch == '\r' || ch == '\n') { break; }
		if (ch == 0x1a || ch == 0x1b) {
			strlcpy(message, "Entry cancelled", sizeof(message));
			console_write("\x1b[?25l", 6u);
			return;
		}
		if (ch == 0x08 || ch == 0x7f) {
			if (length != 0u) {
				--length; buffer[length] = '\0';
				console_write("\b \b", 3u);
			}
			continue;
		}
		if (ch >= 0x20 && ch < 0x7f && length < SC_INPUT_MAX) {
			buffer[length++] = (char)ch; buffer[length] = '\0'; putchar(ch);
		}
	}
	console_write("\x1b[?25l", 6u);
	result = sheet_set(current_col, current_row, buffer);
	set_result_message(result);
	if (result == SC_OK || result == SC_ERR_FORMULA) {
		dirty = 1u;
		if (current_row + 1u < SC_MAX_ROWS) { ++current_row; }
	}
}

static int ask_yes(const char *prompt)
{
	char answer[4];

	if (prompt_line(prompt, answer, sizeof(answer)) < 0) { return 0; }
	return upper((unsigned char)answer[0]) == 'Y';
}

static int ask_ref(const char *prompt, unsigned char *col, unsigned char *row)
{
	char input[24];
	const char *end;

	if (prompt_line(prompt, input, sizeof(input)) <= 0) { return 0; }
	if (!sheet_parse_ref(input, col, row, &end)) { return 0; }
	while (*end == ' ' || *end == '\t') { ++end; }
	return *end == '\0';
}

static void show_help(void)
{
	static const char *const lines[] = {
		"SC2Kos quick reference",
		"",
		"Arrow keys        Move the active cell",
		"Printable text    Replace the active cell; ' forces a label",
		"=A1+B1            Formula (+ - * / and parentheses)",
		"=SUM(A1:B9)        Range functions: SUM AVERAGE MIN MAX COUNT",
		"=IF(A1>0,A1,0)     Comparisons and IF AND OR NOT",
		"=ROUND(A1,2)       ABS INT SIGN SQRT MOD POWER ROUND",
		"=DATE(2026,10,16)  DATE YEAR MONTH DAY; financial PMT FV",
		"",
		"/B  Blank range       /C  Copy current cell     /D  Delete row/column",
		"/E  Edit current      /F  Decimal places       /G  Go to cell",
		"/I  Insert row/col    /L  Load .ksc            /M  Move current cell",
		"/O  Export CSV        /P  Protect cell         /Q  Quit",
		"/R  Replicate range  /S  Save .ksc            /U  Unprotect cell",
		"/Z  Clear worksheet",
		"",
		"Native worksheet files use the KSC1 format. Original CP/M .CAL files",
		"are not accepted. Ctrl-Z or Escape cancels cell entry.",
		"",
		"Press any key to return."
	};
	char line[SCREEN_WIDTH + 1u];
	unsigned int i;

	output_reset();
	output_text("\x1b[2J\x1b[H\x1b[?25l");
	for (i = 0u; i < sizeof(lines) / sizeof(lines[0]); ++i) {
		line_clear(line); line_text(line, 0u, lines[i]); write_line(line, 0u);
	}
	output_send();
	(void)console_flush();
	(void)getchar();
}

static void command_blank(void)
{
	char input[24];
	unsigned char c1 = current_col, r1 = current_row;
	unsigned char c2 = current_col, r2 = current_row;
	int result;

	if (prompt_line("Blank range (Return=current): ", input, sizeof(input)) < 0) { return; }
	if (input[0] != '\0' && !sheet_parse_range(input, &c1, &r1, &c2, &r2)) {
		set_result_message(SC_ERR_RANGE); return;
	}
	result = sheet_blank(c1, r1, c2, r2);
	set_result_message(result);
	if (result == SC_OK) { dirty = 1u; }
}

static void command_copy(unsigned char move)
{
	unsigned char col, row;
	int result;

	if (!ask_ref(move ? "Move to cell: " : "Copy to cell: ", &col, &row)) {
		set_result_message(SC_ERR_RANGE); return;
	}
	result = sheet_copy(current_col, current_row, col, row, move);
	set_result_message(result);
	if (result == SC_OK) { current_col = col; current_row = row; dirty = 1u; }
}

static void command_replicate(void)
{
	char input[24];
	unsigned char c1, r1, c2, r2;
	int result;

	if (prompt_line("Replicate into range: ", input, sizeof(input)) <= 0) { return; }
	if (!sheet_parse_range(input, &c1, &r1, &c2, &r2)) {
		set_result_message(SC_ERR_RANGE);
		return;
	}
	result = sheet_replicate(current_col, current_row, c1, r1, c2, r2);
	set_result_message(result);
	if (result == SC_OK) { dirty = 1u; }
}

static void command_row_col(unsigned char insert)
{
	char answer[4];
	int result;

	if (prompt_line(insert ? "Insert R(ow) or C(olumn)? " :
				   "Delete R(ow) or C(olumn)? ", answer, sizeof(answer)) <= 0) {
		return;
	}
	if (upper((unsigned char)answer[0]) == 'R') {
		result = insert ? sheet_insert_row(current_row) : sheet_delete_row(current_row);
	} else if (upper((unsigned char)answer[0]) == 'C') {
		result = insert ? sheet_insert_col(current_col) : sheet_delete_col(current_col);
	} else {
		result = SC_ERR_RANGE;
	}
	set_result_message(result);
	if (result == SC_OK) { dirty = 1u; }
}

static void command_format(void)
{
	char input[4];
	struct sc_cell *cell = sheet_find(current_col, current_row);

	if (cell == (struct sc_cell *)0) { strlcpy(message, "Blank entry", sizeof(message)); return; }
	if (cell->flags & SC_FLAG_PROTECTED) { set_result_message(SC_ERR_PROTECTED); return; }
	if (prompt_line("Decimal places (0-3): ", input, sizeof(input)) <= 0) { return; }
	if (input[0] < '0' || input[0] > '3' || input[1] != '\0') {
		set_result_message(SC_ERR_RANGE); return;
	}
	cell->precision = (unsigned char)(input[0] - '0');
	dirty = 1u; set_result_message(SC_OK);
}

static void command_goto(void)
{
	unsigned char col, row;

	if (!ask_ref("Enter cell to jump to: ", &col, &row)) {
		set_result_message(SC_ERR_RANGE); return;
	}
	current_col = col; current_row = row; set_result_message(SC_OK);
}

static void command_load(void)
{
	char path[SC_PATH_MAX + 1u];
	int result;

	if (dirty != 0u && !ask_yes("Discard unsaved worksheet? Y/N: ")) { return; }
	if (prompt_line("Load file: ", path, sizeof(path)) <= 0) { return; }
	result = storage_load(path);
	set_result_message(result);
	if (result == SC_OK) {
		strlcpy(work_path, path, sizeof(work_path));
		dirty = 0u; current_col = 0u; current_row = 0u; view_col = 0u; view_row = 0u;
	}
}

static void command_save(void)
{
	char path[SC_PATH_MAX + 1u];
	const char *target = work_path;
	int result;

	if (work_path[0] == '\0') {
		if (prompt_line("Save file (.ksc): ", path, sizeof(path)) <= 0) { return; }
		target = path;
	}
	result = storage_save(target);
	set_result_message(result);
	if (result == SC_OK) { strlcpy(work_path, target, sizeof(work_path)); dirty = 0u; }
}

static void command_export(void)
{
	char path[SC_PATH_MAX + 1u];
	int result;

	if (prompt_line("Export CSV file: ", path, sizeof(path)) <= 0) { return; }
	result = storage_export_csv(path);
	set_result_message(result);
}

static unsigned char slash_command(void)
{
	int ch;
	struct sc_cell *cell;

	strlcpy(message, "Command: B C D E F G I L M O P Q R S U Z ?", sizeof(message));
	redraw();
	ch = upper((unsigned char)read_key());
	message[0] = '\0';
	switch (ch) {
	case 'B': command_blank(); break;
	case 'C': command_copy(0u); break;
	case 'D': command_row_col(0u); break;
	case 'E': edit_cell(-1); break;
	case 'F': command_format(); break;
	case 'G': command_goto(); break;
	case 'I': command_row_col(1u); break;
	case 'L': command_load(); break;
	case 'M': command_copy(1u); break;
	case 'O': command_export(); break;
	case 'P':
		cell = sheet_find(current_col, current_row);
		if (cell != (struct sc_cell *)0) { cell->flags |= SC_FLAG_PROTECTED; dirty = 1u; }
		set_result_message(SC_OK); break;
	case 'Q':
		if (dirty == 0u || ask_yes("EXIT SC2Kos without saving? Y/N: ")) { return 1u; }
		break;
	case 'R': command_replicate(); break;
	case 'S': command_save(); break;
	case 'U':
		cell = sheet_find(current_col, current_row);
		if (cell != (struct sc_cell *)0) { cell->flags &= (unsigned char)~SC_FLAG_PROTECTED; dirty = 1u; }
		set_result_message(SC_OK); break;
	case 'Z':
		if (ask_yes("Zap ENTIRE worksheet? Y/N: ")) {
			sheet_clear(); work_path[0] = '\0'; dirty = 1u;
			current_col = 0u; current_row = 0u; view_col = 0u; view_row = 0u;
			set_result_message(SC_OK);
		}
		break;
	case '?': show_help(); break;
	default: strlcpy(message, "Command aborted", sizeof(message)); break;
	}
	return 0u;
}

int main(int argc, char **argv)
{
	unsigned char running = 1u;

	sheet_clear();
	strlcpy(message, "Ready - type ? for help", sizeof(message));
	if (argc > 0 && argv[0] != (char *)0) {
		int result = storage_load(argv[0]);
		set_result_message(result);
		if (result == SC_OK) { strlcpy(work_path, argv[0], sizeof(work_path)); }
	}
	redraw();
	while (running != 0u) {
		int key;
		unsigned char old_col = current_col;
		unsigned char old_row = current_row;
		unsigned char old_view_col = view_col;
		unsigned char old_view_row = view_row;
		unsigned char refresh = 0u;

		key = read_key();
		message[0] = '\0';
		if (key == KEY_UP) {
			if (current_row != 0u) { --current_row; }
			refresh = 1u;
		} else if (key == KEY_DOWN) {
			if (current_row + 1u < SC_MAX_ROWS) { ++current_row; }
			refresh = 1u;
		} else if (key == KEY_LEFT) {
			if (current_col != 0u) { --current_col; }
			refresh = 1u;
		} else if (key == KEY_RIGHT || key == '\t') {
			if (current_col + 1u < SC_MAX_COLS) { ++current_col; }
			refresh = 1u;
		} else if (key == KEY_HOME) {
			current_col = 0u; refresh = 1u;
		} else if (key == KEY_END) {
			unsigned char c, r;
			sheet_used(&c, &r); current_col = c; refresh = 1u;
		} else if (key == KEY_PGUP) {
			current_row = current_row > GRID_ROWS ?
				(unsigned char)(current_row - GRID_ROWS) : 0u;
			refresh = 1u;
		} else if (key == KEY_PGDN) {
			current_row = (unsigned int)current_row + GRID_ROWS < SC_MAX_ROWS ?
				(unsigned char)(current_row + GRID_ROWS) : SC_MAX_ROWS - 1u;
			refresh = 1u;
		} else if (key == '/') {
			if (slash_command() != 0u) { running = 0u; }
			refresh = 2u;
		} else if (key == '?') {
			show_help(); refresh = 2u;
		} else if (key == '\r' || key == '\n') {
			edit_cell(-1); refresh = 2u;
		} else if (key >= 0x20 && key < 0x7f) {
			edit_cell(key); refresh = 2u;
		}
		if (running != 0u && refresh == 1u) {
			adjust_view();
			if (view_col != old_view_col || view_row != old_view_row) {
				redraw();
			} else {
				redraw_navigation(old_col, old_row);
			}
		} else if (running != 0u && refresh == 2u) {
			redraw();
		}
	}
	console_puts("\x1b[0m\x1b[?25h\x1b[2J\x1b[H");
	console_puts("SC2Kos: worksheet closed.\r\n");
	return 0;
}
