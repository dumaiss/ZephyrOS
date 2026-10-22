/* KonaOS direct-text spike bench.
 *
 * `textbench.exe` measures what a direct-write text renderer (font in
 * CPU RAM, glyph bytes OUT to the V9958 data port - the SymbOS method)
 * would gain over the console's HMMM renderer, on the same realistic
 * text, before any RAM is spent on it in the driver tier.  One row per
 * renderer, T-states per character, with the same TIMER instrument and
 * the closing summary as `iobench`.
 *
 * Rows:
 *
 *   txt.console  the mixed text through console_write in 256-byte calls
 *                (the baseline; equals con.mixed);
 *   txt.glyph1   holding the VDP: per glyph, eight scanlines of "set
 *                the VRAM address, three OUT (A0h)", the bytes expanded
 *                from the 1-bit font through a 64-entry, 3-byte table;
 *   txt.glyph6   as txt.glyph1 from a pre-coloured 6,144-byte font
 *                (24 bytes per glyph, OUTI), no expansion;
 *   txt.run1     per run (the printable bytes up to a control byte or
 *                the right margin): one address set per scanline, then
 *                three bytes per glyph of the run, 1-bit expanded;
 *   txt.run6     as txt.run1 from the pre-coloured font.
 *
 * The direct rows own the text model (CR, LF, wrap at 85, circular
 * R#23 scrolling with the exposed-row HMMV) and live in textbench.asm.
 * They bind `/dev/video` exclusively around the measured render and
 * release it before the row line prints; on a console without a video
 * driver they print `SKIP no video`.  The expand table and the
 * pre-coloured font are built at run time, outside the measured batch.
 *
 * With one argument, only that row runs and the program then waits for
 * one console byte before printing its line (the replay snapshots the
 * clean screen during that wait); with none, every row runs and the
 * closing `textbench: summary` prints one `sum:` line per row.
 */

#include "kos.h"
#include "kos/timer.h"

#include "textbench.font.h"

#define TEXT_CHARS  2000ul
#define WRITE_CHUNK 256u
#define WRITE_CALLS (TEXT_CHARS / WRITE_CHUNK + 1ul)

/* The VDP ports the direct rows drive. */
__sfr __at 0xA0 textbench_vdp_data;
__sfr __at 0xA1 textbench_vdp_cmd;
__sfr __at 0xA3 textbench_vdp_indirect;

/* GRAPHIC 6 constants (drivers/v9958/v9958_driver.h, hardware.h). */
#define VDP_REG_R14       0x0Eu
#define VDP_REG_R23       0x17u
#define VDP_R23_TEXT_BASE 0xFCu /* 100h - the 4-line display offset */
#define VDP_DISPLAY_OFFSET 4u
#define VDP_CELL_WIDTH    6u
#define VDP_CELL_HEIGHT   8u
#define VDP_COLS          85u
#define VDP_ROWS          26u
#define VDP_CLEAR_WIDTH_LO  0x00u
#define VDP_CLEAR_WIDTH_HI  0x02u /* 512 pixels */
#define VDP_CLEAR_HEIGHT_LO 0x00u
#define VDP_CLEAR_HEIGHT_HI 0x01u /* 256 lines */
#define VDP_FILL_NORMAL   0x44u
#define VDP_CMD_HMMV      0xC0u

#define CLOCK_CAL_READS 64u

static int timer_handle;
static uint32_t batch_start_clock;
static uint32_t batch_end_clock;
static unsigned long clock_read_cost;

/* The text: `ls -l`-style lines, the same generator as iobench's
 * con.mixed. */
static char mixed_text[TEXT_CHARS + 1u];

/* The renderer tables (built at run time, outside the batches). */
unsigned char textbench_expand[64u * 3u];
unsigned char textbench_font6[256u * 24u];

/* The text model the asm renderers keep (also read by the record). */
unsigned char textbench_x;
unsigned char textbench_y;
unsigned char textbench_origin;
/* The VRAM page (y >> 6) the renderers last wrote to R#14. */
unsigned char textbench_page;
const char *textbench_text;
unsigned int textbench_text_len;

/* One row's accumulated figures, all in T-states. */
static const char *row_name;
static unsigned long row_ops;
static unsigned long row_bytes;
static unsigned long row_total;
static unsigned long row_min;
static unsigned long row_max;

/* The summary: one entry per row, in run order. */
#define SUM_ROWS 6u
static const char *sum_name[SUM_ROWS];
static unsigned long sum_mean[SUM_ROWS];
static unsigned long sum_per_byte[SUM_ROWS];
static unsigned char sum_skip[SUM_ROWS];
static unsigned char sum_count;

/* The asm renderers (textbench.asm). */
extern void textbench_render_glyph1(void);
extern void textbench_render_glyph6(void);
extern void textbench_render_run1(void);
extern void textbench_render_run6(void);

static unsigned long counts_to_tstates(uint32_t from, uint32_t to)
{
	return (unsigned long)(to - from) << 8;
}

static void clock_read(uint32_t *now)
{
	if (timer_now(timer_handle, now) < 0) {
		*now = 0u;
	}
}

static void batch_begin(void)
{
	clock_read(&batch_start_clock);
}

/* Close a batch of `ops` operations and fold its per-op cost into the
 * row; the batch's two clock readings are the bench's own cost. */
static void batch_end(unsigned long ops)
{
	unsigned long spent;

	clock_read(&batch_end_clock);
	spent = counts_to_tstates(batch_start_clock, batch_end_clock);
	if (spent > 2ul * clock_read_cost) {
		spent -= 2ul * clock_read_cost;
	} else {
		spent = 0ul;
	}
	row_total += spent;
	spent /= ops;
	if (spent < row_min) {
		row_min = spent;
	}
	if (spent > row_max) {
		row_max = spent;
	}
}

static void measure_clock_cost(void)
{
	uint32_t first;
	uint32_t last;
	unsigned int i;

	batch_begin();
	for (i = 0u; i < CLOCK_CAL_READS; ++i) {
		uint32_t scratch;

		clock_read(&scratch);
	}
	batch_end(1ul);
	first = batch_start_clock;
	last = batch_end_clock;
	clock_read_cost = counts_to_tstates(first, last) /
			  (unsigned long)(CLOCK_CAL_READS + 2u);
}

static void row_begin(const char *name, unsigned long ops,
		      unsigned long bytes)
{
	row_name = name;
	row_ops = ops;
	row_bytes = bytes;
	row_total = 0ul;
	row_min = 0xFFFFFFFFul;
	row_max = 0ul;
}

static void sum_add(const char *name, unsigned long mean,
		    unsigned long per_byte, unsigned char skip)
{
	if (sum_count < SUM_ROWS) {
		sum_name[sum_count] = name;
		sum_mean[sum_count] = mean;
		sum_per_byte[sum_count] = per_byte;
		sum_skip[sum_count] = skip;
		++sum_count;
	}
}

static void row_report(void)
{
	unsigned long mean = row_total / row_ops;
	unsigned long per_byte = row_bytes != 0ul ?
				 row_total / row_bytes : 0ul;

	printf("textbench: %s ops %lu bytes %lu min %lu mean %lu max %lu "
	       "per-byte %lu\r\n", row_name, row_ops, row_bytes, row_min,
	       mean, row_max, per_byte);
	sum_add(row_name, mean, per_byte, 0u);
}

static void row_skip_video(const char *name, int error)
{
	printf("textbench: %s SKIP no video (%d)\r\n", name, error);
	sum_add(name, 0ul, 0ul, 1u);
}

static void sum_report(void)
{
	unsigned char i;

	printf("textbench: summary\r\n");
	for (i = 0u; i < sum_count; ++i) {
		if (sum_skip[i] != 0u) {
			printf("sum: %s SKIP\r\n", sum_name[i]);
		} else {
			printf("sum: %s mean %lu per-byte %lu\r\n",
			       sum_name[i], sum_mean[i], sum_per_byte[i]);
		}
	}
}

/* The mixed text, exactly as iobench's con.mixed generates it. */
static void make_mixed_text(void)
{
	unsigned long at = 0ul;
	unsigned int n = 0u;

	while (at < TEXT_CHARS) {
		int written = snprintf(mixed_text + at,
				       (unsigned int)(TEXT_CHARS - at),
				       "drwxr-xr-x  2 kitamura kitamura %5u "
				       "Oct 21 08:00 file%u.txt\r\n",
				       4096u + n, n);

		if (written <= 0) {
			break;
		}
		at += (unsigned long)written;
		++n;
	}
	while (at < TEXT_CHARS) {
		mixed_text[at++] = ' ';
	}
	mixed_text[TEXT_CHARS] = '\0';
}

/* --- the VDP, while held ----------------------------------------------- */

static void vdp_reg(unsigned char value, unsigned char reg)
{
	textbench_vdp_cmd = value;
	textbench_vdp_cmd = (unsigned char)(0x80u | reg);
}

static void vdp_wait_ce(void)
{
	while (textbench_vdp_cmd & 0x01u) {
	}
}

/* One HMMV fill: the block mirrors the console's (DX/DY/NX/NY/CLR,
 * ARG 0 - destination page zero - and the HMMV code).  R#32 points the
 * indirect port at the command registers; the R#46 byte starts it. */
static void vdp_hmmv(unsigned char dy, unsigned int nx, unsigned char ny,
		     unsigned char clr)
{
	static unsigned char fill_cmd[15];
	unsigned char i;

	vdp_wait_ce();
	fill_cmd[6] = dy;
	fill_cmd[8] = (unsigned char)(nx & 0xFFu);
	fill_cmd[9] = (unsigned char)(nx >> 8);
	fill_cmd[10] = ny;
	fill_cmd[12] = clr;
	fill_cmd[14] = VDP_CMD_HMMV;
	vdp_reg(32u, 17u);
	for (i = 0u; i < 15u; ++i) {
		textbench_vdp_indirect = fill_cmd[i];
	}
}

/* The clear the console runs for ED 2 (drivers/v9958/v9958.c
 * v9958_clear_screen): page zero, R#23 back at the text base, one
 * 512 x 256 background fill, and the model parked at home. */
static void textbench_clear(void)
{
	textbench_origin = 0u;
	textbench_page = 0u;
	vdp_reg(0x00u, VDP_REG_R14);
	vdp_reg(VDP_R23_TEXT_BASE, VDP_REG_R23);
	vdp_hmmv(0u, 512u, 0u, VDP_FILL_NORMAL);
	vdp_wait_ce();
	textbench_x = 0u;
	textbench_y = 0u;
}

/* One cell's scroll, exactly as v9958_scroll_up: advance the origin,
 * repaint the fixed margin above logical row zero (visible only near
 * the ring wrap), refill the exposed bottom row (split at physical
 * line 255 like v9958_fill_cells), then commit the new origin. */
void textbench_scroll(void)
{
	unsigned char origin =
		(unsigned char)(textbench_origin + VDP_CELL_HEIGHT);
	unsigned char dy = (unsigned char)(origin +
					   (VDP_ROWS - 1u) * VDP_CELL_HEIGHT);

	textbench_origin = origin;
	vdp_hmmv((unsigned char)(origin - VDP_DISPLAY_OFFSET),
		 512u, VDP_DISPLAY_OFFSET, VDP_FILL_NORMAL);
	if ((unsigned char)(dy + VDP_CELL_HEIGHT) < dy) {
		unsigned char wrapped =
			(unsigned char)(dy + VDP_CELL_HEIGHT);

		vdp_hmmv(dy, (unsigned int)VDP_COLS * VDP_CELL_WIDTH,
			 (unsigned char)(0u - dy), VDP_FILL_NORMAL);
		if (wrapped == 0u) {
			vdp_wait_ce();
			vdp_reg((unsigned char)(origin - VDP_DISPLAY_OFFSET),
				VDP_REG_R23);
			return;
		}
		dy = 0u;
		vdp_hmmv(dy, (unsigned int)VDP_COLS * VDP_CELL_WIDTH,
			 wrapped, VDP_FILL_NORMAL);
	} else {
		vdp_hmmv(dy, (unsigned int)VDP_COLS * VDP_CELL_WIDTH,
			 VDP_CELL_HEIGHT, VDP_FILL_NORMAL);
	}
	vdp_wait_ce();
	vdp_reg((unsigned char)(origin - VDP_DISPLAY_OFFSET), VDP_REG_R23);
}

/* --- tables ------------------------------------------------------------ */

/* The 1-bit font scanline -> three pre-coloured G6 bytes, through the
 * console's pair table (44/4F/F4/FF: pixel on F, off 4). */
static void build_expand_table(void)
{
	static const unsigned char pair_normal[4] = {
		0x44u, 0x4Fu, 0xF4u, 0xFFu
	};
	unsigned int v;

	for (v = 0u; v < 64u; ++v) {
		textbench_expand[v * 3u + 0u] = pair_normal[(v >> 4) & 3u];
		textbench_expand[v * 3u + 1u] = pair_normal[(v >> 2) & 3u];
		textbench_expand[v * 3u + 2u] = pair_normal[v & 3u];
	}
}

/* The pre-coloured font: every glyph expanded, 24 bytes per glyph. */
static void build_font6(void)
{
	unsigned int glyph;
	unsigned int scan;

	for (glyph = 0u; glyph < 256u; ++glyph) {
		for (scan = 0u; scan < 8u; ++scan) {
			unsigned int v = textbench_font[glyph * 8u + scan] >> 2;
			unsigned char *dst = textbench_font6 + glyph * 24u +
					     scan * 3u;

			dst[0] = textbench_expand[v * 3u + 0u];
			dst[1] = textbench_expand[v * 3u + 1u];
			dst[2] = textbench_expand[v * 3u + 2u];
		}
	}
}

/* --- rows -------------------------------------------------------------- */

/* The console row's render: clear, then the text in eight 256-byte
 * writes, measured. */
static void run_console_render(void)
{
	unsigned long remaining = TEXT_CHARS;
	unsigned long offset = 0ul;
	unsigned long i;

	/* Clear and home; the direct rows clear themselves the same way. */
	(void)console_write("\033[2J", 4u);
	row_begin("txt.console", WRITE_CALLS, TEXT_CHARS);
	batch_begin();
	for (i = 0ul; i < WRITE_CALLS; ++i) {
		unsigned int length =
			remaining > (unsigned long)WRITE_CHUNK ?
			WRITE_CHUNK : (unsigned int)remaining;

		(void)console_write(mixed_text + offset, length);
		offset += length;
		remaining -= length;
	}
	batch_end(WRITE_CALLS);
}

static int hold_video(void)
{
	unsigned char type = 0u;

	return bind("/dev/video", KOS_BIND_EXCLUSIVE, &type);
}

/* One direct row's render, still held on return (the replay snapshots
 * the clean screen during the wait that follows): bind, tables, clear
 * and the measured renderer.  Returns the handle, or the negated error
 * (already reported as SKIP). */
static int run_direct_render(const char *name, void (*renderer)(void))
{
	int video = hold_video();

	if (video < 0) {
		row_skip_video(name, video);
		return video;
	}
	build_expand_table();
	build_font6();
	textbench_clear();
	textbench_text = mixed_text;
	textbench_text_len = TEXT_CHARS;
	row_begin(name, TEXT_CHARS, TEXT_CHARS);
	batch_begin();
	renderer();
	batch_end(TEXT_CHARS);
	return video;
}

static int run_one_row(const char *name)
{
	char ch;
	int video = -1;

	if (strcmp(name, "txt.console") == 0) {
		run_console_render();
	} else if (strcmp(name, "txt.glyph1") == 0) {
		video = run_direct_render(name, textbench_render_glyph1);
	} else if (strcmp(name, "txt.glyph6") == 0) {
		video = run_direct_render(name, textbench_render_glyph6);
	} else if (strcmp(name, "txt.run1") == 0) {
		video = run_direct_render(name, textbench_render_run1);
	} else if (strcmp(name, "txt.run6") == 0) {
		video = run_direct_render(name, textbench_render_run6);
	} else {
		printf("textbench: unknown row %s\r\n", name);
		return 2;
	}
	/* One-row mode: hold the clean screen (the direct rows still hold
	 * the VDP) for the replay, then release and print the line. */
	for (;;) {
		int result = console_read(&ch, 1);

		if (result == 1) {
			break;
		}
		idle();
	}
	if (video >= 0) {
		(void)unbind((unsigned char)video);
	}
	row_report();
	return 0;
}

static void run_all_rows(void)
{
	int video;

	run_console_render();
	row_report();
	video = run_direct_render("txt.glyph1", textbench_render_glyph1);
	if (video >= 0) {
		(void)unbind((unsigned char)video);
		row_report();
	}
	video = run_direct_render("txt.glyph6", textbench_render_glyph6);
	if (video >= 0) {
		(void)unbind((unsigned char)video);
		row_report();
	}
	video = run_direct_render("txt.run1", textbench_render_run1);
	if (video >= 0) {
		(void)unbind((unsigned char)video);
		row_report();
	}
	video = run_direct_render("txt.run6", textbench_render_run6);
	if (video >= 0) {
		(void)unbind((unsigned char)video);
		row_report();
	}
}

int main(int argc, char **argv)
{
	int result;

	make_mixed_text();
	timer_handle = timer_open();
	if (timer_handle < 0) {
		printf("textbench: SKIP no timer (%d)\r\n", timer_handle);
		return 1;
	}
	measure_clock_cost();
	if (clock_read_cost == 0ul) {
		printf("textbench: SKIP clock frozen\r\n");
		return 1;
	}

	printf("textbench: begin\r\n");

	if (argc == 1) {
		result = run_one_row(argv[0]);
	} else {
		run_all_rows();
		sum_report();
		result = 0;
	}

	printf("textbench: end\r\n");
	return result;
}
