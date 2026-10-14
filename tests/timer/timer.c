/* KonaOS monotonic-clock outcome program: `/system/timer.exe`.
 *
 * Two uses:
 *
 *   `timer`        the owner's stopwatch check: print one elapsed-second
 *                  line per second for a minute and exit, so the clock
 *                  the driver serves can be compared with a stopwatch.
 *                  A wait that gives up has hit a dead clock
 *                  (`timer_wait_us`'s bounded poll), which is reported
 *                  instead of hanging;
 *
 *   `timer stage`  the acceptance replay's checks in one run: the INFO
 *                  constants, 10,000 successive readings with the tick
 *                  interrupt arriving between them (each reading must be
 *                  at least the previous one), and the 1 s advance the
 *                  replay bounds - reported as its own `advance` line, so
 *                  the transcript carries the number the bounds apply to.
 *
 *   `timer card F` the owner's timing check over a card transfer: copy
 *                  `F` to `/fs/TIMERTMP.BIN` in 512-byte calls, delete
 *                  it, and print the elapsed clock as
 *                  `timer: card <counts> counts <s>.<t> s`.  The counts
 *                  are the driver's own, so a replay can hold them
 *                  against the model's clock: the difference is exactly
 *                  the ticks the CTC's one-deep request latch swallowed
 *                  while the IOC driver masked (0 when the transfer is
 *                  short against the 12.8 ms two-tick budget), which is
 *                  what the stopwatch check catches on a board.
 *
 * The stage verdict covers the INFO values and the monotonic run; the
 * advance and the card line are reported for the caller to judge.  Every
 * line is `timer: ...`, and the exit status is the verdict of the mode
 * that ran.
 */

#include "kos.h"
#include "kos/fs.h"
#include "kos/protocol.h"
#include "kos/timer.h"

#define STAGE_TAIL "stage"
#define CARD_TAIL  "card"

#define STAGE_READS      10000u
#define STEADY_SECONDS   60u
#define SECOND_US        1000000u

/* The clock constants INFO must carry (the driver's programming). */
#define COUNTS_PER_10S   390625u
#define COUNTS_PER_TICK  250u

/* `timer card`: one 512-byte call per transfer, and the card file the
 * copy leaves behind only long enough to delete it. */
#define CARD_TARGET      "/fs/TIMERTMP.BIN"
#define CARD_CHUNK       512u
/* Counts in a second: 390,625 per 10 s, i.e. 39,062.5; the integers are
 * what the printed seconds round from.  The program links no 32-bit
 * multiply or divide runtime, so both are subtraction loops. */
#define COUNTS_PER_SECOND 39062u
#define COUNTS_PER_TENTH  3906u

static char line[96];

static int same(const char *a, const char *b)
{
	while (*a == *b) {
		if (*a == '\0') {
			return 1;
		}
		++a;
		++b;
	}
	return 0;
}

static uint32_t reply_u32(const struct kos_ipc_frame *frame, unsigned char at)
{
	return (uint32_t)frame->payload[at] |
	       ((uint32_t)frame->payload[at + 1u] << 8) |
	       ((uint32_t)frame->payload[at + 2u] << 16) |
	       ((uint32_t)frame->payload[at + 3u] << 24);
}

/* The descriptor-less INFO call; 0 and both constants out, or a negative
 * error. */
static int timer_info(int handle, uint32_t *per_10s, uint32_t *per_tick)
{
	struct kos_ipc_request req;
	int result;

	req.frame.op = KTIMER_OP_INFO;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	result = call((unsigned char)handle, &req);
	if (result < 0) {
		return result;
	}
	*per_10s = reply_u32(&req.frame, 0u);
	*per_tick = reply_u32(&req.frame, 4u);
	return 0;
}

static void timer_send(const char *text)
{
	unsigned int length = 0u;

	while (text[length] != '\0') {
		++length;
	}
	(void)console_write(text, length);
}

/* `timer stage`: the checks, one verdict line each. */
static int timer_stage(void)
{
	uint32_t per_10s = 0u;
	uint32_t per_tick = 0u;
	uint32_t previous;
	uint32_t now;
	uint32_t before_wait;
	uint32_t advance;
	int handle;
	int info_ok;
	int mono_ok = 1;
	int waited;
	unsigned int reads;

	handle = timer_open();
	if (handle < 0) {
		(void)snprintf(line, sizeof(line), "timer: FAIL open %d\r\n",
				   handle);
		timer_send(line);
		return 1;
	}

	info_ok = (timer_info(handle, &per_10s, &per_tick) == 0) &&
		  (per_10s == COUNTS_PER_10S) && (per_tick == COUNTS_PER_TICK);
	(void)snprintf(line, sizeof(line), "timer: info %lu %lu %s\r\n",
			   (unsigned long)per_10s, (unsigned long)per_tick,
			   info_ok ? "OK" : "FAIL");
	timer_send(line);

	/* 10,000 readings; the tick interrupt lands between them. */
	if (timer_now(handle, &previous) < 0) {
		mono_ok = 0;
	}
	for (reads = 0u; mono_ok && reads < STAGE_READS; ++reads) {
		if (timer_now(handle, &now) < 0 || now < previous) {
			mono_ok = 0;
			break;
		}
		previous = now;
	}
	(void)snprintf(line, sizeof(line), "timer: mono %u %s\r\n",
			   (unsigned int)STAGE_READS, mono_ok ? "OK" : "FAIL");
	timer_send(line);

	/* One second of clock time, reported for the caller's bounds.  The
	 * reading is taken here, not reused from the loop: the line above
	 * costs milliseconds of console work, and that cost would land in
	 * the number the replay bounds.  A wait that gave up (the clock
	 * stopped advancing) fails the stage with its reason. */
	if (timer_now(handle, &before_wait) < 0) {
		mono_ok = 0;
		before_wait = 0u;
	}
	waited = timer_wait_us(handle, SECOND_US);
	if (waited < 0) {
		mono_ok = 0;
		(void)snprintf(line, sizeof(line), "timer: FAIL wait %d\r\n",
				   -waited);
		timer_send(line);
	}
	if (timer_now(handle, &now) < 0) {
		mono_ok = 0;
		advance = 0u;
	} else {
		advance = now - before_wait;
	}
	(void)snprintf(line, sizeof(line), "timer: advance %lu\r\n",
			   (unsigned long)advance);
	timer_send(line);
	(void)snprintf(line, sizeof(line), "timer: now %lu\r\n",
			   (unsigned long)now);
	timer_send(line);

	(void)unbind((unsigned char)handle);

	(void)snprintf(line, sizeof(line), "timer: stage %s\r\n",
			   (info_ok && mono_ok) ? "PASS" : "FAIL");
	timer_send(line);
	return (info_ok && mono_ok) ? 0 : 1;
}

/* `timer`: the owner's one-line-per-second stopwatch run. */
static int timer_steady(void)
{
	int handle = timer_open();
	unsigned int second;

	if (handle < 0) {
		(void)snprintf(line, sizeof(line), "timer: FAIL open %d\r\n",
				   handle);
		timer_send(line);
		return 1;
	}
	for (second = 1u; second <= STEADY_SECONDS; ++second) {
		int waited = timer_wait_us(handle, SECOND_US);

		if (waited < 0) {
			/* The clock stopped advancing: the wait gave up
			 * rather than hanging, so the run reports why. */
			(void)snprintf(line, sizeof(line),
					   "timer: FAIL wait %d after %u s\r\n",
					   -waited, second - 1u);
			timer_send(line);
			(void)unbind((unsigned char)handle);
			return 1;
		}
		(void)snprintf(line, sizeof(line), "timer: %u s\r\n", second);
		timer_send(line);
	}
	(void)unbind((unsigned char)handle);
	return 0;
}

/* A card-mode failure line; the caller carries the verdict. */
static int timer_card_fail(const char *what, int status)
{
	(void)snprintf(line, sizeof(line), "timer: card FAIL %s %d\r\n",
			   what, status);
	timer_send(line);
	return 1;
}

/* `timer card F`: copy the card file `source` in 512-byte calls, remove
 * the target, and print the elapsed clock.  The transfer is what the
 * clock is measured over: its calls are the long syscalls a masked body
 * would lose ticks in. */
static int timer_card(const char *source)
{
	static char buffer[CARD_CHUNK];
	uint32_t before = 0u;
	uint32_t after = 0u;
	uint32_t elapsed;
	uint32_t rest;
	uint32_t seconds;
	uint32_t tenths;
	int handle;
	int in;
	int out;
	int status = 0;

	in = open(source, KFS_O_READ);
	if (in < 0) {
		return timer_card_fail("open", -in);
	}
	out = open(CARD_TARGET, KFS_O_WRITE | KFS_O_CREATE | KFS_O_TRUNC);
	if (out < 0) {
		(void)close((unsigned int)in);
		return timer_card_fail("create", -out);
	}
	handle = timer_open();
	if (handle < 0) {
		(void)close((unsigned int)in);
		(void)close((unsigned int)out);
		return timer_card_fail("timer", -handle);
	}
	if (timer_now(handle, &before) < 0) {
		status = KOS_EIO;
	}
	for (; status == 0;) {
		int count = read((unsigned int)in, buffer, CARD_CHUNK);
		int done = 0;

		if (count < 0) {
			status = -count;
			break;
		}
		if (count == 0) {
			break;                     /* end of file */
		}
		/* A write may complete in part: the position advanced with
		 * it, so the rest goes out as the next chunk. */
		while (done < count) {
			int written = write((unsigned int)out, &buffer[done],
					    (unsigned int)(count - done));

			if (written < 0) {
				status = -written;
				break;
			}
			if (written == 0) {
				status = KOS_EIO;  /* no progress */
				break;
			}
			done += written;
		}
	}
	if (status == 0) {
		int flushed = sync((unsigned int)out);

		if (flushed < 0) {
			status = -flushed;
		}
	}
	(void)close((unsigned int)in);
	(void)close((unsigned int)out);
	if (timer_now(handle, &after) < 0) {
		status = KOS_EIO;
	}
	(void)unbind((unsigned char)handle);
	if (status != 0) {
		(void)unlink(CARD_TARGET);
		return timer_card_fail("copy", status);
	}
	elapsed = after - before;
	status = unlink(CARD_TARGET);
	if (status < 0) {
		return timer_card_fail("unlink", -status);
	}
	rest = elapsed;
	seconds = 0u;
	while (rest >= COUNTS_PER_SECOND) {
		rest -= COUNTS_PER_SECOND;
		++seconds;
	}
	tenths = 0u;
	while (tenths < 9u && rest >= COUNTS_PER_TENTH) {
		rest -= COUNTS_PER_TENTH;
		++tenths;
	}
	(void)snprintf(line, sizeof(line),
			   "timer: card %lu counts %lu.%lu s\r\n",
			   (unsigned long)elapsed, (unsigned long)seconds,
			   (unsigned long)tenths);
	timer_send(line);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 1 && argv != (char **)0 && same(argv[0], STAGE_TAIL)) {
		return timer_stage();
	}
	if (argc == 2 && argv != (char **)0 &&
	    argv[1] != (char *)0 && same(argv[0], CARD_TAIL)) {
		return timer_card(argv[1]);
	}
	return timer_steady();
}
