/* The 85-column outcome program's fast-path modes.
 *
 * Compiled into tui_test (the budgeted 85 x 26 program) and left out of
 * the wide-capacity build, which carries only the shift mode from
 * tui_test.c.  tui_modes() dispatches the three modes; it returns the
 * mode's result, or -1 when the argument is none of them.
 */

#include "kos.h"
#include "tui.h"
#include "kos/console_render.h"

int puttext_mode(void);

static int fail(const char *where, int result)
{
	printf("tui: %s FAIL %d\r\n", where, result);
	tui_shutdown();
	return 1;
}

/* pending: a no-change flush emits no span, and after one putc_xy on
 * row 20 only row 20's spans go out. */
static int pending_mode(void)
{
	if (tui_init() < 0) {
		return fail("pending init", 0);
	}
	if (tui_flush() != 0 || tui_flush() != 0) {
		return fail("pending flush", 0);
	}
	if (tui_putc_xy(0, 20, 'P') != 1 || tui_flush() != 0) {
		return fail("pending row", 0);
	}
	printf("tui: pending pass\r\n");
	tui_shutdown();
	return 0;
}

/* timing: one scripted sequence of measured calls; the replay times
 * each entry in order and checks the budgets. */
static int timing_mode(void)
{
	static char row[86];
	int i;
	char ch = 'A';

	for (i = 0; i < 85; ++i) {
		row[i] = ch;
		if (ch == 'Z') {
			ch = 'A';
		} else {
			++ch;
		}
	}
	row[85] = '\0';
	if (tui_init() < 0) {
		return fail("timing init", 0);
	}
	if (tui_puts_xy(0, 0, row) != 85) {
		return fail("timing prefill", 0);
	}
	if (tui_put_text(0, 0, row, 85, 0) != 0) {
		return fail("timing equal", 0);
	}
	if (tui_put_text(0, 1, row, 85, 1) != 0) {
		return fail("timing new", 0);
	}
	if (tui_flush() != 0 || tui_flush() != 0) {
		return fail("timing flush", 0);
	}
	if (tui_putc_xy(0, 10, 'Q') != 1 || tui_flush() != 0) {
		return fail("timing row", 0);
	}
	if (tui_scroll_rows(0u, 24u, TUI_SCROLL_UP) != 0) {
		return fail("timing scroll", 0);
	}
	/* A one-row region: its model moves no block. */
	if (tui_scroll_rows(5u, 1u, TUI_SCROLL_UP) != 0) {
		return fail("timing scroll one", 0);
	}
	printf("tui: timing pass\r\n");
	tui_shutdown();
	return 0;
}

int tui_modes(int argc, char **argv)
{
	if (argc == 1 && strcmp(argv[0], "puttext") == 0) {
		return puttext_mode();
	}
	if (argc == 1 && strcmp(argv[0], "pending") == 0) {
		return pending_mode();
	}
	if (argc == 1 && strcmp(argv[0], "timing") == 0) {
		return timing_mode();
	}
	return -1;
}
