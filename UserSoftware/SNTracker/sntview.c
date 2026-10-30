/* SN-Track static ZTR document view.  Playback remains in sntracker.c. */

#include "kos.h"
#include "kos/errno.h"
#include "kos/gfx.h"
#include "kos/reader.h"
#include "view.h"
#include "ztrdoc.h"

static struct ztrdoc document;
static struct view screen;
static uint8_t order_buffer[ZTRDOC_ORDER_BUFFER];

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

int main(int argc, char **argv)
{
	FILE *file = 0;
	kos_reader_t reader;
	uint16_t order = 0u, first_row = 0u;
	uint8_t shown_page = 0u, gfx_opened = 0u, done = 0u;
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
	result = view_open(&screen, &document, order_buffer, order);
	if (result < 0) { console_puts("sntview: cannot open display\r\n"); goto cleanup; }
	gfx_opened = 1u;
	/* gfx_set_mode shows page 0 with the display off: draw, then enable. */
	if (view_draw_page(&screen, 0u, 0u) < 0 ||
	    gfx_display_enable(1u) < 0) goto cleanup;

	while (done == 0u) {
		char key;
		if (console_poll() <= 0) { idle(); continue; }
		if (console_read(&key, 1u) <= 0) continue;
		if (key == 'q' || key == 'Q' || key == 27) { done = 1u; continue; }
		if (key != ' ') continue;
		first_row = (uint16_t)(first_row + ZTRDOC_VIEW_ROWS);
		if (first_row >= document.rows) {
			first_row = 0u; ++order;
			if (order >= document.order_count) order = 0u;
			if (ztrdoc_load_order(&document, order, order_buffer,
					      sizeof(order_buffer)) < 0) goto cleanup;
			screen.order = order;
			if (order < screen.order_top || order >= screen.order_top + 4u)
				screen.order_top = order;
		}
		shown_page ^= 1u;
		if (view_draw_page(&screen, shown_page, first_row) < 0 ||
		    gfx_show_page(shown_page) < 0) goto cleanup;
	}
	rc = 0;

cleanup:
	if (gfx_opened != 0u) view_close();
	if (file != 0) (void)fclose(file);
	if (rc != 0) console_puts("sntview: stopped with an error\r\n");
	return rc;
}
