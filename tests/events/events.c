/* KonaOS device-event outcome program: `/system/events.exe`.
 *
 * It is the acceptance replay's window on the event mechanism (architecture
 * 4.5/4.10): an application subscribes to the timer driver's event timer
 * with `notify`/`timer_event`, the driver raises events from its channel-3
 * interrupt, and the nucleus delivers them to `events_on_timer` at a safe
 * point of this program's own execution.  Modes, all run from the monitor's
 * `X`:
 *
 *   `events watch`   subscribe, arm 20 ms (782 counts) and wait for 50
 *                    deliveries.  The handler writes one byte to the
 *                    console (a syscall from inside a handler) and reads the
 *                    clock, so the replay gets the delivery count, the grid
 *                    drift and the elapsed counts.  This is also the
 *                    measurement run: 50 deliveries per simulated second.
 *                    The grid figures are reported for the replay's clean
 *                    run (and the owner's board check) to assert; the
 *                    verdict below holds whatever the machine's masking.
 *   `events loop`    the same subscription while the main loop writes lines
 *                    to the console and transfers a card file: events that
 *                    land inside those calls are held (`missed` stays 0)
 *                    and delivered when the program runs again.
 *   `events run P`   run a child while the subscription is armed: the parent
 *                    is suspended for the whole run, so its events are
 *                    dropped (missed grows) and none is delivered inside the
 *                    child's own console output.
 *   `events exit`    subscribe, arm and exit with the subscription active;
 *                    the release that follows must stop the source and free
 *                    the subscription, so a later program can subscribe.
 *
 * Every line is `events: ...`; the verdict is `events: PASS` or `events:
 * FAIL <what>`, and the exit status is the verdict.
 */

#include "kos.h"
#include "kos/event.h"
#include "kos/protocol.h"
#include "kos/timer.h"

/* 20 ms of 25.6 µs counts: 781.25, so 782; the driver keeps the deadline
 * exact from count to count whatever the period's granularity. */
#define EVENTS_PERIOD 782u

#define EVENTS_CARD  "/fs/EVENTS.TMP"
#define EVENTS_BLOCK 512u

/* One spin bound for the wait loops; the replay's step limit is the real
 * guard, this only keeps a dead clock from spinning forever in silence. */
#define EVENTS_SPIN 0x3FFFFFFFul

static int event_handle;

static volatile unsigned int event_deliveries;
static volatile unsigned int event_counts;
static volatile unsigned int event_missed;
static volatile unsigned int event_console;
static volatile uint32_t event_origin;
static volatile uint32_t event_grid;
static volatile unsigned int event_grid_set;
static volatile unsigned int event_drift;
/* The signed errors of the intervals between deliveries, and how many
 * deliveries were further than the tolerance from the grid.  The grid is
 * exact - it is the driver's own count arithmetic - so what moves a reading
 * off it is the interrupt that delayed one expiry and the clock call this
 * handler makes.  A drift would have to show up in the intervals, where the
 * starting reading cancels; the distances themselves are anchored to the
 * first delivery, which is why only their worst case bounds them. */
static volatile long event_intervals;
static volatile uint32_t event_prev;
static volatile unsigned int event_late;
static volatile unsigned int event_zero;
static volatile uint32_t event_last;

static unsigned char event_block[EVENTS_BLOCK];

/* The handler: a console write (a syscall from handler context) and one
 * clock reading.  The first reading is the grid's origin, every later one
 * must land on origin + k * period, and the worst distance from that grid
 * is the drift the replay bounds. */
void events_on_timer(unsigned int count, unsigned int missed)
{
	uint32_t now = 0u;
	long error;

	event_counts += count;
	/* Both counters arrive as the coalesced backlog since the last
	 * delivery, so they accumulate: the child-run mode needs the misses
	 * that built up while it was suspended, and a later delivery would
	 * otherwise overwrite them with its own zero. */
	event_missed += missed;
	++event_deliveries;
	if (count == 0u) {
		++event_zero;
	}
	if (timer_now(event_handle, &now) != 0) {
		return;
	}
	if (event_grid_set == 0u) {
		event_origin = now;
		event_grid = now;
		event_grid_set = 1u;
	} else {
		event_grid += EVENTS_PERIOD;
		error = (long)now - (long)event_grid;
		if (error < 0l) {
			error = -error;
		}
		event_intervals += (long)now - (long)event_prev - (long)EVENTS_PERIOD;
		if ((unsigned int)error > 2u) {
			++event_late;
		}
		if ((unsigned int)error > event_drift) {
			event_drift = (unsigned int)error;
		}
	}
	event_prev = now;
	event_last = now;
	if (event_console != 0u) {
		(void)console_write("E", 1u);
	}
}

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

static int fail(const char *what)
{
	printf("events: FAIL %s\r\n", what);
	return 1;
}

/* Subscribe and arm; NULL on success, the failing step's name otherwise. */
static const char *start(unsigned int console_in_handler)
{
	int result;

	event_handle = timer_open();
	if (event_handle < 0) {
		printf("events: FAIL open %d\r\n", event_handle);
		return "open";
	}
	result = notify(event_handle, events_on_timer);
	if (result < 0) {
		printf("events: FAIL notify %d\r\n", result);
		return "notify";
	}
	event_console = console_in_handler;
	result = timer_event(event_handle, EVENTS_PERIOD);
	if (result < 0) {
		printf("events: FAIL event %d\r\n", result);
		return "event";
	}
	return (const char *)0;
}

/* Stop the source, unsubscribe and drop the handle: the release is the
 * driver's signal to stop the channel. */
static void stop(void)
{
	(void)timer_event(event_handle, 0u);
	(void)notify(event_handle, (kos_event_handler_t)0);
	(void)unbind((unsigned char)event_handle);
	event_handle = 0;
}

static int mode_watch(void)
{
	const char *bad = start(1u);
	unsigned long guard = 0u;

	if (bad != (const char *)0) {
		return 1;
	}
	while (event_deliveries < 50u && guard < EVENTS_SPIN) {
		++guard;
	}
	if (event_deliveries < 50u) {
		return fail("deliveries");
	}
	stop();
	printf("events: watch %u deliveries counts %u missed %u drift %u late %u "
	       "intervals %ld elapsed %lu\r\n",
	       event_deliveries, event_counts, event_missed, event_drift,
	       event_late, event_intervals,
	       (unsigned long)(event_last - event_origin));
	if (event_counts < event_deliveries) {
		return fail("watch-counts");
	}
	if (event_zero != 0u) {
		return fail("watch-count-empty");
	}
	if (event_missed != 0u) {
		return fail("watch-missed");
	}
	/* The grid figures above are reported, not judged: an emulated card
	 * lane can leave the machine masked for whole periods, which moves a
	 * delivery's own reading without the driver's deadlines moving at
	 * all.  The replay asserts them in the run without that peer, and the
	 * owner checks them on the board; what must hold under any masking -
	 * no lost or doubled event, every delivery carrying at least one
	 * deadline, nothing missed while this program runs - is checked
	 * here. */
	printf("events: PASS\r\n");
	return 0;
}

static int mode_loop(void)
{
	const char *bad = start(1u);
	unsigned long guard = 0u;
	unsigned int i;
	int file;

	(void)unlink(EVENTS_CARD);
	for (i = 0u; i < EVENTS_BLOCK; ++i) {
		event_block[i] = (unsigned char)(i & 0xFFu);
	}
	while (event_deliveries < 25u && guard < EVENTS_SPIN) {
		++guard;
		printf("events: loop %u\r\n", event_deliveries);
		file = open(EVENTS_CARD,
			    KFS_O_CREATE | KFS_O_TRUNC | KFS_O_WRITE);
		if (file < 0) {
			return fail("card-open");
		}
		if (write((unsigned int)file, event_block, EVENTS_BLOCK) !=
		    (int)EVENTS_BLOCK) {
			(void)close((unsigned int)file);
			return fail("card-write");
		}
		if (close((unsigned int)file) < 0) {
			return fail("card-close");
		}
		file = open(EVENTS_CARD, KFS_O_READ);
		if (file < 0) {
			return fail("card-reopen");
		}
		if (read((unsigned int)file, event_block, EVENTS_BLOCK) !=
		    (int)EVENTS_BLOCK) {
			(void)close((unsigned int)file);
			return fail("card-read");
		}
		if (close((unsigned int)file) < 0) {
			return fail("card-close-read");
		}
	}
	if (event_deliveries < 25u) {
		return fail("loop-deliveries");
	}
	stop();
	(void)unlink(EVENTS_CARD);
	printf("events: loop %u deliveries counts %u missed %u rounds %lu\r\n",
	       event_deliveries, event_counts, event_missed, guard);
	if (event_missed != 0u) {
		return fail("loop-missed");
	}
	if (event_counts < event_deliveries) {
		return fail("loop-counts");
	}
	printf("events: PASS\r\n");
	return 0;
}

static int mode_run(const char *path, const char *tail)
{
	const char *bad = start(1u);
	unsigned int before;
	unsigned int missed_before;
	unsigned int after;
	unsigned int missed_after;
	int status;

	if (bad != (const char *)0) {
		return 1;
	}
	before = event_deliveries;
	missed_before = event_missed;
	printf("events: run before %u/%u\r\n", before, missed_before);
	status = run(path, tail);
	after = event_deliveries;
	missed_after = event_missed;
	stop();
	printf("events: run %s status %d before %u after %u counts %u "
	       "missed %u->%u\r\n",
	       path, status, before, after, event_counts, missed_before,
	       missed_after);
	if (status < 0) {
		return fail("run");
	}
	if (missed_after <= missed_before) {
		return fail("run-missed");
	}
	if (event_counts < after) {
		return fail("run-counts");
	}
	printf("events: PASS\r\n");
	return 0;
}

static int mode_exit(void)
{
	const char *bad = start(0u);

	if (bad != (const char *)0) {
		return 1;
	}
	printf("events: exit armed\r\n");
	exit(0);
	return 0; /* not reached */
}

int main(int argc, char **argv)
{
	const char *mode = "";

	if (argc >= 1 && argv != (char **)0) {
		mode = argv[0];
	}
	if (same(mode, "watch")) {
		return mode_watch();
	}
	if (same(mode, "loop")) {
		return mode_loop();
	}
	if (same(mode, "run") && argc >= 2) {
		const char *tail = (const char *)0;

		if (argc >= 3) {
			tail = argv[2];
		}
		return mode_run(argv[1], tail);
	}
	if (same(mode, "exit")) {
		return mode_exit();
	}
	console_puts("events: usage: events watch|loop|run PATH [TAIL]|exit\r\n");
	return 1;
}
