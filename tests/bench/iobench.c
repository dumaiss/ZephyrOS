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
 *   file.byte    a 32 KiB card file read one byte per read;
 *   file.getc    the same file one byte per getc, through the stream
 *                buffer (one card read per 512);
 *   file.block   the same file in 512-byte reads.
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

static void row_report(void)
{
	unsigned long mean = row_total / row_ops;
	unsigned long per_byte = row_bytes != 0ul ?
				 row_total / row_bytes : 0ul;

	printf("iobench: %s ops %lu bytes %lu min %lu mean %lu max %lu "
	       "per-byte %lu\r\n", row_name, row_ops, row_bytes, row_min,
	       mean, row_max, per_byte);
}

static void row_skip(const char *name, int error)
{
	printf("iobench: %s SKIP no card (%d)\r\n", name, error);
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

int main(void)
{
	unsigned long i;
	int card;

	for (i = 0ul; i < CON_CHARS; ++i) {
		con_text[i] = (char)('a' + (char)(i % 26ul));
	}
	con_text[CON_CHARS] = '\0';

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

	bench_ipc_null();
	bench_putchar();
	bench_printf();
	bench_write();

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

	printf("iobench: end\r\n");
	return failures == 0u ? 0 : 2;
}
