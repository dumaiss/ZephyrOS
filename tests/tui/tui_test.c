/* KonaOS TUI SDK outcome program.
 *
 * Built with `LIBS=tui` as build/user/tui_test.exe and carried on a card by
 * the replay tests:
 *
 *   foundation  lifecycle, geometry, error and transaction rules;
 *   batch       one full dirty row, an unchanged redraw and transparent
 *               cursor synchronization;
 *   invert      a normal/reverse-only text transition in both directions;
 *   invertspan  two separate attribute-only spans in one render request;
 *   renderboundary  a record header crossing the 64-byte staging chunk;
 *   renderrows  a two-row flush split into two render requests;
 *   renderbad   malformed streams rejected before any backend work;
 *   rendermax   the maximum legal render stream length;
 *   invertwrap  31 scrolls put the bitmap origin on scanline 248; an
 *               INVERT on logical row zero runs the circular-Y split;
 *   rendersync  one flush with TEXT, INVERT and a cursor-moving STATE:
 *               one SAT write and one final command wait;
 *   renderflip  RENDER succeeds, the tee arms, the next flush falls
 *               back to a full ANSI repaint;
 *   screen      deterministic cursor, text, clipping, control-byte,
 *               transaction and formatting rows;
 *   shutdown    dirty cells flushed by tui_shutdown() without an explicit
 *               tui_flush();
 *   windows     window stack, clipping, clear/line operations;
 *   keys        key-decoder vocabulary, replay and standalone ESC;
 *   styles      semantic styles, frames, fills and degenerate rectangles;
 *   region      caller-buffered save/restore over mixed content;
 *   widgets     a configuration form driven through menu, input, list,
 *               buttons and modal dialogs.
 */

#include "kos.h"
#include "tui.h"
#include "tui_widgets.h"
#include "kos/console_render.h"

/* The formatted row 8's expected text; it also pins the formatter's
 * conversion and width rules. */
static const char format_row[] =
	"str Z -1234 54321 ABCD -123456 123456 BEEF 00042   ab %";
static const char format_row_xy[] = "0012|ok|!|-7";

/* The 90-byte row 2 input: only its first 85 bytes may reach the map. */
static char big_row[90];

/* Raw render-transaction checks and the two-row split/boundary cases. */
static uint8_t render_stream[KOS_IPC_DESC_MAX];
static char render_row[86];

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
	    tui_key_pending() != -KOS_EINVAL ||
	    tui_pollkey() != TUI_KEY_NONE ||
	    tui_getkey() != TUI_KEY_NONE ||
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
	if (tui_putc('x') != -KOS_EINVAL ||
	    tui_key_pending() != -KOS_EINVAL ||
	    tui_pollkey() != TUI_KEY_NONE ||
	    tui_getkey() != TUI_KEY_NONE) {
		printf("tui: FAIL released API after shutdown\r\n");
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

static int invert_mode(void)
{
	if (tui_init() < 0) {
		printf("tui: invert FAIL init\r\n");
		return 1;
	}
	if (tui_puts_xy(4, 3, "MENU") != 4 || tui_flush() != 0) {
		return fail("invert text", 0);
	}
	if (tui_set_attr(1u) != 0 ||
	    tui_puts_xy(4, 3, "MENU") != 4 || tui_flush() != 0) {
		return fail("invert on", 0);
	}
	if (tui_set_attr(0u) != 0 ||
	    tui_puts_xy(4, 3, "MENU") != 4 || tui_flush() != 0) {
		return fail("invert off", 0);
	}
	if (tui_gotoxy(0, 22) != 0 || tui_flush() != 0) {
		return fail("invert marker", 0);
	}
	printf("tui: invert pass\r\n");
	tui_shutdown();
	return 0;
}

static int invertspan_mode(void)
{
	if (tui_init() < 0) {
		printf("tui: invertspan FAIL init\r\n");
		return 1;
	}
	if (tui_puts_xy(4, 3, "OLD") != 3 ||
	    tui_puts_xy(20, 3, "NEW") != 3 || tui_flush() != 0) {
		return fail("invertspan text", 0);
	}
	if (tui_set_attr(1u) != 0 ||
	    tui_puts_xy(4, 3, "OLD") != 3 ||
	    tui_puts_xy(20, 3, "NEW") != 3 || tui_flush() != 0) {
		return fail("invertspan on", 0);
	}
	if (tui_set_attr(0u) != 0 ||
	    tui_puts_xy(4, 3, "OLD") != 3 ||
	    tui_puts_xy(20, 3, "NEW") != 3 || tui_flush() != 0) {
		return fail("invertspan off", 0);
	}
	if (tui_gotoxy(0, 22) != 0 || tui_flush() != 0) {
		return fail("invertspan marker", 0);
	}
	printf("tui: invertspan pass\r\n");
	tui_shutdown();
	return 0;
}

static int renderboundary_mode(void)
{
	uint8_t i;
	char ch = 'A';

	for (i = 0u; i < 85u; ++i) {
		render_row[i] = ch;
		if (ch == 'Z') {
			ch = 'A';
		} else {
			++ch;
		}
	}
	render_row[85] = '\0';
	render_row[52] = '\0';
	if (tui_init() < 0) {
		printf("tui: renderboundary FAIL init\r\n");
		return 1;
	}
	if (tui_puts_xy(0, 0, render_row) != 52 ||
	    tui_putc_xy(0, 1, 'Z') != 1 || tui_flush() != 0) {
		return fail("renderboundary", 0);
	}
	if (tui_gotoxy(0, 22) != 0 || tui_flush() != 0) {
		return fail("renderboundary marker", 0);
	}
	printf("tui: renderboundary pass\r\n");
	tui_shutdown();
	return 0;
}

static int renderrows_mode(void)
{
	uint8_t i;
	char ch = 'A';

	for (i = 0u; i < 85u; ++i) {
		render_row[i] = ch;
		if (ch == 'Z') {
			ch = 'A';
		} else {
			++ch;
		}
	}
	render_row[85] = '\0';
	if (tui_init() < 0) {
		printf("tui: renderrows FAIL init\r\n");
		return 1;
	}
	if (tui_puts_xy(0, 0, render_row) != 85 ||
	    tui_puts_xy(0, 1, render_row) != 85 || tui_flush() != 0) {
		return fail("renderrows", 0);
	}
	if (tui_gotoxy(0, 22) != 0 || tui_flush() != 0) {
		return fail("renderrows marker", 0);
	}
	printf("tui: renderrows pass\r\n");
	tui_shutdown();
	return 0;
}

static uint16_t render_add_span(uint16_t at, uint8_t count, uint8_t ch)
{
	uint8_t i;

	render_stream[at++] = KCONSOLE_RENDER_TEXT;
	render_stream[at++] = (uint8_t)(KCONSOLE_RENDER_SPAN_BASE + count);
	render_stream[at++] = 0u;
	render_stream[at++] = 0u;
	render_stream[at++] = 0u;
	render_stream[at++] = count;
	for (i = 0u; i < count; ++i) {
		render_stream[at++] = ch;
	}
	return at;
}

static int renderbad_mode(void)
{
	static const uint8_t bad_version[] = {
		2u, 0u, KCONSOLE_RENDER_STATE_SIZE + 4u, 0u,
		KCONSOLE_RENDER_STATE, KCONSOLE_RENDER_STATE_SIZE,
		0u, 0u, 0u, 1u
	};
	static const uint8_t bad_flags[] = {
		KCONSOLE_RENDER_VERSION, 1u, KCONSOLE_RENDER_STATE_SIZE + 4u, 0u,
		KCONSOLE_RENDER_STATE, KCONSOLE_RENDER_STATE_SIZE,
		0u, 0u, 0u, 1u
	};
	static const uint8_t bad_attr[] = {
		KCONSOLE_RENDER_VERSION, 0u, KCONSOLE_RENDER_STATE_SIZE + 4u, 0u,
		KCONSOLE_RENDER_STATE, KCONSOLE_RENDER_STATE_SIZE,
		0u, 0u, 2u, 1u
	};
	static const uint8_t bad_x[] = {
		KCONSOLE_RENDER_VERSION, 0u, KCONSOLE_RENDER_STATE_SIZE + 4u, 0u,
		KCONSOLE_RENDER_STATE, KCONSOLE_RENDER_STATE_SIZE,
		85u, 0u, 0u, 1u
	};
	static const uint8_t bad_op[] = {
		KCONSOLE_RENDER_VERSION, 0u, KCONSOLE_RENDER_STATE_SIZE + 4u, 0u,
		99u, KCONSOLE_RENDER_STATE_SIZE, 0u, 0u, 0u, 1u
	};
	static const uint8_t bad_count[] = {
		KCONSOLE_RENDER_VERSION, 0u, KCONSOLE_RENDER_SPAN_BASE + 4u, 0u,
		KCONSOLE_RENDER_TEXT, KCONSOLE_RENDER_SPAN_BASE,
		0u, 0u, 0u, 0u
	};
	static const uint8_t header_only[] = {
		KCONSOLE_RENDER_VERSION, 0u, 4u, 0u
	};
	static const uint8_t trailing[] = {
		KCONSOLE_RENDER_VERSION, 0u, KCONSOLE_RENDER_STATE_SIZE + 5u, 0u,
		KCONSOLE_RENDER_STATE, KCONSOLE_RENDER_STATE_SIZE,
		0u, 0u, 0u, 1u, 0u
	};
	const uint8_t *bad[] = {
		bad_version, bad_flags, bad_attr, bad_x, bad_op, bad_count,
		header_only, trailing
	};
	const uint8_t lengths[] = {
		(uint8_t)sizeof(bad_version), (uint8_t)sizeof(bad_flags),
		(uint8_t)sizeof(bad_attr), (uint8_t)sizeof(bad_x),
		(uint8_t)sizeof(bad_op), (uint8_t)sizeof(bad_count),
		(uint8_t)sizeof(header_only), (uint8_t)sizeof(trailing)
	};
	uint16_t i;
	int result;
	int serial;

	if (tui_init() < 0) {
		printf("tui: renderbad FAIL init\r\n");
		return 1;
	}
	result = console_render(bad[0], lengths[0]);
	serial = result == -KOS_ENOTSUP;
	if (serial == 0 && result != -KOS_EINVAL) {
		return fail("renderbad version", result);
	}
	for (i = 0u; i < (uint16_t)(sizeof(bad) / sizeof(bad[0])); ++i) {
		result = console_render(bad[i], lengths[i]);
		if (serial != 0) {
			if (result != -KOS_ENOTSUP) {
				return fail("renderbad serial reject", result);
			}
		} else if (result != -KOS_EINVAL) {
			return fail("renderbad malformed", result);
		}
	}
	if (tui_gotoxy(0, 22) != 0 || tui_flush() != 0) {
		return fail("renderbad marker", 0);
	}
	printf("tui: renderbad pass\r\n");
	tui_shutdown();
	return 0;
}

static int rendermax_mode(void)
{
	uint16_t at;
	uint16_t i;
	int result;

	if (tui_init() < 0) {
		printf("tui: rendermax FAIL init\r\n");
		return 1;
	}
	/* 582 one-cell TEXT records and one six-cell TEXT record fill a
	 * 4096-byte stream exactly; STATE is the final six bytes. */
	render_stream[0] = KCONSOLE_RENDER_VERSION;
	render_stream[1] = 0u;
	render_stream[2] = 0u;
	render_stream[3] = 0x10u;
	at = KCONSOLE_RENDER_HEADER_SIZE;
	for (i = 0u; i < 582u; ++i) {
		at = render_add_span(at, 1u, 'A');
	}
	at = render_add_span(at, 6u, 'B');
	render_stream[at++] = KCONSOLE_RENDER_STATE;
	render_stream[at++] = KCONSOLE_RENDER_STATE_SIZE;
	render_stream[at++] = 0u;
	render_stream[at++] = 0u;
	render_stream[at++] = 0u;
	render_stream[at++] = 1u;
	if (at != KOS_IPC_DESC_MAX) {
		return fail("rendermax length", at);
	}
	result = console_render(render_stream, at);
	if (result != -KOS_ENOTSUP && result != (int)at) {
		return fail("rendermax", result);
	}
	printf("tui: rendermax pass\r\n");
	tui_shutdown();
	return 0;
}

/* The circular-bitmap boundary: 31 single-line scrolls put the bitmap
 * origin on scanline 248, so logical row zero occupies physical
 * scanlines 248..255 and an INVERT there runs the split path.  The
 * scrolls go through the ordinary console WRITE path (a render record
 * never advances the cursor at the last column).  tui_init homes the
 * cursor to row zero, so 56 newlines walk to the bottom row in 25 and
 * scroll 31 times. */
static int invertwrap_mode(void)
{
	static const char scroll_lfs[] =
		"\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"
		"\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n";

	if (tui_init() < 0) {
		printf("tui: invertwrap FAIL init\r\n");
		return 1;
	}
	console_write(scroll_lfs, sizeof(scroll_lfs) - 1u);
	if (tui_puts_xy(4, 0, "WRAP") != 4 || tui_flush() != 0) {
		return fail("invertwrap text", 0);
	}
	if (tui_set_attr(1u) != 0 ||
	    tui_puts_xy(4, 0, "WRAP") != 4 || tui_flush() != 0) {
		return fail("invertwrap on", 0);
	}
	if (tui_set_attr(0u) != 0 ||
	    tui_puts_xy(4, 0, "WRAP") != 4 || tui_flush() != 0) {
		return fail("invertwrap off", 0);
	}
	if (tui_gotoxy(0, 22) != 0 || tui_flush() != 0) {
		return fail("invertwrap marker", 0);
	}
	printf("tui: invertwrap pass\r\n");
	tui_shutdown();
	return 0;
}

/* One flush with three records - TEXT, INVERT and a STATE whose cursor
 * moves - so the replay can pin the transaction boundary: one SAT write
 * and one final command wait. */
static int rendersync_mode(void)
{
	if (tui_init() < 0) {
		printf("tui: rendersync FAIL init\r\n");
		return 1;
	}
	if (tui_puts_xy(0, 0, "AB") != 2 ||
	    tui_puts_xy(5, 0, "CD") != 2 || tui_flush() != 0) {
		return fail("rendersync text", 0);
	}
	if (tui_set_attr(1u) != 0 ||
	    tui_puts_xy(5, 0, "CD") != 2 ||
	    tui_puts_xy(0, 0, "XY") != 2 ||
	    tui_gotoxy(10, 5) != 0 || tui_flush() != 0) {
		return fail("rendersync move", 0);
	}
	if (tui_gotoxy(0, 22) != 0 || tui_flush() != 0) {
		return fail("rendersync marker", 0);
	}
	printf("tui: rendersync pass\r\n");
	tui_shutdown();
	return 0;
}

/* The RENDER-to-ANSI fallback after a successful RENDER: the program
 * draws through RENDER (support 1), waits on one key while the replay
 * arms the serial tee, dirties one cell and flushes.  The console now
 * answers -KOS_ENOTSUP, so libtui repaints the whole screen through
 * ANSI and the flush returns that path's result. */
static int renderflip_mode(void)
{
	char ch;

	if (tui_init() < 0) {
		printf("tui: renderflip FAIL init\r\n");
		return 1;
	}
	if (tui_puts_xy(0, 0, "FLIP") != 4 ||
	    tui_puts_xy(0, 20, "TAIL") != 4 || tui_flush() != 0) {
		return fail("renderflip draw", 0);
	}
	for (;;) {
		int result = console_read(&ch, 1);

		if (result == 1) {
			break;
		}
		idle();
	}
	if (tui_puts_xy(4, 1, "Z") != 1 || tui_flush() != 0) {
		return fail("renderflip flush", 0);
	}
	printf("tui: renderflip pass\r\n");
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


static int windows_mode(void)
{
	struct tui_rect rect;
	int result;
	int i;

	result = tui_init();
	if (result < 0) {
		printf("tui: windows FAIL init %d\r\n", result);
		return 1;
	}
	if (tui_puts_xy(0, 0, "OUT") != 3 ||
	    tui_puts_xy(0, 1, "S1") != 2 ||
	    tui_puts_xy(0, 7, "S7") != 2 ||
	    tui_puts_xy(12, 2, "R2") != 2) {
		return fail("windows sentinels", 0);
	}
	if (tui_window(2, 2, 10, 6) != 0 ||
	    tui_wherex() != 0u || tui_wherey() != 0u) {
		return fail("windows install", 0);
	}
	if (tui_puts("ABCDEFGHIJ") != 10 ||
	    tui_gotoxy(0, 1) != 0 ||
	    tui_puts("KLMNOPQRST") != 10 ||
	    tui_gotoxy(0, 2) != 0 ||
	    tui_puts("UVWXYZ0123") != 10) {
		return fail("windows rows", 0);
	}
	if (tui_gotoxy(10, 0) != -KOS_EINVAL) {
		return fail("windows gotoxy clip", 0);
	}
	if (tui_puts_xy(8, 1, "XYZ") != 3) {
		return fail("windows puts_xy", 0);
	}
	if (tui_wherex() != 10u || tui_wherey() != 2u ||
	    tui_clreol() != 0) {
		return fail("windows clreol", 0);
	}
	if (tui_gotoxy(0, 3) != 0 || tui_clrline() != 0) {
		return fail("windows clrline", 0);
	}
	if (tui_gotoxy(3, 1) != 0 || tui_insert_line() != 0) {
		return fail("windows insert", 0);
	}
	if (tui_gotoxy(3, 1) != 0 || tui_delete_line() != 0) {
		return fail("windows delete", 0);
	}
	if (tui_push_window((const struct tui_rect *)0) != -KOS_EINVAL) {
		return fail("windows null push", 0);
	}
	rect.x = 4u;
	rect.y = 4u;
	rect.w = 5u;
	rect.h = 3u;
	if (tui_push_window(&rect) != 0 ||
	    tui_wherex() != 0u || tui_wherey() != 0u) {
		return fail("windows push", 0);
	}
	if (tui_puts("IN") != 2 || tui_pop_window() != 0 ||
	    tui_wherex() != 0u || tui_wherey() != 0u) {
		return fail("windows pop", 0);
	}
	for (i = 0; i < 4; ++i) {
		if (tui_push_window(&rect) != 0) {
			return fail("windows depth", 0);
		}
	}
	if (tui_push_window(&rect) != -KOS_EINVAL) {
		return fail("windows overflow", 0);
	}
	for (i = 0; i < 4; ++i) {
		if (tui_pop_window() != 0) {
			return fail("windows unwind", 0);
		}
	}
	if (tui_pop_window() != -KOS_EINVAL) {
		return fail("windows underflow", 0);
	}
	if (tui_wherex() != 0u || tui_wherey() != 0u) {
		return fail("windows restored cursor", 0);
	}
	if (tui_gotoxy(3, 3) != 0) {
		return fail("windows cursor", 0);
	}
	if (tui_window(80, 2, 10, 6) != -KOS_EINVAL ||
	    tui_window(2, 2, 0, 6) != -KOS_EINVAL) {
		return fail("windows invalid", 0);
	}
	if (tui_wherex() != 3u || tui_wherey() != 3u) {
		return fail("windows invalid kept", 0);
	}
	if (tui_reset_window() != 0 || tui_wherex() != 0u ||
	    tui_wherey() != 0u) {
		return fail("windows reset", 0);
	}
	/* Clear a separate window: the main window keeps the insert/delete
	 * result for the replay model. */
	if (tui_window(12, 8, 8, 4) != 0 ||
	    tui_puts("CLRME") != 5 ||
	    tui_clear() != 0 ||
	    tui_wherex() != 0u || tui_wherey() != 0u) {
		return fail("windows clear", 0);
	}
	if (tui_reset_window() != 0 || tui_wherex() != 0u ||
	    tui_wherey() != 0u) {
		return fail("windows reset two", 0);
	}
	if (tui_puts("END") != 3) {
		return fail("windows end", 0);
	}
	if (tui_gotoxy(0, 22) != 0 || tui_flush() != 0) {
		return fail("windows flush", 0);
	}
	printf("tui: windows pass\r\n");
	tui_shutdown();
	return 0;
}

/* The expected transcript of keys_mode: ordinary and decoded keys, then
 * the three malformed sequences and the standalone ESC with its tail. */
static const tui_key_t key_expected[] = {
	0x0041u,
	TUI_KEY_UP, TUI_KEY_DOWN, TUI_KEY_RIGHT, TUI_KEY_LEFT,
	TUI_KEY_HOME, TUI_KEY_END,
	TUI_KEY_INSERT, TUI_KEY_DELETE, TUI_KEY_PGUP, TUI_KEY_PGDN,
	TUI_KEY_F1, TUI_KEY_F2, TUI_KEY_F3, TUI_KEY_F4,
	TUI_KEY_F5, TUI_KEY_F6, TUI_KEY_F7, TUI_KEY_F8,
	TUI_KEY_F9, TUI_KEY_F10, TUI_KEY_F11, TUI_KEY_F12,
	TUI_KEY_ESC, 0x005Bu, 0x005Au,
	TUI_KEY_ESC, 0x005Bu, 0x0031u, 0x0041u,
	TUI_KEY_ESC, 0x005Bu, 0x0031u, 0x0032u, 0x0033u, 0x0034u,
	0x0035u, 0x0036u, 0x0037u, 0x0038u, 0x007Eu, 0x0059u,
	TUI_KEY_ESC, 0x004Fu, 0x0058u,
	TUI_KEY_ESC, 0x0058u
};

static int keys_mode(void)
{
	tui_key_t key;
	uint16_t i;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("tui: keys FAIL init %d\r\n", result);
		return 1;
	}
	for (i = 0u; i < (uint16_t)(sizeof(key_expected) /
				    sizeof(key_expected[0])); ++i) {
		key = tui_getkey();
		if (key != key_expected[i]) {
			printf("tui: keys FAIL %u %04x\r\n",
			       (unsigned int)i, (unsigned int)key);
			tui_shutdown();
			return 1;
		}
		printf("%04x ", (unsigned int)key);
		if ((uint8_t)(i & 7u) == 7u) {
			printf("\r\n");
		}
	}
	if (tui_pollkey() != TUI_KEY_NONE || tui_key_pending() != 0) {
		printf("\r\ntui: keys FAIL extra\r\n");
		tui_shutdown();
		return 1;
	}
	printf("\r\ntui: keys pass\r\n");
	tui_shutdown();
	return 0;
}

static int styles_mode(void)
{
	struct tui_rect rect;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("tui: styles FAIL init %d\r\n", result);
		return 1;
	}
	if (tui_set_style((tui_style_t)TUI_STYLE_COUNT) != -KOS_EINVAL ||
	    tui_get_style() != TUI_STYLE_NORMAL) {
		return fail("styles invalid style", 0);
	}
	if (tui_set_attr(2u) != -KOS_EINVAL || tui_get_attr() != 0u) {
		return fail("styles invalid attr", 0);
	}

	rect.x = 2u;
	rect.y = 1u;
	rect.w = 20u;
	rect.h = 7u;
	if (tui_frame_title(&rect, TUI_FRAME_SINGLE, TUI_STYLE_TITLE,
			    "Single") != 0) {
		return fail("styles single", 0);
	}
	rect.x = 30u;
	if (tui_frame_title(&rect, TUI_FRAME_DOUBLE, TUI_STYLE_TITLE,
			    "Double") != 0) {
		return fail("styles double", 0);
	}
	rect.x = 60u;
	rect.w = 10u;
	rect.h = 5u;
	if (tui_frame_title(&rect, TUI_FRAME_NONE, TUI_STYLE_TITLE,
			    "LongTitle") != 0) {
		return fail("styles none", 0);
	}
	if (tui_frame_title(&rect, (tui_frame_style_t)3,
			    TUI_STYLE_NORMAL, "bad") != -KOS_EINVAL) {
		return fail("styles invalid frame", 0);
	}
	if (tui_frame_title(&rect, TUI_FRAME_SINGLE, TUI_STYLE_NORMAL,
			    (const char *)0) != -KOS_EINVAL) {
		return fail("styles null title", 0);
	}

	/* One-cell, two-cell and flat degenerate rectangles. */
	rect.x = 1u;
	rect.y = 10u;
	rect.w = 1u;
	rect.h = 1u;
	if (tui_frame(&rect, TUI_FRAME_SINGLE, TUI_STYLE_NORMAL) != 0) {
		return fail("styles 1x1", 0);
	}
	rect.x = 3u;
	rect.w = 2u;
	rect.h = 2u;
	if (tui_frame(&rect, TUI_FRAME_SINGLE, TUI_STYLE_NORMAL) != 0) {
		return fail("styles 2x2", 0);
	}
	rect.x = 6u;
	rect.w = 1u;
	rect.h = 3u;
	if (tui_frame(&rect, TUI_FRAME_DOUBLE, TUI_STYLE_NORMAL) != 0) {
		return fail("styles 1x3", 0);
	}
	rect.x = 8u;
	rect.w = 3u;
	rect.h = 1u;
	if (tui_frame(&rect, TUI_FRAME_SINGLE, TUI_STYLE_NORMAL) != 0) {
		return fail("styles 3x1", 0);
	}
	rect.x = 12u;
	rect.y = 10u;
	rect.w = 0u;
	rect.h = 4u;
	if (tui_frame(&rect, TUI_FRAME_SINGLE, TUI_STYLE_NORMAL) != 0) {
		return fail("styles empty frame", 0);
	}

	/* Empty titled frames are no-ops: the sentinel row must survive. */
	if (tui_puts_xy(20, 19, "SENT") != 4) {
		return fail("styles sentinel", 0);
	}
	rect.x = 20u;
	rect.y = 19u;
	rect.w = 5u;
	rect.h = 0u;
	if (tui_frame_title(&rect, TUI_FRAME_SINGLE, TUI_STYLE_NORMAL,
			    "ZERO") != 0) {
		return fail("styles 5x0 title", 0);
	}
	rect.w = 0u;
	rect.h = 5u;
	if (tui_frame_title(&rect, TUI_FRAME_SINGLE, TUI_STYLE_NORMAL,
			    "ZERO") != 0) {
		return fail("styles 0x5 title", 0);
	}
	rect.x = 70u;
	rect.y = 10u;
	rect.w = 1u;
	rect.h = 1u;
	if (tui_frame_title(&rect, TUI_FRAME_SINGLE, TUI_STYLE_NORMAL,
			    "X") != 0) {
		return fail("styles 1x1 title", 0);
	}
	rect.x = 80u;
	rect.y = 15u;
	rect.w = 10u;
	rect.h = 5u;
	if (tui_frame_title(&rect, TUI_FRAME_SINGLE, TUI_STYLE_NORMAL,
			    "TITLE") != 0) {
		return fail("styles clipped title", 0);
	}

	rect.x = 2u;
	rect.y = 12u;
	rect.w = 10u;
	rect.h = 2u;
	if (tui_fill(&rect, '#', TUI_STYLE_ERROR) != 0) {
		return fail("styles fill", 0);
	}
	if (tui_hline(2, 15, 10, TUI_FRAME_SINGLE) != 0 ||
	    tui_vline(2, 16, 5, TUI_FRAME_DOUBLE) != 0 ||
	    tui_hline(2, 18, 10, TUI_FRAME_NONE) != 0 ||
	    tui_vline(2, 18, 5, TUI_FRAME_NONE) != 0 ||
	    tui_hline(2, 18, 0, TUI_FRAME_SINGLE) != 0) {
		return fail("styles lines", 0);
	}

	/* Alternating attribute runs: one reverse word, one normal word and
	 * a reverse status field on the same row. */
	rect.x = 0u;
	rect.y = 21u;
	rect.w = 85u;
	rect.h = 1u;
	if (tui_fill(&rect, ' ', TUI_STYLE_STATUS) != 0) {
		return fail("styles status fill", 0);
	}
	if (tui_set_style(TUI_STYLE_STATUS) != 0 ||
	    tui_puts_xy(0, 21, "STATUS") != 6 ||
	    tui_set_style(TUI_STYLE_NORMAL) != 0 ||
	    tui_puts_xy(7, 21, "normal") != 6 ||
	    tui_set_style(TUI_STYLE_SELECTED) != 0 ||
	    tui_puts_xy(14, 21, "SELECTED") != 8 ||
	    tui_set_style(TUI_STYLE_NORMAL) != 0) {
		return fail("styles runs", 0);
	}
	if (tui_set_attr(1u) != 0 || tui_get_attr() != 1u ||
	    tui_puts_xy(2, 20, "ATTR") != 4 ||
	    tui_set_attr(0u) != 0) {
		return fail("styles attr", 0);
	}

	/* A style change alone must not dirty an existing cell. */
	if (tui_puts_xy(2, 22, "SAME") != 4 || tui_flush() != 0) {
		return fail("styles same flush", 0);
	}
	if (tui_set_style(TUI_STYLE_TITLE) != 0 || tui_flush() != 0) {
		return fail("styles silent style", 0);
	}

	if (tui_gotoxy(0, 22) != 0 || tui_flush() != 0) {
		return fail("styles marker", 0);
	}
	printf("tui: styles pass\r\n");
	tui_shutdown();
	return 0;
}

static struct tui_cell region_buffer[20u * 8u];

static int region_mode(void)
{
	struct tui_rect rect;
	struct tui_rect push;
	uint16_t need;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("tui: region FAIL init %d\r\n", result);
		return 1;
	}
	rect.x = 10u;
	rect.y = 5u;
	rect.w = 20u;
	rect.h = 8u;
	if (tui_fill(&rect, '.', TUI_STYLE_NORMAL) != 0) {
		return fail("region fill", 0);
	}
	if (tui_set_style(TUI_STYLE_SELECTED) != 0 ||
	    tui_puts_xy(12, 6, "KEEP") != 4 ||
	    tui_set_style(TUI_STYLE_NORMAL) != 0 ||
	    tui_puts_xy(20, 9, "TEXT") != 4) {
		return fail("region content", 0);
	}
	need = tui_saved_size(&rect);
	if (need != (uint16_t)(20u * 8u * 2u)) {
		return fail("region size", need);
	}
	if (tui_save(&rect, region_buffer, need) != 0) {
		return fail("region save", 0);
	}
	/* Squeeze a popup over the saved region, then restore it exactly. */
	rect.x = 12u;
	rect.y = 7u;
	rect.w = 12u;
	rect.h = 4u;
	if (tui_fill(&rect, 'P', TUI_STYLE_TITLE) != 0 ||
	    tui_frame(&rect, TUI_FRAME_DOUBLE, TUI_STYLE_TITLE) != 0) {
		return fail("region popup", 0);
	}
	rect.x = 10u;
	rect.y = 5u;
	rect.w = 20u;
	rect.h = 8u;
	if (tui_restore(&rect, region_buffer, need) != 0) {
		return fail("region restore", 0);
	}
	if (tui_flush() != 0) {
		return fail("region first flush", 0);
	}
	/* Restoring identical cells emits no cell run. */
	if (tui_restore(&rect, region_buffer, need) != 0 ||
	    tui_flush() != 0) {
		return fail("region identical", 0);
	}

	/* Short and null buffers change neither the model nor the buffer. */
	if (tui_save(&rect, region_buffer, (uint16_t)(need - 1u)) !=
	    -KOS_EINVAL ||
	    tui_save(&rect, (struct tui_cell *)0, need) != -KOS_EINVAL ||
	    tui_restore(&rect, region_buffer, (uint16_t)(need - 1u)) !=
	    -KOS_EINVAL ||
	    tui_restore(&rect, (const struct tui_cell *)0, need) !=
	    -KOS_EINVAL) {
		return fail("region short", 0);
	}
	/* Empty clips have size zero and succeed without touching cells. */
	rect.x = 90u;
	rect.y = 5u;
	rect.w = 5u;
	rect.h = 5u;
	if (tui_saved_size(&rect) != 0u ||
	    tui_save(&rect, region_buffer, 0u) != 0 ||
	    tui_restore(&rect, region_buffer, 0u) != 0 ||
	    tui_save(&rect, (struct tui_cell *)0, 0u) != -KOS_EINVAL) {
		return fail("region empty", 0);
	}
	if (tui_saved_size((const struct tui_rect *)0) != 0u) {
		return fail("region null size", 0);
	}

	/* Nested-window relative save and restore. */
	push.x = 20u;
	push.y = 10u;
	push.w = 20u;
	push.h = 10u;
	if (tui_push_window(&push) != 0) {
		return fail("region push", 0);
	}
	rect.x = 2u;
	rect.y = 2u;
	rect.w = 10u;
	rect.h = 4u;
	need = tui_saved_size(&rect);
	if (need != (uint16_t)(10u * 4u * 2u)) {
		return fail("region nested size", need);
	}
	if (tui_fill(&rect, 'N', TUI_STYLE_ACCENT) != 0 ||
	    tui_save(&rect, region_buffer, need) != 0 ||
	    tui_fill(&rect, 'X', TUI_STYLE_ERROR) != 0 ||
	    tui_restore(&rect, region_buffer, need) != 0) {
		return fail("region nested", 0);
	}
	if (tui_pop_window() != 0) {
		return fail("region pop", 0);
	}

	if (tui_gotoxy(0, 22) != 0 || tui_flush() != 0) {
		return fail("region marker", 0);
	}
	printf("tui: region pass\r\n");
	tui_shutdown();
	return 0;
}


static const struct tui_menu_item widget_menu[] = {
	{"File", 'f', 10u},
	{"Edit", 'e', 11u},
	{"Help", 'h', 12u}
};

static const char * const widget_items[] = {"Red", "Green", "Blue"};
static uint8_t widget_checked;

static int widget_form_draw(char *name, uint8_t checked,
			    struct tui_list *list, tui_id_t selected)
{
	struct tui_rect rect;
	rect.x = 2u;
	rect.y = 6u;
	rect.w = 20u;
	rect.h = 8u;
	if (tui_statusbar("KonaOS Config") != 0 ||
	    tui_menubar(widget_menu, 3u, &selected) != 0 ||
	    tui_label(2, 2, "Name:") != 0 ||
	    tui_input(10, 2, 20u, name, 16u) != 0 ||
	    tui_checkbox(2, 4, "Enabled", &checked) != 0 ||
	    tui_listbox(&rect, list) != 0 ||
	    tui_button(2, 15, "OK", TUI_ID_OK, 1u) != 0 ||
	    tui_button(12, 15, "Cancel", TUI_ID_CANCEL, 0u) != 0) {
		return -1;
	}
	return 0;
}

static int flush_form(int variant)
{
	char name[4];
	struct tui_list list;
	tui_id_t selected = 10u;
	uint8_t checked = 1u;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("tui: flush FAIL init %d\r\n", result);
		return 1;
	}
	name[0] = 'A';
	name[1] = 'd';
	name[2] = 'a';
	name[3] = '\0';
	list.items = widget_items;
	list.count = 3u;
	list.selected = 1u;
	list.top = 0u;
	if (widget_form_draw(name, checked, &list, selected) < 0 ||
	    tui_flush() < 0) {
		return fail("flush form", 0);
	}
	if (variant == 1) {
		if (tui_flush() < 0) {
			return fail("flush unchanged", 0);
		}
	}
	if (variant == 2) {
		if (tui_label(2, 2, "Name!") < 0 || tui_flush() < 0) {
			return fail("flush changed", 0);
		}
	}
	if (variant == 0) {
		printf("tui: flushfull pass\r\n");
	} else if (variant == 1) {
		printf("tui: flushsame pass\r\n");
	} else {
		printf("tui: flushrow pass\r\n");
	}
	tui_shutdown();
	return 0;
}

static int flushfull_mode(void)
{
	return flush_form(0);
}

static int flushsame_mode(void)
{
	return flush_form(1);
}

static int flushrow_mode(void)
{
	return flush_form(2);
}

static int widgets_mode(void)
{
	struct tui_rect rect;
	struct tui_list list;
	struct tui_list empty;
	char name[16];
	char tiny[3];
	char long_label[60];
	tui_id_t selected = 10u;
	tui_id_t menu_result;
	tui_id_t menu_shortcut;
	tui_id_t menu_escape;
	tui_id_t confirm_result;
	tui_id_t message_result;
	tui_key_t key;
	uint16_t visible;
	int input_result;
	int input_cancel;
	int tiny_result;
	int result;
	int i;

	result = tui_init();
	if (result < 0) {
		printf("tui: widgets FAIL init %d\r\n", result);
		return 1;
	}
	widget_checked = 1u;
	name[0] = 'A';
	name[1] = 'd';
	name[2] = 'a';
	name[3] = '\0';
	tiny[0] = 'a';
	tiny[1] = 'b';
	tiny[2] = '\0';
	for (i = 0; i < 59; ++i) {
		long_label[i] = 'L';
	}
	long_label[59] = '\0';
	list.items = widget_items;
	list.count = 3u;
	list.selected = 1u;
	list.top = 0u;
	empty.items = (const char * const *)0;
	empty.count = 0u;
	empty.selected = 0u;
	empty.top = 0u;

	if (tui_menu_run((const struct tui_menu_item *)0, 0u) !=
	    TUI_ID_NONE ||
	    tui_menubar(widget_menu, 3u, (tui_id_t *)0) != -KOS_EINVAL ||
	    tui_message_box("X", "Y", 0u) != TUI_ID_NONE ||
	    tui_message_box("X", "Y", 0x8000u) != TUI_ID_NONE ||
	    tui_input(0, 0, 4u, (char *)0, 4u) != -KOS_EINVAL ||
	    tui_input_run(0, 0, 4u, (char *)0, 4u) != -KOS_EINVAL ||
	    tui_listbox((const struct tui_rect *)0, &list) != -KOS_EINVAL ||
	    tui_listbox(&rect, (struct tui_list *)0) != -KOS_EINVAL) {
		return fail("widgets invalid", 0);
	}
	rect.x = 2u;
	rect.y = 6u;
	rect.w = 2u;
	rect.h = 2u;
	if (tui_listbox(&rect, &list) != 0 ||
	    tui_listbox(&rect, &empty) != 0) {
		return fail("widgets minimal list", 0);
	}
	if (tui_label(60, 24, long_label) != 0) {
		return fail("widgets long label", 0);
	}
	rect.x = 2u;
	rect.y = 6u;
	rect.w = 20u;
	rect.h = 8u;
	if (widget_form_draw(name, widget_checked, &list, selected) != 0) {
		return fail("widgets form", 0);
	}
	if (tui_flush() != 0) {
		return fail("widgets form flush", 0);
	}

	/* Menu: navigate right twice and ENTER, then a shortcut, then ESC. */
	menu_result = tui_menu_run(widget_menu, 3u);
	menu_shortcut = tui_menu_run(widget_menu, 3u);
	menu_escape = tui_menu_run(widget_menu, 3u);

	/* Input: append on commit, edit and commit, then cancel. */
	input_result = tui_input_run(10, 2, 20u, name, 16u);
	input_result = tui_input_run(10, 2, 20u, name, 16u);
	input_cancel = tui_input_run(10, 2, 20u, name, 16u);

	/* List policy: END, HOME, DOWN, UP, PGDN, PGUP, ENTER. */
	visible = 6u;
	for (;;) {
		key = TUI_KEY_NONE;
		if (tui_listbox(&rect, &list) != 0 || tui_flush() != 0) {
			return fail("widgets list draw", 0);
		}
		key = tui_getkey();
		if (key == 0x000Du || key == 0x000Au || key == TUI_KEY_ESC) {
			break;
		}
		if (key == TUI_KEY_END) {
			list.selected = (uint16_t)(list.count - 1u);
		} else if (key == TUI_KEY_HOME) {
			list.selected = 0u;
		} else if (key == TUI_KEY_DOWN) {
			if ((uint16_t)(list.selected + 1u) < list.count) {
				++list.selected;
			}
		} else if (key == TUI_KEY_UP) {
			if (list.selected > 0u) {
				--list.selected;
			}
		} else if (key == TUI_KEY_PGDN) {
			list.selected = (uint16_t)(list.selected + visible);
			if (list.selected >= list.count) {
				list.selected = (uint16_t)(list.count - 1u);
			}
		} else if (key == TUI_KEY_PGUP) {
			if (list.selected > visible) {
				list.selected = (uint16_t)(list.selected - visible);
			} else {
				list.selected = 0u;
			}
		}
	}

	confirm_result = tui_confirm("Confirm",
				     "Apply all changes to the selected profile and close the editor now?");
	message_result = tui_message_box("Information",
					 "The configuration was saved.",
					 (uint16_t)(TUI_BUTTON(TUI_ID_OK) |
						    TUI_BUTTON(TUI_ID_CANCEL)));

	/* A full buffer accepts no more printable input, including ENTER. */
	tiny_result = tui_input_run(10, 18, 20u, tiny, 3u);

	if (menu_result != 12u) {
		return fail("widgets menu result", (int)menu_result);
	}
	if (menu_shortcut != 10u) {
		return fail("widgets shortcut", (int)menu_shortcut);
	}
	if (menu_escape != TUI_ID_NONE) {
		return fail("widgets menu escape", (int)menu_escape);
	}
	if (input_result != 1) {
		return fail("widgets input result", input_result);
	}
	if (input_cancel != 0) {
		return fail("widgets input cancel", input_cancel);
	}
	if (tiny_result != 1) {
		return fail("widgets tiny", tiny_result);
	}
	if (strcmp(name, "dam") != 0) {
		return fail("widgets edit buffer", 0);
	}
	if (strcmp(tiny, "ab") != 0) {
		return fail("widgets full buffer", 0);
	}
	if (list.selected != 0u) {
		return fail("widgets list selected", (int)list.selected);
	}
	if (list.top != 0u) {
		return fail("widgets list top", (int)list.top);
	}
	if (widget_checked != 1u) {
		return fail("widgets checked", (int)widget_checked);
	}
	if (confirm_result != TUI_ID_NO) {
		return fail("widgets confirm", (int)confirm_result);
	}
	if (message_result != TUI_ID_CANCEL) {
		return fail("widgets message", (int)message_result);
	}
	result = tui_gotoxy(0, 21);
	if (result != 0) {
		return fail("widgets transcript cursor", result);
	}
	result = tui_flush();
	if (result != 0) {
		return fail("widgets transcript flush", result);
	}
	printf("tui: widgets m=%u f=%u e=%u i=%d c=%d t=%d\r\n",
	       (unsigned int)menu_result, (unsigned int)menu_shortcut,
	       (unsigned int)menu_escape, input_result, input_cancel,
	       tiny_result);
	printf("tui: widgets n=%s s=%u cf=%u ms=%u\r\n", name,
	       (unsigned int)list.selected,
	       (unsigned int)confirm_result, (unsigned int)message_result);
	printf("tui: widgets pass\r\n");
	tui_shutdown();
	return 0;
}


static int loneesc_mode(void)
{
	tui_key_t key;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("tui: loneesc FAIL init %d\r\n", result);
		return 1;
	}
	key = tui_getkey();
	if (key != TUI_KEY_ESC) {
		printf("tui: loneesc FAIL key %04x\r\n", (unsigned int)key);
		tui_shutdown();
		return 1;
	}
	if (tui_pollkey() != TUI_KEY_NONE || tui_key_pending() != 0) {
		printf("tui: loneesc FAIL tail\r\n");
		tui_shutdown();
		return 1;
	}
	printf("tui: loneesc pass\r\n");
	tui_shutdown();
	return 0;
}

static int keyreset_mode(void)
{
	tui_key_t key;
	int result;

	result = tui_init();
	if (result < 0) {
		printf("tui: keyreset FAIL init %d\r\n", result);
		return 1;
	}
	/* Consume ESC '[' and leave the parser in CSI with no final byte. */
	if (tui_pollkey() != TUI_KEY_NONE) {
		printf("tui: keyreset FAIL prime\r\n");
		tui_shutdown();
		return 1;
	}
	printf("tui: keyreset primed\r\n");
	tui_shutdown();
	result = tui_init();
	if (result < 0) {
		printf("tui: keyreset FAIL reinit %d\r\n", result);
		return 1;
	}
	key = tui_getkey();
	if (key != (tui_key_t)'A') {
		printf("tui: keyreset FAIL key %04x\r\n", (unsigned int)key);
		tui_shutdown();
		return 1;
	}
	if (tui_pollkey() != TUI_KEY_NONE) {
		printf("tui: keyreset FAIL tail\r\n");
		tui_shutdown();
		return 1;
	}
	printf("tui: keyreset pass\r\n");
	tui_shutdown();
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
	if (argc == 1 && strcmp(argv[0], "invert") == 0) {
		return invert_mode();
	}
	if (argc == 1 && strcmp(argv[0], "invertspan") == 0) {
		return invertspan_mode();
	}
	if (argc == 1 && strcmp(argv[0], "renderboundary") == 0) {
		return renderboundary_mode();
	}
	if (argc == 1 && strcmp(argv[0], "renderrows") == 0) {
		return renderrows_mode();
	}
	if (argc == 1 && strcmp(argv[0], "renderbad") == 0) {
		return renderbad_mode();
	}
	if (argc == 1 && strcmp(argv[0], "rendermax") == 0) {
		return rendermax_mode();
	}
	if (argc == 1 && strcmp(argv[0], "invertwrap") == 0) {
		return invertwrap_mode();
	}
	if (argc == 1 && strcmp(argv[0], "rendersync") == 0) {
		return rendersync_mode();
	}
	if (argc == 1 && strcmp(argv[0], "renderflip") == 0) {
		return renderflip_mode();
	}
	if (argc == 1 && strcmp(argv[0], "screen") == 0) {
		return screen_mode();
	}
	if (argc == 1 && strcmp(argv[0], "shutdown") == 0) {
		return shutdown_mode();
	}
	if (argc == 1 && strcmp(argv[0], "windows") == 0) {
		return windows_mode();
	}
	if (argc == 1 && strcmp(argv[0], "keys") == 0) {
		return keys_mode();
	}
	if (argc == 1 && strcmp(argv[0], "loneesc") == 0) {
		return loneesc_mode();
	}
	if (argc == 1 && strcmp(argv[0], "keyreset") == 0) {
		return keyreset_mode();
	}
	if (argc == 1 && strcmp(argv[0], "styles") == 0) {
		return styles_mode();
	}
	if (argc == 1 && strcmp(argv[0], "region") == 0) {
		return region_mode();
	}
	if (argc == 1 && strcmp(argv[0], "widgets") == 0) {
		return widgets_mode();
	}
	if (argc == 1 && strcmp(argv[0], "flushfull") == 0) {
		return flushfull_mode();
	}
	if (argc == 1 && strcmp(argv[0], "flushsame") == 0) {
		return flushsame_mode();
	}
	if (argc == 1 && strcmp(argv[0], "flushrow") == 0) {
		return flushrow_mode();
	}
	printf("tui: FAIL mode\r\n");
	return 1;
}
