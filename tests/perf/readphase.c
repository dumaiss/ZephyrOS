/* KonaOS read-path measurement program (tests/perf/).
 *
 *     readphase FILE [BLOCKS]
 *
 * Reads FILE from its start in 512-byte calls and then once in 128-byte
 * calls, fetches the instrumented provider's phase table after every read
 * (KFILE_OP_PHASES, abi/kos/protocol.h) and prints, per pass and per
 * phase, min/mean/max in ms.  Nothing is printed while a pass runs.
 *
 * The table exists only in a `KOS_IOC_PHASES` build (`make phase-image`);
 * an image without it answers the fetch with nothing - no payload, or the
 * kernel's own refusal - and the program says so and stops.  The phase clock is the instrument's
 * own CTC pair, so its unit is the same 25.6 us count the TIMER driver
 * reports; the per-pass `timer` line brackets the read calls with a
 * reading of that driver, and the two agree except for whatever the
 * interrupt-masked transport window hides from the tick counter - a
 * result to read here, not an error for this program to fix.
 *
 * Three passes, each re-reading from the start with the provider's cache
 * dropped and its read-ahead off (KFILE_PHASE_CTL_COLD, _SINGLE: one card
 * transfer per missed block), because the copy path of a read is the
 * nucleus's choice for the *caller's* buffer (architecture 4.9):
 *
 *   pass 1  512-byte reads into this program's static buffer, below
 *           8000h: the caller's own bank, clear of the aperture, so the
 *           one-LDIR mode-10 path (B) carries the bytes;
 *   pass 2  512-byte reads into 8000h of the caller's own bank: an
 *           aperture-range address, so the nucleus must stage the copy
 *           through the bank gate (path C) even though the memory is the
 *           program's own;
 *   pass 3  128-byte reads into the static buffer: four reads per
 *           512-byte block, so three of them answer from the provider's
 *           cache and only the first carries the transport's phases.
 *
 * The provider cannot see the caller's buffer - that is what a descriptor
 * is for - so the table it reports leaves the path as "unknown"; this
 * program fills it in from the buffer it passed.  The validation replay
 * checks that classification against the nucleus's own path counters.
 *
 * The `sum` line is the arithmetic's own check: the consecutive phase
 * differences of one block telescope, so the stamped ones must add up to
 * the ENTERED-to-RETURN difference.  A deviation beyond
 * PHASE_SUM_TOLERANCE counts means an alias (a phase longer than the
 * 0.8192 s window) or a wrongly resolved pair, and that invalidates the
 * block.  A pair of stamps that cannot be resolved at all (see
 * phase_counts) is counted as torn and the block is left out of the check
 * rather than answered wrongly; the line prints both counts, so a pass
 * says how much of its data it could verify.
 *
 * A phase's row is the time from the step before it to it, so each row
 * reads as what that step cost to reach: `last` is the payload transfer,
 * `first` the admission's tail, and `entered` - the read's first step -
 * has no step before it and never carries a time.
 */

#include "kos.h"
#include "kos/protocol.h"
#include "kos/syscall.h"
#include "kos/timer.h"

#define PASS_COUNT 3
#define PHASE_COUNT ((unsigned int)KFILE_PHASE_COUNT)

#define DEFAULT_BLOCKS 256u
#define MAX_BLOCKS     4096u
#define BLOCK_READ     512u
#define SMALL_READ     128u

/* The channel cycles the stamps resolve through: 256 counts on CTC
 * channel 1 (the instrument's own programming, in the provider) and 250
 * on channel 2 (the timer driver's tick, KOS_TIMER_COUNTS_PER_TICK of
 * abi/kos/protocol.h).  The pair's least common multiple, 32,000 counts =
 * 0.8192 s, bounds one difference. */
#define PHASE_CYCLE_FAST 256u
#define PHASE_CYCLE_TICK ((unsigned int)KOS_TIMER_COUNTS_PER_TICK)

/* The sum check's allowance, in counts, for a verified block.  A pair's
 * two readings are a handful of T-states apart and share one latency, so
 * a resolved difference is exact but for the endpoints' read latency (a
 * count at most, 25.6 us); an interrupt landing between a pair's two
 * reads can shift the second reading a count or two without breaking the
 * pair's parity, and the door-to-door sum carries the first and last
 * stamp's shifts.  Four counts leaves room for that without hiding what
 * the check is for: a wrong resolution is a whole cycle out (256 counts),
 * not a few. */
#define PHASE_SUM_TOLERANCE 4u

/* The aperture range of a caller's bank: an address in it forces the
 * nucleus's staged copy path (4.9). */
#define CALLER_APERTURE_BASE 0x8000u
#define CALLER_APERTURE_END  0xC000u

/* The read buffers.  The high pass names address 8000h of this program's
 * own bank: free memory between the image and the stack, reached only
 * while the program runs with the whole bank mapped, and named by a
 * pointer constant because the language places no object absolutely.  The
 * validation replay checks the link map keeps the program's own data well
 * below it. */
static unsigned char low_buffer[BLOCK_READ];
static unsigned char small_buffer[SMALL_READ];

/* One row per pass and phase, kept in the instrument's own count unit
 * until the report turns it into microseconds. */
static unsigned int phase_calls[PASS_COUNT][KFILE_PHASE_COUNT];
static unsigned int phase_min[PASS_COUNT][KFILE_PHASE_COUNT];
static unsigned int phase_max[PASS_COUNT][KFILE_PHASE_COUNT];
static unsigned long phase_sum[PASS_COUNT][KFILE_PHASE_COUNT];
static unsigned int pass_reads[PASS_COUNT];
static unsigned int pass_misses[PASS_COUNT];
static unsigned int pass_hits[PASS_COUNT];
static unsigned int pass_full[PASS_COUNT];
static unsigned int pass_bad[PASS_COUNT];
static unsigned int pass_worst[PASS_COUNT];
static unsigned int pass_torn[PASS_COUNT];
static unsigned int pass_timer_calls[PASS_COUNT];
static unsigned int pass_timer_min[PASS_COUNT];
static unsigned int pass_timer_max[PASS_COUNT];
static unsigned long pass_timer_sum[PASS_COUNT];
static unsigned int pass_buffer[PASS_COUNT];
static unsigned int pass_size[PASS_COUNT];
static unsigned char pass_path[PASS_COUNT];
static int pass_error[PASS_COUNT];

static int timer_handle;
static int file_handle;

/* The phase names as printed, in KFILE_PHASE_* order. */
static const char *const phase_name[KFILE_PHASE_COUNT] = {
	"entered", "cache", "sent", "reply", "admit",
	"first", "last", "copy", "caller", "return"
};

/* --- the timestamps ---------------------------------------------------- */

/* A pair of stamps whose two channels disagree on parity was read while
 * one channel stepped between its two readings (both count one per 256
 * T-states, so a clean pair's cycle differences are congruent modulo 2).
 * The difference is then one of four neighbours, one count either way on
 * either channel; the true one is within a count of the answer and the
 * other parities land about 10,000 counts away, so the nearest candidate
 * is taken while it is shorter than PHASE_STEP_LIMIT.  Past that limit
 * the pair is torn: dropped, not answered.  The sum check verifies every
 * block a resolved pair belongs to. */
#define PHASE_COUNTS_TORN 0xFFFFu
#define PHASE_STEP_LIMIT 4000u /* counts: 102.4 ms */

/* One count difference from the two channels' cycle differences.  With
 * the unknown difference `d`, d = first (mod 256) and d = tick (mod 250),
 * so d = first + 256k with 6k = tick - first (mod 250), i.e.
 * 3k = (tick - first)/2 (mod 125) and k = 42 * ((tick - first)/2) mod 125,
 * because 42 * 3 = 126 = 1 (mod 125).  The value is unique inside the
 * 32,000-count window and the resolution is one count, 25.6 us; an odd
 * difference has no value at all. */
static unsigned int phase_resolve(unsigned int first, unsigned int tick)
{
	unsigned int half;
	unsigned int turns;

	if (((tick - first) & 1u) != 0u) {
		return PHASE_COUNTS_TORN;
	}
	half = (tick + (2u * PHASE_CYCLE_TICK) - first)
	       % PHASE_CYCLE_TICK / 2u;
	turns = (42u * half) % (PHASE_CYCLE_TICK / 2u);

	return first + PHASE_CYCLE_FAST * turns;
}

/* One count difference from two stamps, each a pair of channel readings
 * advancing in its cycle. */
static unsigned int phase_counts(unsigned char a1, unsigned char a2,
				 unsigned char b1, unsigned char b2)
{
	unsigned int first = (unsigned int)(unsigned char)(a2 - a1);
	int tick_delta = (int)(unsigned char)b2 - (int)(unsigned char)b1;
	unsigned int tick = (unsigned int)(((tick_delta % (int)PHASE_CYCLE_TICK)
					    + (int)PHASE_CYCLE_TICK)
					   % (int)PHASE_CYCLE_TICK);
	unsigned int best = phase_resolve(first, tick);
	unsigned int candidate[4];
	unsigned int i;

	if (best != PHASE_COUNTS_TORN) {
		return best;
	}
	candidate[0] = phase_resolve((first + 1u) & 0xFFu, tick);
	candidate[1] = phase_resolve((first + 255u) & 0xFFu, tick);
	candidate[2] = phase_resolve(first, (tick + 1u) % PHASE_CYCLE_TICK);
	candidate[3] = phase_resolve(first, (tick + PHASE_CYCLE_TICK - 1u)
					    % PHASE_CYCLE_TICK);
	for (i = 0u; i < 4u; ++i) {
		if (candidate[i] < best) {
			best = candidate[i];
		}
	}
	return best < PHASE_STEP_LIMIT ? best : PHASE_COUNTS_TORN;
}

/* The 4.9 copy path of a descriptor over this buffer: A when the driver's
 * mode-11 mapping shows it whole (the low caller window, or common), B
 * when it lies in the caller's own bank clear of the aperture, C
 * otherwise (inside 8000h-BFFFh, or a far bank behind it). */
static unsigned char copy_path(const unsigned char *buffer,
			       unsigned int length)
{
	unsigned long base = (unsigned long)(unsigned int)buffer;
	unsigned long end = base + (unsigned long)length;

	if ((base >= (unsigned long)KOS_USER_BUFFER_BASE &&
	     end <= (unsigned long)KOS_SYSCALL_WINDOW_END) ||
	    (base >= (unsigned long)KOS_USER_BUFFER_END &&
	     end <= 0x10000ul)) {
		return KFILE_PHASE_PATH_A;
	}
	if (base >= (unsigned long)KOS_USER_BUFFER_BASE &&
	    end <= (unsigned long)KOS_USER_BUFFER_END &&
	    !(base < (unsigned long)CALLER_APERTURE_END &&
	      end > (unsigned long)CALLER_APERTURE_BASE)) {
		return KFILE_PHASE_PATH_B;
	}
	return KFILE_PHASE_PATH_C;
}

static const char path_letter[3] = { 'A', 'B', 'C' };

/* --- the fetch --------------------------------------------------------- */

/* One KFILE_OP_PHASES call: no descriptor, the packed table back in the
 * reply payload.  The payload is zeroed first so an image that does not
 * serve the operation cannot be mistaken for a provider that answered: a
 * dispatched call leaves the untouched zeroes behind (the kernel's own
 * refusal returns a value the call wrapper cannot carry), while a served
 * one always has the table's ENTERED bit set.  Returns -KOS_ENOTSUP for
 * that all-zero answer, -KOS_EINVAL or another refusal code from a
 * provider that refuses the operation itself. */
static int fetch_phases(unsigned int handle, struct kfile_phases *out,
			unsigned char control)
{
	struct kos_ipc_request req;
	unsigned char *dst = (unsigned char *)out;
	unsigned char *src = (unsigned char *)&req.frame.payload[0];
	unsigned int i;
	int result;

	req.frame.op = KFILE_OP_PHASES;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = control;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	for (i = 0u; i < (unsigned int)sizeof(struct kfile_phases); ++i) {
		src[i] = 0u;
	}
	result = call((unsigned char)handle, &req);
	if (result < 0) {
		return result;
	}
	for (i = 0u; i < (unsigned int)sizeof(struct kfile_phases); ++i) {
		dst[i] = src[i];
	}
	if (out->valid == 0u && out->flags == 0u && out->path == 0u) {
		return -KOS_ENOTSUP;
	}
	return 0;
}

/* --- accumulation ------------------------------------------------------ */

/* Fold one read's table into its pass.  A difference the two channels
 * cannot agree on (a torn pair, see phase_counts) is left out of its
 * row's samples and marks the read as unverifiable: its own time is
 * unknown, but the other steps' are still good. */
static void note_read(unsigned int pass, const struct kfile_phases *table)
{
	unsigned int i;
	unsigned char full = 1u;
	unsigned char torn = 0u;
	unsigned long total = 0ul;

	++pass_reads[pass];
	if ((table->flags & KFILE_PHASE_FLAG_CACHE_HIT) != 0u) {
		++pass_hits[pass];
	} else {
		++pass_misses[pass];
	}
	for (i = 0u; i < PHASE_COUNT; ++i) {
		unsigned int counts;

		if ((table->valid & (unsigned int)(1u << i)) == 0u) {
			full = 0u;
			continue;
		}
		if (i == 0u ||
		    (table->valid & (unsigned int)(1u << (i - 1u))) == 0u) {
			continue; /* no previous stamp to difference from */
		}
		counts = phase_counts(table->elapsed_1[i - 1u],
				      table->elapsed_1[i],
				      table->elapsed_2[i - 1u],
				      table->elapsed_2[i]);
		if (counts == PHASE_COUNTS_TORN) {
			torn = 1u;
			continue;
		}
		if (phase_calls[pass][i] == 0u ||
		    counts < phase_min[pass][i]) {
			phase_min[pass][i] = counts;
		}
		if (counts > phase_max[pass][i]) {
			phase_max[pass][i] = counts;
		}
		phase_sum[pass][i] += (unsigned long)counts;
		++phase_calls[pass][i];
		total += (unsigned long)counts;
	}
	if (torn != 0u) {
		++pass_torn[pass];
	} else if (full != 0u) {
		unsigned int span = phase_counts(
			table->elapsed_1[KFILE_PHASE_ENTERED],
			table->elapsed_1[KFILE_PHASE_RETURN],
			table->elapsed_2[KFILE_PHASE_ENTERED],
			table->elapsed_2[KFILE_PHASE_RETURN]);

		if (span == PHASE_COUNTS_TORN) {
			++pass_torn[pass];
		} else {
			unsigned int deviation =
				(span > (unsigned int)total)
				? span - (unsigned int)total
				: (unsigned int)total - span;

			++pass_full[pass];
			if (deviation > pass_worst[pass]) {
				pass_worst[pass] = deviation;
			}
			if (deviation > PHASE_SUM_TOLERANCE) {
				++pass_bad[pass];
			}
		}
	}
}

static void note_timer(unsigned int pass, unsigned long counts)
{
	if (pass_timer_calls[pass] == 0u ||
	    counts < (unsigned long)pass_timer_min[pass]) {
		pass_timer_min[pass] = (unsigned int)counts;
	}
	if (counts > (unsigned long)pass_timer_max[pass]) {
		pass_timer_max[pass] = (unsigned int)counts;
	}
	pass_timer_sum[pass] += counts;
	++pass_timer_calls[pass];
}

/* --- the passes -------------------------------------------------------- */

/* Read `reads` times `size` bytes into `buffer`, fetching and folding the
 * table after each one.  Returns 0, or -1 when a read or a fetch failed:
 * the pass report stays silent about it, and main names the pass. */
static int run_pass(unsigned int pass, unsigned int size,
		    unsigned char *buffer, unsigned int reads)
{
	unsigned int i;

	pass_size[pass] = size;
	pass_buffer[pass] = (unsigned int)buffer;
	pass_path[pass] = copy_path(buffer, size);
	/* Every pass starts from the card: the provider's cache is dropped,
	 * and each miss fetches only the block it needs. */
	{
		struct kfile_phases table;
		int cold = fetch_phases((unsigned int)file_handle, &table,
					KFILE_PHASE_CTL_COLD |
					KFILE_PHASE_CTL_SINGLE);

		if (cold != 0) {
			pass_error[pass] = cold;
			return -1;
		}
	}
	for (i = 0u; i < reads; ++i) {
		struct kfile_phases table;
		uint32_t start = 0u;
		int got;

		if (timer_handle >= 0 && timer_now(timer_handle, &start) < 0) {
			timer_handle = -1; /* the phase rows stand alone */
		}
		got = read(file_handle, buffer, size);
		if (got <= 0) {
			if (got < 0) {
				pass_error[pass] = got;
				return -1;
			}
			break; /* end of file: a short pass is reported */
		}
		if (timer_handle >= 0) {
			uint32_t now;

			if (timer_now(timer_handle, &now) == 0) {
				note_timer(pass, (unsigned long)(now - start));
			}
		}
		{
			int fetched = fetch_phases((unsigned int)file_handle,
						   &table,
						   KFILE_PHASE_CTL_SINGLE);

			if (fetched != 0) {
				pass_error[pass] = fetched;
				return -1;
			}
		}
		table.path = pass_path[pass];
		note_read(pass, &table);
	}
	return 0;
}

/* --- the report -------------------------------------------------------- */

/* `us` as milliseconds with three decimals, in a fixed-width field. */
static void print_ms(unsigned long us)
{
	printf("%5lu.%03lu", us / 1000ul, us % 1000ul);
}

static void print_phase_row(unsigned int pass, unsigned int phase)
{
	unsigned int calls = phase_calls[pass][phase];

	printf("  %8s %5u", phase_name[phase], calls);
	if (calls == 0u) {
		printf("         -\r\n");
		return;
	}
	print_ms((unsigned long)timer_us(phase_min[pass][phase]));
	print_ms((unsigned long)timer_us(
			 (uint32_t)(phase_sum[pass][phase] /
				    (unsigned long)calls)));
	print_ms((unsigned long)timer_us(phase_max[pass][phase]));
	printf("\r\n");
}

static void print_pass(unsigned int pass)
{
	unsigned int phase;

	printf("pass %u: %u-byte reads into %04xh, path %c, %u reads",
	       pass + 1u, pass_size[pass], pass_buffer[pass],
	       path_letter[pass_path[pass]], pass_reads[pass]);
	printf(", %u misses, %u hits\r\n", pass_misses[pass],
	       pass_hits[pass]);
	printf("  %8s %5s %8s %8s %8s\r\n", "phase", "n", "min", "mean",
	       "max");
	for (phase = 0u; phase < PHASE_COUNT; ++phase) {
		print_phase_row(pass, phase);
	}
	if (pass_timer_calls[pass] != 0u) {
		printf("  %8s %5u", "timer", pass_timer_calls[pass]);
		print_ms((unsigned long)timer_us(pass_timer_min[pass]));
		print_ms((unsigned long)timer_us(
				 (uint32_t)(pass_timer_sum[pass] /
					    (unsigned long)pass_timer_calls
						    [pass])));
		print_ms((unsigned long)timer_us(pass_timer_max[pass]));
		printf("\r\n");
	}
	printf("  %8s %5u of %u fully stamped, %u mismatch, %u torn,"
	       " worst %u counts\r\n", "sum", pass_full[pass],
	       pass_reads[pass], pass_bad[pass], pass_torn[pass],
	       pass_worst[pass]);
}

/* --- start ------------------------------------------------------------ */

static unsigned int parse_blocks(const char *text)
{
	unsigned int value = 0u;

	if (text == (const char *)0 || *text == '\0') {
		return 0u;
	}
	while (*text != '\0') {
		if (*text < '0' || *text > '9') {
			return 0u;
		}
		if (value > (MAX_BLOCKS / 10u)) {
			return 0u;
		}
		value = value * 10u + (unsigned int)(*text - '0');
		++text;
	}
	return (value != 0u && value <= MAX_BLOCKS) ? value : 0u;
}

int main(int argc, char **argv)
{
	unsigned int blocks = DEFAULT_BLOCKS;
	unsigned int pass;
	unsigned int failed = 0u;
	int result = 0;

	if (argc < 1) {
		printf("usage: readphase FILE [BLOCKS]\r\n");
		return 2;
	}
	if (argc > 1) {
		blocks = parse_blocks(argv[1]);
		if (blocks == 0u) {
			printf("readphase: bad block count\r\n");
			return 2;
		}
	}
	if ((unsigned int)&low_buffer >= CALLER_APERTURE_BASE ||
	    (unsigned int)&small_buffer >= CALLER_APERTURE_BASE) {
		printf("readphase: program data is not below 8000h\r\n");
		return 2;
	}
	printf("readphase: begin\r\n");
	file_handle = open(argv[0], KFS_O_READ);
	if (file_handle < 0) {
		printf("readphase: %s: %d\r\n", argv[0], file_handle);
		return 1;
	}
	timer_handle = timer_open();
	if (timer_handle < 0) {
		printf("readphase: no clock (%d), no timer rows\r\n",
		       timer_handle);
	}
	/* Three passes over the same bytes; the seeks put every pass's
	 * first read on a fresh 512-byte block. */
	if (run_pass(0u, BLOCK_READ, low_buffer, blocks) != 0) {
		failed = 1u;
	} else if (seek(file_handle, 0l, KFS_SEEK_SET) < 0l) {
		failed = 2u;
	} else if (run_pass(1u, BLOCK_READ,
			    (unsigned char *)CALLER_APERTURE_BASE, blocks)
		   != 0) {
		failed = 3u;
	} else if (seek(file_handle, 0l, KFS_SEEK_SET) < 0l) {
		failed = 4u;
	} else if (run_pass(2u, SMALL_READ, small_buffer,
			    blocks * (BLOCK_READ / SMALL_READ)) != 0) {
		failed = 5u;
	}
	for (pass = 0u; pass < PASS_COUNT; ++pass) {
		if (pass_reads[pass] != 0u) {
			print_pass(pass);
		}
	}
	if (failed != 0u) {
		unsigned int broken = (failed + 1u) / 2u - 1u;

		if (pass_error[broken] == -KOS_ENOTSUP) {
			printf("readphase: no phase table: this image "
			       "carries no read-path instrument\r\n");
		} else if ((failed & 1u) != 0u) {
			printf("readphase: pass %u failed: %d\r\n",
			       broken + 1u, pass_error[broken]);
		} else {
			printf("readphase: pass %u failed: seek\r\n",
			       broken + 1u);
		}
		result = 1;
	} else if (pass_reads[0] < (blocks / 2u)) {
		printf("readphase: file too short for %u blocks\r\n", blocks);
		result = 1;
	}
	(void)close((unsigned int)file_handle);
	if (timer_handle >= 0) {
		(void)close((unsigned int)timer_handle);
	}
	printf("readphase: end\r\n");
	return result;
}
