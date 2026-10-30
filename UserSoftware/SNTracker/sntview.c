/* SN-Track ZTR document view.  Playback remains in sntracker.c: P scrolls
 * the cursor at the song's tempo with no sound, drawing the next page on
 * the hidden one between cursor moves, and reports how far it fell
 * behind. */

#include "kos.h"
#include "kos/errno.h"
#include "kos/gfx.h"
#include "kos/reader.h"
#include "kos/timer.h"
#include "view.h"
#include "ztrdoc.h"

/* The clock runs in 25.6 us counts: 78125 counts are two seconds. */
#define COUNTS_2S 78125ul

static struct ztrdoc document;
static struct view screen;
static uint8_t order_buffer[ZTRDOC_ORDER_BUFFER];

/* The shown page and the cursor's viewport row on it. */
static uint16_t order, first_row;
static uint16_t loaded_order;
static uint8_t shown_page, cursor;

/* The hidden page: whether it has a target (NEXT_NONE, NEXT_PAGE, or
 * NEXT_END past the song's last page), the target, and how far it is
 * drawn (0 not begun, 1..VIEW_STEPS the next step plus one,
 * VIEW_STEPS + 1 ready). */
#define NEXT_NONE 0u
#define NEXT_PAGE 1u
#define NEXT_END  2u
static uint16_t next_order, next_first;
static uint8_t next, prep;

/* Scrolling: the clock's row deadlines and the lateness counts. */
static int timer_handle;
static uint8_t playing, ended;
static uint32_t start, deadline, row_counts, row_frac, frac, frac_den;
static uint16_t due, done, rows_scrolled, late_rows, late_pages;

/* What each kind of loop step costs, clock read to clock read (so the
 * console poll and the read itself are included): the order load and
 * decode, a panel, a row, the rules/status/cursor finish, a prefetch,
 * a cursor move (with any flip).  Reported at exit, in ms. */
#define T_ORDER  0u
#define T_PANEL  1u
#define T_ROW    2u
#define T_FINISH 3u
#define T_LOAD   4u
#define T_MOVE   5u
#define T_KINDS  6u
#define T_NONE   0xFFu
static const char *const t_names[T_KINDS] = {
	" order ", " panel ", " row ", " finish ", " load ", " move "
};
static uint32_t t_sum[T_KINDS], last_now;
static uint16_t t_n[T_KINDS], t_max[T_KINDS];
static uint8_t last_kind;	/* start_scroll sets T_NONE */

static int parse_order(const char *text, uint16_t *value)
{
	uint16_t result = 0u; uint8_t digit;
	if (text == 0 || *text == '\0') return -1;
	while (*text != '\0') {
		if (*text < '0' || *text > '9') return -1;
		digit = (uint8_t)(*text++ - '0');
		if (result > 6553u || (result == 6553u && digit > 5u)) return -1;
		result = (uint16_t)(result * 10u + digit);
	}
	*value = result; return 0;
}

static void put_decimal(uint16_t value)
{
	char b[6]; uint8_t n = 5u;
	b[5] = '\0';
	do { b[--n] = (char)('0' + value % 10u); value /= 10u; }
	while (value != 0u);
	console_puts(b + n);
}

/* Counts (25.6 us) to whole ms. */
static uint16_t to_ms(uint32_t counts)
{
	return (uint16_t)((counts << 8) / 10000ul);
}

static void account(uint32_t now)
{
	uint32_t spent = now - last_now;
	if (last_kind != T_NONE) {
		t_sum[last_kind] += spent; ++t_n[last_kind];
		if (spent > t_max[last_kind])
			t_max[last_kind] = spent > 0xFFFFu ? 0xFFFFu : (uint16_t)spent;
	}
	last_now = now;
	last_kind = T_NONE;
}

static int load(uint16_t number)
{
	int result;
	if (number == loaded_order) return 0;
	result = ztrdoc_load_order(&document, number, order_buffer,
				   sizeof(order_buffer));
	if (result < 0) return result;
	loaded_order = number;
	return 0;
}

/* Target the page after the shown one; past the song's end, order 0
 * when `wrap`, else NEXT_END. */
static void target_next(uint8_t wrap)
{
	next_order = order;
	next_first = (uint16_t)(first_row + ZTRDOC_VIEW_ROWS);
	prep = 0u; next = NEXT_PAGE;
	if (next_first >= document.rows) {
		next_first = 0u; ++next_order;
		if (next_order >= document.order_count) {
			if (wrap == 0u) next = NEXT_END;
			next_order = 0u;
		}
	}
}

/* One piece of the hidden page. */
static int prep_step(void)
{
	uint8_t hidden = (uint8_t)(shown_page ^ 1u);
	if (prep == 0u) {
		if (load(next_order) < 0) return -1;
		screen.order = next_order;
		if (next_order < screen.order_top ||
		    next_order >= screen.order_top + VIEW_PANEL_LINES)
			screen.order_top = next_order;
		if (view_begin_page(&screen, hidden, next_first) < 0) return -1;
	} else if (view_draw_step(&screen, hidden, (uint8_t)(prep - 1u)) < 0) {
		return -1;
	}
	++prep;
	return 0;
}

/* With the hidden page drawn, the order buffer is free: load the order
 * of the page after it now rather than inside a short page's time.
 * Returns 1 when it loaded one, 0 when none is needed, or an error. */
static int prefetch(void)
{
	uint16_t after = next_order;
	if ((uint16_t)(next_first + ZTRDOC_VIEW_ROWS) >= document.rows) ++after;
	if (after >= document.order_count || after == loaded_order) return 0;
	return load(after) < 0 ? -1 : 1;
}

/* Finish the hidden page and show it, the cursor on its first row. */
static int flip(void)
{
	while (prep <= VIEW_STEPS)
		if (prep_step() < 0) return -1;
	shown_page ^= 1u;
	if (gfx_show_page(shown_page) < 0) return -1;
	order = next_order; first_row = next_first; cursor = 0u;
	next = NEXT_NONE;
	return 0;
}

/* The shown page's last viewport row. */
static uint8_t last_row(void)
{
	uint16_t left = (uint16_t)(document.rows - first_row);
	return (uint8_t)((left < ZTRDOC_VIEW_ROWS ? left : ZTRDOC_VIEW_ROWS) - 1u);
}

static int start_scroll(void)
{
	if (timer_handle <= 0) {
		timer_handle = timer_open();
		if (timer_handle < 0) { timer_handle = 0; return -1; }
	}
	if (ended != 0u) {
		/* From the song's start: order 0's first page, shown. */
		next_order = 0u; next_first = 0u; prep = 0u; next = NEXT_PAGE;
		if (flip() < 0) return -1;
		ended = 0u;
	}
	/* A row is `speed` ticks at `rate` Hz: speed * 78125 / (2 * rate)
	 * counts, kept as a quotient and a remainder. */
	frac_den = (uint32_t)document.rate << 1;
	row_counts = 0u;
	for (row_frac = 0u; row_frac < document.speed; ++row_frac)
		row_counts += COUNTS_2S;	/* the runtime has no __mullong */
	row_frac = row_counts % frac_den;
	row_counts /= frac_den;
	if (timer_now(timer_handle, &start) < 0) return -1;
	deadline = row_counts; frac = row_frac; due = done = 0u;
	last_kind = T_NONE;
	playing = 1u;
	return 0;
}

/* Move the cursor to the row the clock says is due: skipped rows and a
 * hidden page not drawn in time count as late, nothing queues.  Returns
 * 1 after a cursor move or an idle wait (a moment to poll the console),
 * 0 after a piece of the hidden page, or a negative error. */
static int scroll(void)
{
	uint32_t now;
	uint16_t advance, moved;
	uint8_t to;
	if (timer_now(timer_handle, &now) < 0) return -1;
	account(now);
	while ((uint32_t)(now - start) >= deadline) {
		++due; deadline += row_counts; frac += row_frac;
		if (frac >= frac_den) { frac -= frac_den; ++deadline; }
	}
	advance = (uint16_t)(due - done);
	if (advance == 0u) {
		int fetched = 0;
		if (next == NEXT_NONE) { target_next(0u); return 1; }
		if (next == NEXT_PAGE && prep <= VIEW_STEPS) {
			last_kind = prep == 0u ? T_ORDER : prep <= VIEW_PANEL_STEPS ? T_PANEL :
				    prep < VIEW_STEPS ? T_ROW : T_FINISH;
			return prep_step();
		}
		if (next == NEXT_PAGE) fetched = prefetch();
		if (fetched != 0) { last_kind = T_LOAD; return fetched < 0 ? -1 : 0; }
		idle();
		return 1;
	}
	moved = advance;
	done = due;
	last_kind = T_MOVE;
	to = cursor;
	while (advance != 0u) {
		uint8_t left = (uint8_t)(last_row() - to);
		if (advance <= left) { to = (uint8_t)(to + advance); break; }
		advance = (uint16_t)(advance - left - 1u);
		if (next == NEXT_NONE) target_next(0u);
		if (next == NEXT_END) {
			/* The song's end: the cursor stops on its last row;
			 * the rows past it were never shown. */
			moved = (uint16_t)(moved - advance - 1u);
			to = last_row(); playing = 0u; ended = 1u;
			break;
		}
		if (to != cursor && view_move_cursor(shown_page, cursor, to) < 0)
			return -1;
		if (prep <= VIEW_STEPS) ++late_pages;
		if (flip() < 0) return -1;
		cursor = to = 0u;
	}
	if (moved > 1u) late_rows = (uint16_t)(late_rows + moved - 1u);
	rows_scrolled = (uint16_t)(rows_scrolled + moved);
	if (to != cursor) {
		if (view_move_cursor(shown_page, cursor, to) < 0) return -1;
		cursor = to;
	}
	return 1;
}

int main(int argc, char **argv)
{
	FILE *file = 0;
	kos_reader_t reader;
	uint8_t gfx_opened = 0u, quit = 0u, poll = 1u;
	int result, rc = 1;

	if (argc < 1 || argc > 2 || argv == 0 ||
	    (argc == 2 && parse_order(argv[1], &order) < 0)) {
		console_puts("sntview: usage: sntview FILE.ZTR [ORDER]\r\n");
		return 1;
	}
	file = fopen(argv[0], "rb");
	if (file == 0) { console_puts("sntview: cannot open song\r\n"); return 1; }
	result = kos_reader_from_stdio(&reader, file);
	if (result >= 0)
		result = ztrdoc_open(&document, &reader, order_buffer,
				     sizeof(order_buffer));
	if (result >= 0 && order >= document.order_count) {
		console_puts("sntview: no such order\r\n");
		goto cleanup;
	}
	if (result >= 0)
		result = ztrdoc_load_order(&document, order, order_buffer,
					   sizeof(order_buffer));
	if (result < 0) {
		console_puts(result == -KOS_ENOSPC ?
			     "sntview: an order exceeds 1024 stream bytes\r\n" :
			     result == -KOS_ENOTSUP ?
			     "sntview: an instrument plays tone and noise\r\n" :
			     "sntview: invalid ZTR song\r\n");
		goto cleanup;
	}
	loaded_order = order;
	result = view_open(&screen, &document, order_buffer, order);
	if (result < 0) { console_puts("sntview: cannot open display\r\n"); goto cleanup; }
	gfx_opened = 1u;
	/* gfx_set_mode shows page 0 with the display off: draw, then enable. */
	if (view_draw_page(&screen, 0u, 0u) < 0 ||
	    gfx_display_enable(1u) < 0) goto cleanup;

	while (quit == 0u) {
		char key = 0;
		/* While scrolling, the console waits for a cursor move or an
		 * idle moment: a poll costs as much as a page piece. */
		if (poll != 0u && console_poll() > 0 &&
		    console_read(&key, 1u) <= 0) key = 0;
		poll = 1u;
		if (key == 'q' || key == 'Q' || key == 27) { quit = 1u; continue; }
		if (key == 'p' || key == 'P') {
			if (playing != 0u) playing = 0u;
			else if (start_scroll() < 0) goto cleanup;
			continue;
		}
		if (playing != 0u) {
			result = scroll();
			if (result < 0) goto cleanup;
			poll = (uint8_t)result;
			continue;
		}
		if (key != ' ') { if (key == 0) idle(); continue; }
		if (next != NEXT_PAGE) target_next(1u);
		if (flip() < 0) goto cleanup;
		ended = 0u;
	}
	rc = 0;

cleanup:
	if (gfx_opened != 0u) view_close();
	if (timer_handle > 0) (void)unbind((unsigned char)timer_handle);
	if (file != 0) (void)fclose(file);
	if (rows_scrolled != 0u) {
		console_puts("sntview: "); put_decimal(rows_scrolled);
		console_puts(" rows, "); put_decimal(late_rows);
		console_puts(" late, "); put_decimal(late_pages);
		console_puts(" late pages\r\nsntview: ms mean/max");
		for (result = 0; result < (int)T_KINDS; ++result) {
			if (t_n[result] == 0u) continue;
			console_puts(t_names[result]);
			put_decimal(to_ms(t_sum[result] / t_n[result]));
			console_puts("/"); put_decimal(to_ms(t_max[result]));
		}
		console_puts("\r\n");
	}
	if (rc != 0) console_puts("sntview: stopped with an error\r\n");
	return rc;
}
