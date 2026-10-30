/* SN-Track ZTR document view and player.  P plays the song through
 * libsnd: the timer handler ticks the engine, and the cursor follows the
 * row the engine plays.  Audio gets the deadlines; the display gets what
 * remains - the next page is drawn on the hidden one between cursor
 * moves, the cursor jumps when drawing falls behind, and the exit report
 * says how far it did. */

#include "kos.h"
#include "kos/errno.h"
#include "kos/event.h"
#include "kos/gfx.h"
#include "kos/reader.h"
#include "kos/snd.h"
#include "kos/timer.h"
#include "sntview_tick.h"
#include "view.h"
#include "ztrdoc.h"

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

/* Playback: the engine is ticked by the timer handler while `ticking`;
 * `playing` while the cursor follows it, `paused` while held, `ended`
 * after the song's end.  The lateness and audio counts for the report. */
static int timer_handle;
static kos_handle_source_t source;
static kos_reader_t reader;
static uint8_t snd_opened, song_loaded, song_used, playing, paused, ended;
static uint16_t serviced_order;
static uint16_t rows_scrolled, late_rows, late_pages;
static struct snd_ztr_status status;

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
#define T_REFILL 6u
#define T_KINDS  7u
#define T_NONE   0xFFu
static const char *const t_names[T_KINDS] = {
	" order ", " panel ", " row ", " finish ", " load ", " move ",
	" refill "
};
static uint32_t t_sum[T_KINDS], last_now;
static uint16_t t_n[T_KINDS], t_max[T_KINDS];
static uint8_t last_kind;	/* start_play sets T_NONE */

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

static int start_timer(void)
{
	uint32_t period;
	if (snd_timer_period(&period) < 0) return -1;
	/* Prime idle() before this process subscribes to the timer. */
	idle();
	timer_handle = timer_open();
	if (timer_handle < 0) { timer_handle = 0; return -1; }
	if (notify(timer_handle, on_tick) < 0 ||
	    timer_event(timer_handle, period) < 0) return -1;
	return 0;
}

static void stop_timer(void)
{
	if (timer_handle <= 0) return;
	(void)timer_event(timer_handle, 0u);
	(void)notify(timer_handle, (kos_event_handler_t)0);
	(void)unbind((unsigned char)timer_handle);
	timer_handle = 0;
}

/* The title bar's transport on the shown page: PLAY or STOP and the
 * time the ticks played make. */
static int show_transport(void)
{
	uint32_t ticks = ticks_played;
	screen.playing = (uint8_t)(playing != 0u && paused == 0u);
	screen.seconds = (uint16_t)(ticks / document.rate);
	return view_update_transport(&screen, shown_page);
}

/* Play from the song's start: order 0's first page, the cursor on its
 * first row; a song played before is loaded again (the tables the
 * document borrows are refilled with the same contents). */
static int start_play(void)
{
	if (song_used != 0u) {
		(void)snd_ztr_stop(); snd_ztr_close();
		song_loaded = 0u;
		if (snd_ztr_open(&reader) < 0) return -1;
		song_loaded = 1u;
	}
	song_used = 1u;
	for (;;) {
		int result = snd_ztr_prepare();
		if (result < 0) return -1;
		if (result != SND_NEED_MORE) break;
		(void)snd_ztr_service();
	}
	if (order != 0u || first_row != 0u) {
		next_order = 0u; next_first = 0u; prep = 0u; next = NEXT_PAGE;
		if (flip() < 0) return -1;
	} else if (cursor != 0u) {
		if (view_move_cursor(shown_page, cursor, 0u) < 0) return -1;
		cursor = 0u;
	}
	if (timer_handle <= 0 && start_timer() < 0) return -1;
	serviced_order = 0xFFFFu;
	ticks_played = 0u;
	last_kind = T_NONE;
	ended = 0u; paused = 0u; playing = 1u;
	if (snd_ztr_play() < 0) return -1;
	ticking = 1u;
	return show_transport();
}

static int pause_play(uint8_t hold)
{
	if (hold != 0u) {
		ticking = 0u;
		if (snd_ztr_pause() < 0) return -1;
	} else {
		if (snd_ztr_resume() < 0) return -1;
		ticking = 1u;
		last_kind = T_NONE;
	}
	paused = hold;
	return show_transport();
}

/* Follow the engine: the refill first (audio's deadline), then the
 * cursor to the row the engine plays, else a piece of the hidden page.
 * Skipped rows and a page not drawn by its flip count as late; nothing
 * queues.  Returns 1 after a cursor move or an idle wait (a moment to
 * poll the console), 0 after other work, or a negative error. */
static int follow(void)
{
	uint32_t now;
	uint16_t advance, moved, at, target;
	uint8_t to, t_order, t_row;
	if (timer_now(timer_handle, &now) < 0) return -1;
	account(now);
	(void)snd_ztr_get_status(&status);
	if (status.order != serviced_order) {
		/* The engine changed order: this service refills the next. */
		serviced_order = status.order;
		(void)snd_ztr_service();
		last_kind = T_REFILL;
		return 0;
	}
	(void)snd_ztr_service();
	if (status.state == SND_ZTR_ERROR) return -1;
	/* The engine names the row it consumes next: the one playing is
	 * the row before it (none yet before the first tick). */
	t_order = status.order; t_row = status.row;
	if (status.state == SND_ZTR_READY ||
	    (t_order == 0u && t_row == 0u)) {
		t_row = 0u;
	} else if (t_row != 0u) {
		--t_row;
	} else {
		--t_order; t_row = (uint8_t)(document.rows - 1u);
	}
	/* Rows from the cursor to the engine's row (never backwards). */
	target = t_row;
	for (at = order; at < t_order; ++at) target = (uint16_t)(target + document.rows);
	at = (uint16_t)(first_row + cursor);
	advance = target > at ? (uint16_t)(target - at) : 0u;
	if (advance == 0u) {
		int fetched = 0;
		if (status.state == SND_ZTR_DONE) {
			ticking = 0u; playing = 0u; ended = 1u;
			return show_transport() < 0 ? -1 : 1;
		}
		if ((uint16_t)(ticks_played / document.rate) != screen.seconds) {
			last_kind = T_MOVE;
			return show_transport() < 0 ? -1 : 1;
		}
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
	last_kind = T_MOVE;
	to = cursor;
	while (advance != 0u) {
		uint8_t left = (uint8_t)(last_row() - to);
		if (advance <= left) { to = (uint8_t)(to + advance); break; }
		advance = (uint16_t)(advance - left - 1u);
		if (next == NEXT_NONE) target_next(0u);
		if (next == NEXT_END) {
			/* Past the song's last row: never, the engine stops. */
			moved = (uint16_t)(moved - advance - 1u);
			to = last_row();
			break;
		}
		if (to != cursor && view_move_cursor(shown_page, cursor, to) < 0)
			return -1;
		if (prep <= VIEW_STEPS) ++late_pages;
		if (flip() < 0) return -1;
		cursor = to = 0u;
		if (show_transport() < 0) return -1;
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
	int file;
	uint8_t gfx_opened = 0u, quit = 0u, poll = 1u;
	int result, rc = 1;

	if (argc < 1 || argc > 2 || argv == 0 ||
	    (argc == 2 && parse_order(argv[1], &order) < 0)) {
		console_puts("sntview: usage: sntview FILE.ZTR [ORDER]\r\n");
		return 1;
	}
	/* A raw handle reader: the reads are whole spans, and stdio would
	 * cost the program 6 KB it does not have. */
	file = open(argv[0], KFS_O_READ);
	if (file < 0) { console_puts("sntview: cannot open song\r\n"); return 1; }
	source.handle = (unsigned int)file;
	result = kos_reader_from_handle(&reader, &source);
	/* libsnd loads and checks the song; the document borrows its
	 * tables. */
	if (result >= 0) {
		if (snd_open() < 0) {
			console_puts("sntview: cannot open /dev/sound\r\n");
			goto cleanup;
		}
		snd_opened = 1u;
		result = snd_ztr_open(&reader);
	}
	if (result >= 0) {
		struct snd_ztr_tables tables;
		song_loaded = 1u;
		result = snd_ztr_tables(&tables);
		if (result >= 0)
			result = ztrdoc_open(&document, &reader, &tables,
					     order_buffer, sizeof(order_buffer));
	}
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
			if (playing != 0u) result = pause_play((uint8_t)(paused ^ 1u));
			else result = start_play();
			if (result < 0) goto cleanup;
			continue;
		}
		if (playing != 0u && paused == 0u) {
			result = follow();
			if (result < 0) goto cleanup;
			poll = (uint8_t)result;
			continue;
		}
		/* Space flips pages while stopped, not while paused: the view
		 * must stay on the engine's row. */
		if (key != ' ' || paused != 0u) { if (key == 0) idle(); continue; }
		if (next != NEXT_PAGE) target_next(1u);
		if (flip() < 0) goto cleanup;
		ended = 0u;
	}
	rc = 0;

cleanup:
	ticking = 0u;
	stop_timer();
	if (song_loaded != 0u) {
		(void)snd_ztr_get_status(&status);
		(void)snd_ztr_stop(); snd_ztr_close();
	}
	if (snd_opened != 0u) snd_close();
	if (gfx_opened != 0u) view_close();
	(void)close((unsigned int)file);
	if (song_loaded != 0u) {
		console_puts("sntview: underruns "); put_decimal(status.underruns);
		console_puts(", overruns "); put_decimal(status.overruns);
		console_puts(", missed ticks "); put_decimal(missed_events);
		console_puts("\r\n");
	}
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
