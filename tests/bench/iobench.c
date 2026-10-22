/* KonaOS I/O bench.
 *
 * `iobench.exe` measures what a program's ordinary I/O costs: one row
 * per pattern, each row one line with the row's op/byte counts and its
 * min/mean/max T-states per op plus the mean per byte.  The clock is the
 * resident TIMER driver (25.6 us counts, so 256 T-states at 10 MHz); the
 * bench measures its own readings first and subtracts the two a batch
 * spends, because a reading is itself an IPC call (about 15,000
 * T-states) and a cheap row would otherwise report mostly its own
 * instrument.
 *
 * That fixes how a row is sampled: a batch must span enough counts to be
 * read at all, and eight batches of the console rows would each fit in
 * one or two counts, where the reading error is larger than the effect
 * measured.  The console and ipc rows therefore run as one batch (the
 * row's cost is its total), and the file rows keep eight, their calls
 * being large enough to sample.
 *
 * Rows:
 *
 *   ipc.null     100 descriptor-less console calls (console_poll);
 *   con.putchar  2,000 characters through putchar;
 *   con.printf   the same 2,000 characters through one printf call;
 *   con.write    the same 2,000 characters in 256-byte console_writes
 *                (seven full, one of 208);
 *   con.mixed    2,000 characters of `ls -l`-style lines (names,
 *                spaces, digits, CR LF, under 80 columns), the
 *                realistic case;
 *   con.space    2,000 spaces (HMMV fills only);
 *   con.held     the con.write text while the bench holds the VDP: the
 *                call chain, staging and drop path with no rendering;
 *   vdp.hmmm6    holding the VDP, 2,000 HMMM copies of one 6x8 glyph
 *                from the atlas, each started and then waited for (CE):
 *                setup plus the engine's whole time per copy;
 *   vdp.hmmm12   the same with 12x8 copies (the paired width): against
 *                vdp.hmmm6, whether the engine is bound per command or
 *                per byte;
 *   vdp.direct.on/off  holding the VDP, 2,000 direct writes of one 6x8
 *                glyph (eight scanlines, each "set the VRAM address,
 *                three OUT (A0h)" of constant bytes, display on), once
 *                with sprites on and once with R#8 sprites off: the
 *                port-only write ceiling under the V9958's WAIT, before
 *                any font fetch or expansion;
 *   file.byte    a 32 KiB card file read one byte per read;
 *   file.getc    the same file one byte per getc, through the stream
 *                buffer (one card read per 512);
 *   file.block   the same file in 512-byte reads.
 *
 * After the last row the bench prints `iobench: summary` and one short
 * `sum:` line per row (its mean and per-byte figures, or SKIP), so the
 * whole result fits on one screen: the VDP rows' releases reset the
 * display and wipe the rows printed before them.
 *
 * The held and VDP rows bind `/dev/video` exclusively for the row and
 * release it right after the row prints (the release re-runs the
 * display init, so the next row starts from a reset display).
 *
 * The file rows create `/fs/BENCHTMP` and the file in it and remove both
 * afterwards; with no card they report SKIP.  The bench prints
 * `iobench: begin`, one line per row, and `iobench: end`, and returns 0
 * when every row ran or reported SKIP (1 when the clock is missing, 2
 * when a row failed).
 */

#include "kos.h"
#include "kos/timer.h"

#define IPC_CALLS   100ul
#define CON_CHARS   2000ul
#define WRITE_CHUNK 256u
#define WRITE_CALLS (CON_CHARS / WRITE_CHUNK + 1ul)

#define BENCH_DIR   "/fs/BENCHTMP"
#define BENCH_FILE  "/fs/BENCHTMP/BENCH.BIN"
#define FILE_BYTES  32768ul
#define READ_CHUNK  512u

#define MIXED_CHUNK 256u
#define HMMM_COPIES 2000ul
#define DIRECT_GLYPHS 2000ul
#define DIRECT_BYTES (8u * 3u)

/* The VDP ports the held rows drive directly (the command port needs
 * the two-byte protocol; the data and indirect ports are single OUTs). */
__sfr __at 0xA0 bench_vdp_data;
__sfr __at 0xA1 bench_vdp_cmd;
__sfr __at 0xA3 bench_vdp_indirect;

/* The pattern the file rows write and verify: every byte position gets a
 * value of its own, so a misplaced byte is visible to the check. */
#define PATTERN(i)  ((unsigned char)((i) * 7ul + 3ul))

#define CLOCK_CAL_READS 64u

static int timer_handle;
static uint32_t batch_start_clock;
static uint32_t batch_end_clock;
static unsigned long clock_read_cost;   /* T-states per timer_now call */
static unsigned int failures;

/* The console text: CON_CHARS characters, NUL-terminated for the printf
 * row and written out whole by the write row. */
static char con_text[CON_CHARS + 1u];

/* The mixed and space rows' texts, the same length. */
static char mixed_text[CON_CHARS + 1u];
static char space_text[CON_CHARS + 1u];

/* The direct-write row's VRAM address (page 0, R#14 already zero). */
static unsigned int bench_vdp_addr;

static unsigned char file_block[READ_CHUNK];

/* One row's accumulated figures, all in T-states. */
static const char *row_name;
static unsigned long row_ops;
static unsigned long row_bytes;
static unsigned long row_batches;
static unsigned long row_total;
static unsigned long row_min;
static unsigned long row_max;

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
 * row: the batch's two clock readings are the bench's own cost and are
 * subtracted before the division. */
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

/* The clock's own cost, so a row can report its work and not its two
 * readings: one reading is an IPC call to the TIMER driver. */
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
	/* The interval holds CLOCK_CAL_READS readings plus its two ends;
	 * the loop's own T-states are far inside the reading's noise. */
	clock_read_cost = counts_to_tstates(first, last) /
			  (unsigned long)(CLOCK_CAL_READS + 2u);
}

static unsigned long batch_ops(unsigned long index)
{
	unsigned long base = row_ops / row_batches;
	unsigned long extra = row_ops % row_batches;

	return base + (index < extra ? 1ul : 0ul);
}

static void row_begin(const char *name, unsigned long ops,
		      unsigned long bytes, unsigned long batches)
{
	row_name = name;
	row_ops = ops;
	row_bytes = bytes;
	row_batches = batches;
	row_total = 0ul;
	row_min = 0xFFFFFFFFul;
	row_max = 0ul;
}

/* The summary: one entry per row, in run order.  A SKIP row keeps its
 * error in `sum_mean` and is marked by `sum_skip`. */
#define SUM_ROWS 20u
static const char *sum_name[SUM_ROWS];
static unsigned long sum_mean[SUM_ROWS];
static unsigned long sum_per_byte[SUM_ROWS];
static unsigned char sum_skip[SUM_ROWS];
static unsigned char sum_count;

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

	printf("iobench: %s ops %lu bytes %lu min %lu mean %lu max %lu "
	       "per-byte %lu\r\n", row_name, row_ops, row_bytes, row_min,
	       mean, row_max, per_byte);
	sum_add(row_name, mean, per_byte, 0u);
}

static void row_skip(const char *name, int error)
{
	printf("iobench: %s SKIP no card (%d)\r\n", name, error);
	sum_add(name, 0ul, 0ul, 1u);
}

static void row_skip_video(const char *name, int error)
{
	printf("iobench: %s SKIP no video (%d)\r\n", name, error);
	sum_add(name, 0ul, 0ul, 1u);
}

static void sum_report(void)
{
	unsigned char i;

	printf("iobench: summary\r\n");
	for (i = 0u; i < sum_count; ++i) {
		if (sum_skip[i] != 0u) {
			printf("sum: %s SKIP\r\n", sum_name[i]);
		} else {
			printf("sum: %s mean %lu per-byte %lu\r\n",
			       sum_name[i], sum_mean[i], sum_per_byte[i]);
		}
	}
}

static void bench_ipc_null(void)
{
	unsigned long b;

	row_begin("ipc.null", IPC_CALLS, 0ul, 1ul);
	for (b = 0ul; b < row_batches; ++b) {
		unsigned long n = batch_ops(b);
		unsigned long i;

		batch_begin();
		for (i = 0ul; i < n; ++i) {
			(void)console_poll();
		}
		batch_end(n);
	}
	row_report();
}

static void bench_putchar(void)
{
	unsigned long b;

	row_begin("con.putchar", CON_CHARS, CON_CHARS, 1ul);
	for (b = 0ul; b < row_batches; ++b) {
		unsigned long n = batch_ops(b);
		unsigned long i;

		batch_begin();
		for (i = 0ul; i < n; ++i) {
			putchar('x');
		}
		batch_end(n);
	}
	row_report();
}

static void bench_printf(void)
{
	unsigned long b;

	row_begin("con.printf", 1ul, CON_CHARS, 1ul);
	for (b = 0ul; b < row_batches; ++b) {
		unsigned long n = batch_ops(b);
		unsigned long i;

		batch_begin();
		for (i = 0ul; i < n; ++i) {
			(void)printf("%s", con_text);
		}
		batch_end(n);
	}
	row_report();
}

static void bench_write(void)
{
	unsigned long b;
	unsigned long remaining = CON_CHARS;
	unsigned long offset = 0ul;

	row_begin("con.write", WRITE_CALLS, CON_CHARS, 1ul);
	for (b = 0ul; b < row_batches; ++b) {
		unsigned long n = batch_ops(b);
		unsigned long i;

		batch_begin();
		for (i = 0ul; i < n; ++i) {
			unsigned int length =
				remaining > (unsigned long)WRITE_CHUNK ?
				WRITE_CHUNK : (unsigned int)remaining;

			(void)console_write(con_text + offset, length);
			offset += length;
			remaining -= length;
		}
		batch_end(n);
	}
	row_report();
}

/* Run the 8-call 256-byte console_write loop over `text`, the shape of
 * the con.write row shared by the mixed, space and held rows. */
static void bench_write_text(const char *text)
{
	unsigned long b;
	unsigned long remaining = CON_CHARS;
	unsigned long offset = 0ul;

	for (b = 0ul; b < row_batches; ++b) {
		unsigned long n = batch_ops(b);
		unsigned long i;

		batch_begin();
		for (i = 0ul; i < n; ++i) {
			unsigned int length =
				remaining > (unsigned long)WRITE_CHUNK ?
				WRITE_CHUNK : (unsigned int)remaining;

			(void)console_write(text + offset, length);
			offset += length;
			remaining -= length;
		}
		batch_end(n);
	}
}

/* con.mixed: `ls -l`-style lines, the realistic case. */
static void bench_mixed(void)
{
	row_begin("con.mixed", WRITE_CALLS, CON_CHARS, 1ul);
	bench_write_text(mixed_text);
	row_report();
}

/* con.space: blanks only, so the run engine issues HMMV fills. */
static void bench_space(void)
{
	row_begin("con.space", WRITE_CALLS, CON_CHARS, 1ul);
	bench_write_text(space_text);
	row_report();
}

/* Take the VDP exclusively for the held and VDP rows. */
static int bench_video_hold(void)
{
	unsigned char type = 0u;

	return bind("/dev/video", KOS_BIND_EXCLUSIVE, &type);
}

/* con.held: the con.write text while the bench holds the VDP.  The bind
 * sits outside the batch (one takeover per 2,000 characters); the hold
 * is released before the report so the row prints, and the release
 * re-runs the display init before the next row. */
static void bench_held(void)
{
	unsigned long b;
	unsigned long remaining = CON_CHARS;
	unsigned long offset = 0ul;
	int video = bench_video_hold();

	if (video < 0) {
		row_skip_video("con.held", video);
		return;
	}
	row_begin("con.held", WRITE_CALLS, CON_CHARS, 1ul);
	for (b = 0ul; b < row_batches; ++b) {
		unsigned long n = batch_ops(b);
		unsigned long i;

		batch_begin();
		for (i = 0ul; i < n; ++i) {
			unsigned int length =
				remaining > (unsigned long)WRITE_CHUNK ?
				WRITE_CHUNK : (unsigned int)remaining;

			(void)console_write(con_text + offset, length);
			offset += length;
			remaining -= length;
		}
		batch_end(n);
	}
	(void)unbind((unsigned char)video);
	row_report();
}

/* One register write through the command port's two-byte protocol. */
static void vdp_reg(unsigned char value, unsigned char reg)
{
	bench_vdp_cmd = value;
	bench_vdp_cmd = (unsigned char)(0x80u | reg);
}

/* Wait for the command engine: status register 2, CE. */
static void vdp_wait_ce(void)
{
	while (bench_vdp_cmd & 0x01u) {
	}
}

/* One HMMM copy from the precolored atlas: `nx` pixels wide (6 or 12),
 * always 8 lines tall.  The block mirrors the console's own glyph
 * template: SX from 'a', SY 0100h | its atlas row (the atlas lives in
 * page one), DX/DY zero, argument zero (destination page zero).  R#32
 * points the indirect port at the command registers; the R#46 byte
 * starts the copy, and the copy is waited for before returning, so a
 * row's time is setup plus the engine's whole time, never overlapped. */
static unsigned char glyph_cmd[15];

static void vdp_hmmm_init(void)
{
	static const unsigned char glyph_template[15] = {
		0x00, 0x00, 0x18, 0x01, 0x00, 0x00, 0x00, 0x00,
		0x06, 0x00, 0x08, 0x00, 0x00, 0x00, 0xD0,
	};
	unsigned char i;

	for (i = 0u; i < 15u; ++i) {
		glyph_cmd[i] = glyph_template[i];
	}
}

static void vdp_hmmm_copy(unsigned char nx)
{
	unsigned char i;

	glyph_cmd[8] = nx;
	vdp_reg(32u, 17u);
	for (i = 0u; i < 15u; ++i) {
		bench_vdp_indirect = glyph_cmd[i];
	}
	vdp_wait_ce();
}

/* vdp.hmmm6 / vdp.hmmm12: HMMM_COPIES engine copies `nx` pixels wide
 * (6, or the pair width 12) while held, each waited for.  The byte count
 * is the copy's VRAM bytes (nx / 2 per line, eight lines). */
static void bench_hmmm(unsigned char nx)
{
	const char *name = nx == 6u ? "vdp.hmmm6" : "vdp.hmmm12";
	unsigned long b;
	int video = bench_video_hold();

	if (video < 0) {
		row_skip_video(name, video);
		return;
	}
	row_begin(name, HMMM_COPIES,
		  nx == 6u ? HMMM_COPIES * 24ul : HMMM_COPIES * 48ul, 1ul);
	for (b = 0ul; b < row_batches; ++b) {
		unsigned long n = batch_ops(b);
		unsigned long i;

		batch_begin();
		for (i = 0ul; i < n; ++i) {
			vdp_hmmm_copy(nx);
		}
		batch_end(n);
	}
	(void)unbind((unsigned char)video);
	row_report();
}

/* One 6x8 glyph's direct write: eight unrolled scanlines, each "set the
 * VRAM address, three OUT (A0h)" (six pixels), advancing 256 bytes per
 * scanline (one GRAPHIC 6 line).  The bytes are constants: the cost is
 * the ports' alone, at `bench_vdp_addr`, page zero. */
static void vdp_direct_once(void)
{
	__asm__(
		"ld	hl,(_bench_vdp_addr)\n"
		"ld	de,#256\n"
		"ld	a,l\n"
		"out	(0xA1),a\n"
		"ld	a,h\n"
		"or	#0x40\n"
		"out	(0xA1),a\n"
		"ld	a,#0x21\n"
		"out	(0xA0),a\n"
		"ld	a,#0x43\n"
		"out	(0xA0),a\n"
		"ld	a,#0x65\n"
		"out	(0xA0),a\n"
		"add	hl,de\n"
		"ld	a,l\n"
		"out	(0xA1),a\n"
		"ld	a,h\n"
		"or	#0x40\n"
		"out	(0xA1),a\n"
		"ld	a,#0x21\n"
		"out	(0xA0),a\n"
		"ld	a,#0x43\n"
		"out	(0xA0),a\n"
		"ld	a,#0x65\n"
		"out	(0xA0),a\n"
		"add	hl,de\n"
		"ld	a,l\n"
		"out	(0xA1),a\n"
		"ld	a,h\n"
		"or	#0x40\n"
		"out	(0xA1),a\n"
		"ld	a,#0x21\n"
		"out	(0xA0),a\n"
		"ld	a,#0x43\n"
		"out	(0xA0),a\n"
		"ld	a,#0x65\n"
		"out	(0xA0),a\n"
		"add	hl,de\n"
		"ld	a,l\n"
		"out	(0xA1),a\n"
		"ld	a,h\n"
		"or	#0x40\n"
		"out	(0xA1),a\n"
		"ld	a,#0x21\n"
		"out	(0xA0),a\n"
		"ld	a,#0x43\n"
		"out	(0xA0),a\n"
		"ld	a,#0x65\n"
		"out	(0xA0),a\n"
		"add	hl,de\n"
		"ld	a,l\n"
		"out	(0xA1),a\n"
		"ld	a,h\n"
		"or	#0x40\n"
		"out	(0xA1),a\n"
		"ld	a,#0x21\n"
		"out	(0xA0),a\n"
		"ld	a,#0x43\n"
		"out	(0xA0),a\n"
		"ld	a,#0x65\n"
		"out	(0xA0),a\n"
		"add	hl,de\n"
		"ld	a,l\n"
		"out	(0xA1),a\n"
		"ld	a,h\n"
		"or	#0x40\n"
		"out	(0xA1),a\n"
		"ld	a,#0x21\n"
		"out	(0xA0),a\n"
		"ld	a,#0x43\n"
		"out	(0xA0),a\n"
		"ld	a,#0x65\n"
		"out	(0xA0),a\n"
		"add	hl,de\n"
		"ld	a,l\n"
		"out	(0xA1),a\n"
		"ld	a,h\n"
		"or	#0x40\n"
		"out	(0xA1),a\n"
		"ld	a,#0x21\n"
		"out	(0xA0),a\n"
		"ld	a,#0x43\n"
		"out	(0xA0),a\n"
		"ld	a,#0x65\n"
		"out	(0xA0),a\n"
		"add	hl,de\n"
		"ld	a,l\n"
		"out	(0xA1),a\n"
		"ld	a,h\n"
		"or	#0x40\n"
		"out	(0xA1),a\n"
		"ld	a,#0x21\n"
		"out	(0xA0),a\n"
		"ld	a,#0x43\n"
		"out	(0xA0),a\n"
		"ld	a,#0x65\n"
		"out	(0xA0),a\n"
		"ret\n");
}

/* vdp.direct: the CPU's own write ceiling under the V9958's WAIT while
 * the display is on.  `sprites_off` selects R#8's SPD bit; the row
 * reports as vdp.direct.on / vdp.direct.off. */
static void bench_direct(unsigned char sprites_off)
{
	unsigned long b;
	int video = bench_video_hold();

	if (video < 0) {
		row_skip_video(sprites_off != 0u ? "vdp.direct.off"
						: "vdp.direct.on", video);
		return;
	}
	vdp_reg(0x00u, 14u); /* VRAM address organisation: page zero */
	vdp_reg(sprites_off != 0u ? 0x0Au : 0x08u, 8u);
	bench_vdp_addr = 0u;
	row_begin(sprites_off != 0u ? "vdp.direct.off" : "vdp.direct.on",
		  DIRECT_GLYPHS, DIRECT_GLYPHS * DIRECT_BYTES, 1ul);
	for (b = 0ul; b < row_batches; ++b) {
		unsigned long n = batch_ops(b);
		unsigned long i;

		batch_begin();
		for (i = 0ul; i < n; ++i) {
			vdp_direct_once();
		}
		batch_end(n);
	}
	(void)unbind((unsigned char)video);
	row_report();
}

/* Put FILE_BYTES pattern bytes into `/fs/BENCHTMP/BENCH.BIN`; 0 when the
 * card took the whole file, the negated error otherwise. */
static int bench_file_create(void)
{
	unsigned long offset;
	int handle;

	if (mkdir(BENCH_DIR) < 0) {
		struct kfs_stat info;

		if (stat(BENCH_DIR, &info) < 0) {
			return -KOS_ENOENT;
		}
	}
	handle = open(BENCH_FILE, (unsigned char)(KFS_O_WRITE |
						  KFS_O_CREATE |
						  KFS_O_TRUNC));
	if (handle < 0) {
		return handle;
	}
	for (offset = 0ul; offset < FILE_BYTES; offset += READ_CHUNK) {
		unsigned int i;
		int result;

		for (i = 0u; i < READ_CHUNK; ++i) {
			file_block[i] = PATTERN(offset + i);
		}
		result = write((unsigned int)handle, file_block, READ_CHUNK);
		if (result != (int)READ_CHUNK) {
			(void)close((unsigned int)handle);
			return result < 0 ? result : -KOS_EIO;
		}
	}
	if (sync((unsigned int)handle) < 0) {
		(void)close((unsigned int)handle);
		return -KOS_EIO;
	}
	if (close((unsigned int)handle) < 0) {
		return -KOS_EIO;
	}
	return 0;
}

static void bench_file_remove(void)
{
	(void)unlink(BENCH_FILE);
	(void)rmdir(BENCH_DIR);
}

/* file.byte: one byte per read, each byte checked against the pattern. */
static void bench_file_byte(void)
{
	unsigned long b;
	unsigned long position = 0ul;
	unsigned int bad = 0u;
	int handle = open(BENCH_FILE, KFS_O_READ);

	if (handle < 0) {
		row_skip("file.byte", handle);
		return;
	}
	row_begin("file.byte", FILE_BYTES, FILE_BYTES, 8ul);
	for (b = 0ul; b < row_batches; ++b) {
		unsigned long n = batch_ops(b);
		unsigned long i;

		batch_begin();
		for (i = 0ul; i < n; ++i) {
			char byte;
			int got = read((unsigned int)handle, &byte, 1u);

			if (got != 1) {
				bad = 1u;
				break;
			}
			if ((unsigned char)byte != PATTERN(position)) {
				bad = 1u;
				break;
			}
			++position;
		}
		batch_end(n);
		if (bad != 0u) {
			break;
		}
	}
	(void)close((unsigned int)handle);
	if (bad != 0u) {
		++failures;
		printf("iobench: file.byte FAIL card read\r\n");
		return;
	}
	row_report();
}

/* file.block: the same file in 512-byte reads, every byte checked. */
static void bench_file_block(void)
{
	unsigned long b;
	unsigned long position = 0ul;
	unsigned int bad = 0u;
	int handle = open(BENCH_FILE, KFS_O_READ);

	if (handle < 0) {
		row_skip("file.block", handle);
		return;
	}
	row_begin("file.block", FILE_BYTES / READ_CHUNK,
		  FILE_BYTES, 8ul);
	for (b = 0ul; b < row_batches; ++b) {
		unsigned long n = batch_ops(b);
		unsigned long i;

		batch_begin();
		for (i = 0ul; i < n; ++i) {
			unsigned int k;
			int got = read((unsigned int)handle, file_block,
				       READ_CHUNK);

			if (got != (int)READ_CHUNK) {
				bad = 1u;
				break;
			}
			for (k = 0u; k < READ_CHUNK; ++k) {
				if (file_block[k] != PATTERN(position + k)) {
					bad = 1u;
					break;
				}
			}
			if (bad != 0u) {
				break;
			}
			position += READ_CHUNK;
		}
		batch_end(n);
		if (bad != 0u) {
			break;
		}
	}
	(void)close((unsigned int)handle);
	if (bad != 0u) {
		++failures;
		printf("iobench: file.block FAIL card read\r\n");
		return;
	}
	row_report();
}

/* file.getc: the same file through the stream layer, one getc at a
 * time; the stream's 512-byte buffer turns that into one card read per
 * 512 bytes. */
static void bench_file_getc(void)
{
	unsigned long b;
	unsigned long position = 0ul;
	unsigned int bad = 0u;
	FILE *stream = fopen(BENCH_FILE, "r");

	if (stream == (FILE *)0) {
		row_skip("file.getc", -KOS_ENOENT);
		return;
	}
	row_begin("file.getc", FILE_BYTES, FILE_BYTES, 8ul);
	for (b = 0ul; b < row_batches; ++b) {
		unsigned long n = batch_ops(b);
		unsigned long i;

		batch_begin();
		for (i = 0ul; i < n; ++i) {
			int ch = getc(stream);

			if (ch < 0) {
				bad = 1u;
				break;
			}
			if ((unsigned char)ch != PATTERN(position)) {
				bad = 1u;
				break;
			}
			++position;
		}
		batch_end(n);
		if (bad != 0u) {
			break;
		}
	}
	(void)fclose(stream);
	if (bad != 0u) {
		++failures;
		printf("iobench: file.getc FAIL card read\r\n");
		return;
	}
	row_report();
}

/* Build the mixed row's text: `ls -l`-style lines (each under 80
 * columns), filled to exactly CON_CHARS characters. */
static void make_mixed_text(void)
{
	unsigned long at = 0ul;
	unsigned int n = 0u;

	while (at < CON_CHARS) {
		int written = snprintf(mixed_text + at,
				       (unsigned int)(CON_CHARS - at),
				       "drwxr-xr-x  2 kitamura kitamura %5u "
				       "Oct 21 08:00 file%u.txt\r\n",
				       4096u + n, n);

		if (written <= 0) {
			break;
		}
		at += (unsigned long)written;
		++n;
	}
	while (at < CON_CHARS) {
		mixed_text[at++] = ' ';
	}
	mixed_text[CON_CHARS] = '\0';
}

int main(void)
{
	unsigned long i;
	int card;

	for (i = 0ul; i < CON_CHARS; ++i) {
		con_text[i] = (char)('a' + (char)(i % 26ul));
		space_text[i] = ' ';
	}
	con_text[CON_CHARS] = '\0';
	space_text[CON_CHARS] = '\0';
	make_mixed_text();

	timer_handle = timer_open();
	if (timer_handle < 0) {
		printf("iobench: SKIP no timer (%d)\r\n", timer_handle);
		return 1;
	}
	measure_clock_cost();
	if (clock_read_cost == 0ul) {
		/* The clock never moved across 66 readings: a stopped
		 * clock would make every row report zero. */
		printf("iobench: SKIP clock frozen\r\n");
		return 1;
	}

	printf("iobench: begin\r\n");

	vdp_hmmm_init();
	bench_ipc_null();
	bench_putchar();
	bench_printf();
	bench_write();
	bench_mixed();
	bench_space();
	bench_held();
	bench_hmmm(6u);
	bench_hmmm(12u);
	bench_direct(0u);
	bench_direct(1u);

	card = bench_file_create();
	if (card == 0) {
		bench_file_byte();
		bench_file_getc();
		bench_file_block();
		bench_file_remove();
	} else {
		row_skip("file.byte", card);
		row_skip("file.getc", card);
		row_skip("file.block", card);
	}

	sum_report();
	printf("iobench: end\r\n");
	return failures == 0u ? 0 : 2;
}
