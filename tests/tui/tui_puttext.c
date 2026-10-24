/* The tui_put_text equivalence mode, linked into both outcome programs
 * (the 85 x 26 tui_test and the 128 x 35 tui_wide), so the whole cell
 * map of each capacity is exercised.
 */

#include "kos.h"
#include "tui.h"

static int fail(const char *where, int result)
{
	printf("tui: %s FAIL %d\r\n", where, result);
	tui_shutdown();
	return 1;
}

/* Full-width rows across the whole grid: the last two rows and the
 * middle one, each drawn whole in attribute 0 and then half of it again
 * in attribute 1 (an attribute-only change).  `put` selects the path:
 * tui_put_text, or tui_puts_xy with tui_set_attr. */
static char puttext_row[129];

static int puttext_rows(uint8_t put)
{
	struct tui_size size;
	uint8_t rows[3];
	uint8_t i;
	uint8_t half;

	if (tui_reset_window() != 0 || tui_get_size(&size) != 0) {
		return -1;
	}
	{
		char ch = 'a';

		for (i = 0u; i < size.cols; ++i) {
			puttext_row[i] = ch;
			ch = ch == 'z' ? 'a' : (char)(ch + 1);
		}
	}
	puttext_row[size.cols] = '\0';
	half = (uint8_t)(size.cols >> 1);
	rows[0] = (uint8_t)(size.rows - 1u);
	rows[1] = (uint8_t)(size.rows - 2u);
	rows[2] = (uint8_t)(size.rows >> 1);
	for (i = 0u; i < 3u; ++i) {
		if (put) {
			if (tui_put_text(0, rows[i], puttext_row, size.cols, 0) != 0 ||
			    tui_put_text(0, rows[i], puttext_row, half, 1) != 0) {
				return -1;
			}
		} else {
			if (tui_set_attr(0) != 0 ||
			    tui_puts_xy(0, rows[i], puttext_row) != size.cols) {
				return -1;
			}
			{
				char saved = puttext_row[half];

				puttext_row[half] = '\0';
				if (tui_set_attr(1) != 0 ||
				    tui_puts_xy(0, rows[i], puttext_row) != half) {
					return -1;
				}
				puttext_row[half] = saved;
			}
		}
	}
	return 0;
}

/* puttext: the same screen drawn once with tui_put_text and once with
 * tui_puts_xy, over mixed attributes, already-equal cells, an
 * attribute-only change, a row clipped by a window and full-width rows
 * down to the grid's last row.  The replay compares the two flushes'
 * render streams byte for byte. */
int puttext_mode(void)
{
	if (tui_init() < 0) {
		printf("tui: puttext FAIL init\r\n");
		return 1;
	}
	if (tui_window(2, 2, 30, 8) != 0) {
		return fail("puttext window", 0);
	}
	if (tui_put_text(0, 0, "HELLO", 5, 0) != 0 ||
	    tui_put_text(3, 1, "ROW-ONE", 7, 1) != 0 ||
	    tui_put_text(25, 2, "CLIPPED-AWAY", 12, 0) != 0 ||
	    tui_put_text(3, 1, "ROW-ONE", 7, 0) != 0 ||
	    tui_put_text(0, 0, "HELLO", 5, 0) != 0) {
		return fail("puttext draw a", 0);
	}
	if (puttext_rows(1u) != 0) {
		return fail("puttext rows a", 0);
	}
	if (tui_set_attr(1) != 0 || tui_flush() != 0) {
		return fail("puttext flush a", 0);
	}
	printf("tui: puttext phase A\r\n");
	tui_shutdown();
	if (tui_init() < 0 || tui_window(2, 2, 30, 8) != 0) {
		return fail("puttext restart", 0);
	}
	if (tui_set_attr(0) != 0 || tui_puts_xy(0, 0, "HELLO") != 5 ||
	    tui_set_attr(1) != 0 || tui_puts_xy(3, 1, "ROW-ONE") != 7 ||
	    tui_set_attr(0) != 0 || tui_puts_xy(25, 2, "CLIPPED-AWAY") != 12 ||
	    tui_set_attr(0) != 0 || tui_puts_xy(3, 1, "ROW-ONE") != 7 ||
	    tui_set_attr(0) != 0 || tui_puts_xy(0, 0, "HELLO") != 5) {
		return fail("puttext draw b", 0);
	}
	if (puttext_rows(0u) != 0) {
		return fail("puttext rows b", 0);
	}
	if (tui_set_attr(1) != 0 || tui_flush() != 0) {
		return fail("puttext flush b", 0);
	}
	printf("tui: puttext phase B\r\n");
	tui_shutdown();
	printf("tui: puttext pass\r\n");
	return 0;
}

