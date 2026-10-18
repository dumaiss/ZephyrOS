/* KonaOS TUI SDK outcome program.
 *
 * Built with `LIBS=tui` as build/user/tui_test.exe and carried on a card by
 * the replay tests:
 *
 *   foundation  lifecycle, geometry, error and transaction rules;
 *   batch       one full dirty row, an unchanged redraw and transparent
 *               cursor synchronization;
 *   screen      deterministic cursor, text, clipping, control-byte,
 *               transaction and formatting rows;
 *   shutdown    dirty cells flushed by tui_shutdown() without an explicit
 *               tui_flush().
 */

#include "kos.h"
#include "tui.h"

/* The formatted row 8's expected text; it also pins the formatter's
 * conversion and width rules. */
static const char format_row[] =
	"str Z -1234 54321 ABCD -123456 123456 BEEF 00042   ab %";
static const char format_row_xy[] = "0012|ok|!|-7";

/* The 90-byte row 2 input: only its first 85 bytes may reach the map. */
static char big_row[90];

static int fail(const char *where, int result)
{
	printf("tui: %s FAIL %d\r\n", where, result);
	tui_shutdown();
	return 1;
}

static int foundation(void)
{
	struct tui_size size;
	int result;

	if (tui_putc('x') != -KOS_EINVAL ||
	    tui_gotoxy(0, 0) != -KOS_EINVAL ||
	    tui_cursor_show() != -KOS_EINVAL ||
	    tui_get_size(&size) != -KOS_EINVAL ||
	    tui_flush() != -KOS_EINVAL ||
	    tui_printf("%d", 1) != -KOS_EINVAL) {
		printf("tui: FAIL before init\r\n");
		return 1;
	}
	result = tui_init();
	if (result < 0) {
		printf("tui: FAIL init %d\r\n", result);
		return 1;
	}
	result = tui_init();
	if (result != -KOS_EINVAL) {
		printf("tui: FAIL reinit %d\r\n", result);
		tui_shutdown();
		return 1;
	}
	result = tui_get_size(&size);
	if (result < 0) {
		printf("tui: FAIL size %d\r\n", result);
		tui_shutdown();
		return 1;
	}
	if (size.cols != 85u || size.rows != 26u) {
		printf("tui: FAIL geometry %u %u\r\n",
		       (unsigned int)size.cols, (unsigned int)size.rows);
		tui_shutdown();
		return 1;
	}
	result = tui_get_size((struct tui_size *)0);
	if (result != -KOS_EINVAL) {
		printf("tui: FAIL null size %d\r\n", result);
		tui_shutdown();
		return 1;
	}
	if (tui_puts((const char *)0) != -KOS_EINVAL ||
	    tui_puts_xy(0, 0, (const char *)0) != -KOS_EINVAL ||
	    tui_write((const char *)0, 1u) != -KOS_EINVAL ||
	    tui_printf((const char *)0) != -KOS_EINVAL ||
	    tui_printf_xy(0, 0, (const char *)0) != -KOS_EINVAL ||
	    tui_write((const char *)0, 0u) != 0) {
		printf("tui: FAIL null argument\r\n");
		tui_shutdown();
		return 1;
	}

	tui_begin();
	tui_begin();
	result = tui_end();
	if (result != 0) {
		printf("tui: FAIL inner end %d\r\n", result);
		tui_shutdown();
		return 1;
	}
	result = tui_end();
	if (result != 0) {
		printf("tui: FAIL outer end %d\r\n", result);
		tui_shutdown();
		return 1;
	}
	result = tui_end();
	if (result != -KOS_EINVAL) {
		printf("tui: FAIL unmatched end %d\r\n", result);
		tui_shutdown();
		return 1;
	}
	result = tui_flush();
	if (result != 0) {
		printf("tui: FAIL flush %d\r\n", result);
		tui_shutdown();
		return 1;
	}

	printf("tui: foundation\r\n");
	tui_shutdown();
	tui_shutdown();
	result = tui_end();
	if (result != -KOS_EINVAL) {
		printf("tui: FAIL end after shutdown %d\r\n", result);
		return 1;
	}
	if (tui_putc('x') != -KOS_EINVAL) {
		printf("tui: FAIL putc after shutdown\r\n");
		return 1;
	}
	result = tui_init();
	if (result != 0) {
		printf("tui: FAIL restart %d\r\n", result);
		return 1;
	}
	tui_shutdown();
	printf("tui: PASS\r\n");
	return 0;
}

static int batch_mode(void)
{
	int i;
	int result;

	if (tui_init() < 0) {
		printf("tui: batch FAIL init\r\n");
		return 1;
	}
	if (tui_gotoxy(0, 0) != 0) {
		return fail("batch gotoxy", 0);
	}
	tui_begin();
	{
		char ch = 'A';

		for (i = 0; i < 85; ++i) {
			result = tui_putc(ch);
			if (result != 1) {
				return fail("batch putc", result);
			}
			++ch;
			if (ch > 'Z') {
				ch = 'A';
			}
		}
	}
	result = tui_end();
	if (result != 0) {
		return fail("batch end", result);
	}
	if (tui_wherex() != 85u || tui_wherey() != 0u) {
		return fail("batch cursor", 0);
	}

	/* Nothing changed: this flush may not emit a cell or a cursor. */
	result = tui_flush();
	if (result != 0) {
		return fail("batch redraw", result);
	}
	if (tui_gotoxy(0, 22) != 0) {
		return fail("batch marker", 0);
	}
	result = tui_flush();
	if (result != 0) {
		return fail("batch marker flush", result);
	}
	printf("tui: batch pass\r\n");
	tui_shutdown();
	return 0;
}

static int screen_mode(void)
{
	struct tui_size size;
	int i;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("tui: screen FAIL init %d\r\n", result);
		return 1;
	}
	result = tui_get_size(&size);
	if (result != 0 || size.cols != 85u || size.rows != 26u) {
		return fail("screen size", result);
	}

	/* Row 0: a whole row from 85 single-byte calls; the cursor ends just
	 * past the right edge. */
	if (tui_gotoxy(0, 0) != 0) {
		return fail("screen row0", 0);
	}
	{
		char ch = 'A';

		for (i = 0; i < 85; ++i) {
			result = tui_putc(ch);
			if (result != 1) {
				return fail("screen row0 putc", result);
			}
			++ch;
			if (ch > 'Z') {
				ch = 'A';
			}
		}
	}
	if (tui_wherex() != 85u || tui_wherey() != 0u) {
		return fail("screen row0 cursor", 0);
	}

	/* Row 1: another full row. */
	if (tui_gotoxy(0, 1) != 0) {
		return fail("screen row1", 0);
	}
	for (i = 0; i < 85; ++i) {
		if (tui_putc('#') != 1) {
			return fail("screen row1 putc", 0);
		}
	}

	/* Row 2: 90 input bytes, clipped after 85. */
	{
		char ch = '0';

		for (i = 0; i < 90; ++i) {
			big_row[i] = ch;
			++ch;
			if (ch > '9') {
				ch = '0';
			}
		}
	}
	if (tui_gotoxy(0, 2) != 0) {
		return fail("screen row2", 0);
	}
	result = tui_write(big_row, 90u);
	if (result != 90) {
		return fail("screen row2 write", result);
	}

	/* Row 3: _xy writes clip at the right edge and leave the cursor
	 * alone. */
	if (tui_gotoxy(0, 20) != 0) {
		return fail("screen row3 cursor", 0);
	}
	result = tui_puts_xy(83, 3, "XYZ");
	if (result != 3) {
		return fail("screen row3 puts_xy", result);
	}
	if (tui_wherex() != 0u || tui_wherey() != 20u) {
		return fail("screen row3 cursor kept", 0);
	}

	/* Rows 4-5: TAB, CR and LF move the logical cursor. */
	if (tui_gotoxy(10, 4) != 0) {
		return fail("screen row4", 0);
	}
	result = tui_puts("AB\tC\rD\nE");
	if (result != 8) {
		return fail("screen row4 puts", result);
	}

	/* Rows 6-7: _xy placement does not disturb the cursor; X and Y follow
	 * the cursor set before it. */
	if (tui_gotoxy(0, 7) != 0) {
		return fail("screen row7", 0);
	}
	result = tui_puts_xy(0, 6, "ABCDE");
	if (result != 5) {
		return fail("screen row6 puts_xy", result);
	}
	if (tui_wherex() != 0u || tui_wherey() != 7u) {
		return fail("screen row6 cursor kept", 0);
	}
	if (tui_putc('X') != 1) {
		return fail("screen row7 X", 0);
	}
	if (tui_putc_xy(10, 7, 'P') != 1) {
		return fail("screen row7 P", 0);
	}
	if (tui_wherex() != 1u || tui_wherey() != 7u) {
		return fail("screen row7 cursor kept", 0);
	}
	if (tui_putc('Y') != 1) {
		return fail("screen row7 Y", 0);
	}

	/* Row 8: the formatter's complete conversion set. */
	if (tui_gotoxy(0, 8) != 0) {
		return fail("screen row8", 0);
	}
	result = tui_printf("%s %c %d %u %x %ld %lu %lx %05d %4s %%",
			    "str", 'Z', -1234, 54321u, 0xABCDu,
			    -123456L, 123456UL, 0xBEEFUL, 42, "ab");
	if (result != (int)strlen(format_row)) {
		return fail("screen row8 printf", result);
	}

	/* Row 9: positioned formatting leaves the cursor at row 10, where Q
	 * follows. */
	if (tui_gotoxy(0, 10) != 0) {
		return fail("screen row10", 0);
	}
	result = tui_printf_xy(0, 9, "%04x|%s|%c|%d", 0x12u, "ok", '!', -7);
	if (result != (int)strlen(format_row_xy)) {
		return fail("screen row9 printf_xy", result);
	}
	if (tui_wherex() != 0u || tui_wherey() != 10u) {
		return fail("screen row9 cursor kept", 0);
	}
	if (tui_putc('Q') != 1) {
		return fail("screen row10 Q", 0);
	}

	/* Rows 11-12: nested transactions flush only at the outermost end. */
	tui_begin();
	if (tui_gotoxy(0, 11) != 0 || tui_puts("inner1") != 6) {
		return fail("screen row11", 0);
	}
	tui_begin();
	if (tui_gotoxy(0, 12) != 0 || tui_puts("inner2") != 6) {
		return fail("screen row12", 0);
	}
	if (tui_end() != 0) {
		return fail("screen inner end", 0);
	}
	if (tui_end() != 0) {
		return fail("screen outer end", 0);
	}

	/* Row 13: an explicit flush inside a transaction still writes. */
	tui_begin();
	if (tui_gotoxy(0, 13) != 0 || tui_puts("explicit") != 8) {
		return fail("screen row13", 0);
	}
	if (tui_flush() != 0) {
		return fail("screen explicit flush", 0);
	}
	if (tui_end() != 0) {
		return fail("screen explicit end", 0);
	}

	/* Row 14: an invalid cursor position leaves the cursor alone. */
	if (tui_gotoxy(0, 14) != 0) {
		return fail("screen row14", 0);
	}
	if (tui_gotoxy(85, 0) != -KOS_EINVAL) {
		return fail("screen invalid gotoxy", 0);
	}
	if (tui_putc('Z') != 1) {
		return fail("screen row14 Z", 0);
	}

	/* Rows 15-17: BS clamps at the left edge, TAB clamps at the right,
	 * and other C0 bytes are ignored. */
	if (tui_gotoxy(0, 15) != 0 || tui_putc(0x08) != 1 ||
	    tui_putc('B') != 1) {
		return fail("screen BS", 0);
	}
	if (tui_gotoxy(80, 16) != 0 || tui_putc(0x09) != 1 ||
	    tui_putc('T') != 1) {
		return fail("screen TAB", 0);
	}
	if (tui_gotoxy(2, 17) != 0 || tui_putc(0x01) != 1 ||
	    tui_putc('I') != 1) {
		return fail("screen C0", 0);
	}

	/* Row 25: LF clamps at the bottom, and text at the right edge cannot
	 * alter another row or scroll. */
	if (tui_gotoxy(0, 25) != 0 || tui_putc(0x0A) != 1 ||
	    tui_putc('W') != 1) {
		return fail("screen bottom LF", 0);
	}
	if (tui_gotoxy(83, 25) != 0) {
		return fail("screen bottom cursor", 0);
	}
	if (tui_puts("XYZ") != 3) {
		return fail("screen bottom puts", 0);
	}
	if (tui_wherex() != 85u || tui_wherey() != 25u) {
		return fail("screen bottom cursor end", 0);
	}

	/* The visibility state is part of the flush synchronization. */
	if (tui_cursor_hide() != 0) {
		return fail("screen hide", 0);
	}
	if (tui_flush() != 0) {
		return fail("screen hide flush", 0);
	}
	if (tui_cursor_show() != 0) {
		return fail("screen show", 0);
	}

	if (tui_gotoxy(0, 22) != 0) {
		return fail("screen marker", 0);
	}
	if (tui_flush() != 0) {
		return fail("screen final flush", 0);
	}
	printf("tui: screen pass\r\n");
	tui_shutdown();
	return 0;
}

static int shutdown_mode(void)
{
	char ch = 'A';
	int i;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("tui: shutdown FAIL init %d\r\n", result);
		return 1;
	}
	if (tui_gotoxy(0, 0) != 0) {
		return fail("shutdown row0", 0);
	}
	for (i = 0; i < 85; ++i) {
		result = tui_putc(ch);
		if (result != 1) {
			return fail("shutdown row0 putc", result);
		}
		++ch;
		if (ch > 'Z') {
			ch = 'A';
		}
	}
	if (tui_gotoxy(0, 1) != 0) {
		return fail("shutdown row1", 0);
	}
	if (tui_puts("dirty two") != 9) {
		return fail("shutdown row1 puts", 0);
	}
	if (tui_puts_xy(5, 2, "xy") != 2) {
		return fail("shutdown row2 puts_xy", 0);
	}
	if (tui_gotoxy(0, 22) != 0) {
		return fail("shutdown marker", 0);
	}

	/* No explicit flush: shutdown must emit the dirty rows, then restore
	 * the terminal and release the library. */
	tui_shutdown();
	tui_shutdown();
	if (tui_putc('x') != -KOS_EINVAL) {
		return fail("shutdown released", 0);
	}
	printf("tui: shutdown pass\r\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 0 || (argc == 1 && strcmp(argv[0], "foundation") == 0)) {
		return foundation();
	}
	if (argc == 1 && strcmp(argv[0], "batch") == 0) {
		return batch_mode();
	}
	if (argc == 1 && strcmp(argv[0], "screen") == 0) {
		return screen_mode();
	}
	if (argc == 1 && strcmp(argv[0], "shutdown") == 0) {
		return shutdown_mode();
	}
	printf("tui: FAIL mode\r\n");
	return 1;
}
