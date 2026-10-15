/* KonaOS device-event board check: `/system/beat.exe`.
 *
 * The hardware proof for the event mechanism: subscribe to `/dev/timer`'s event
 * source at 500 ms and toggle a 440 Hz tone on PSG0's tone generator 0 at
 * every delivery, for ten seconds, printing `count/missed` once per
 * second.  A steady two-tone-per-second beat and a `missed` of 0 are the
 * check; the stopwatch is the ten-second run.
 *
 *   `beat`                 the checklist: 500 ms period, ten seconds;
 *   `beat run <n>`         the same period for `n` seconds - a shorter
 *                          start-up check for a board or a replay;
 *   `beat ls <dir> <n>`    the same period for `n` seconds while the
 *                          directory is enumerated in a loop: the
 *                          checklist's "while `ls /fs` repeats", which
 *                          the monitor cannot do for itself (its X run
 *                          is synchronous).
 *
 * The handler is entered from wherever the program was - including in the
 * middle of the console write the main loop makes - so it only touches its
 * own counters and one I/O port, and every line is `beat: ...`.
 *
 * The PSG ports and the SN76489 byte order are the board's: the card
 * selects the four chips at E0h-E3h and takes conventional SN76489 bit
 * order with no software data-bit reversal (`../Zephyr-80/Code/HOST/
 * CPM2.2/src/layout/platform.inc`, `SOUND_PSG0_PORT = 0xE0`;
 * `Code/HOST/SNTracker/src/sn76489.asm`, "the hardware data-bit reversal
 * is already accounted for on the card and must not be repeated in
 * software").
 */

#include "kos.h"
#include "diag.h" /* the SDK diag wrappers (not in kos.h) */
#include "kos/event.h"
#include "kos/ipc.h"
#include "kos/timer.h"

/* PSG0 at E0h, and the SN76489's two-byte writes: a latch byte
 * (`1RRRDDDD`: register and the low four data bits) followed by a data
 * byte (`0DDDDDD`: the upper six). */
#define PSG0_PORT   0xE0u
#define PSG_TONE0   0x80u /* register 0: tone generator 0's frequency */
#define PSG_VOL0    0x90u /* register 8: tone generator 0's volume     */
#define PSG_SILENT  0x0Fu /* attenuation 15 is off                    */

/* 440 Hz from the 3.579545 MHz clock: N = 3579545 / (32 * 440) = 254.
 * Attenuation 3 is an audible level well below the maximum. */
#define TONE_TICKS  254u
#define TONE_LEVEL  0x03u

/* 500 ms in the clock's 25.6 us counts (39,062.5 per second).  The
 * second is rounded up so the tenth line is printed after the event that
 * falls on ten seconds. */
#define PERIOD_COUNTS 19531u
#define RUN_SECONDS   10u
#define SECOND_COUNTS 39063u
#define RUN_TAIL      "run"
#define LS_TAIL       "ls"
#define REBOOT_TAIL   "reboot"
/* `beat reboot`: the warm reboot comes after this many deliveries, with
 * the subscription and channel 3 still armed. */
#define REBOOT_AFTER  5u

static int event_handle;
static unsigned int beat_events;
static unsigned int beat_missed;
static unsigned char beat_loud;


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

/* A decimal count, 0 for anything that is not one. */
static unsigned int parse_seconds(const char *text)
{
	unsigned int value = 0u;

	while (*text >= '0' && *text <= '9') {
		value = value * 10u + (unsigned int)(*text - '0');
		if (value > 600u) {
			return 0u;
		}
		++text;
	}
	return (*text == '\0') ? value : 0u;
}

/* One full directory pass; 0 when it completed, the negated error
 * otherwise.  This is the shell's `ls` load without the printing: the
 * card's directory is read once per entry. */
static int list_once(const char *dir)
{
	struct kos_ns_entry entry;
	unsigned int cookie = 0;
	int status;

	for (;;) {
		status = list(dir, &cookie, &entry, 1u);
		if (status == 0) {
			return 0;
		}
		if (status < 0) {
			return status;
		}
	}
}

static void psg_write(unsigned char value)
{
	struct kos_diag_io_out_args req;

	req.port = PSG0_PORT;
	req.value = value;
	(void)diag_io_out(&req);
}

/* One 440 Hz tone, silent until the first event. */
static void tone_program(void)
{
	psg_write((unsigned char)(PSG_TONE0 | (TONE_TICKS & 0x0Fu)));
	psg_write((unsigned char)((TONE_TICKS >> 4) & 0x3Fu));
	psg_write((unsigned char)(PSG_VOL0 | PSG_SILENT));
}

static void tone_silence(void)
{
	psg_write((unsigned char)(PSG_VOL0 | PSG_SILENT));
	beat_loud = 0u;
}

/* The delivery: every event toggles the tone, so ten seconds of a 500 ms
 * period are twenty audible steps and the tone ends where it started. */
/* 1 when this program runs with interrupts enabled.  IFF2 is read up to
 * three times: an interrupt accepted right after LD A,I can make one read
 * report 0.  A program left running masked receives no delivery, so a
 * stalled count with `di` on its line names that cause. */
static unsigned char beat_irq_on(void) __naked
{
	__asm
	ld	a,i
	jp	pe,beat_irq_on_yes
	ld	a,i
	jp	pe,beat_irq_on_yes
	ld	a,i
	jp	pe,beat_irq_on_yes
	xor	a
	ret
beat_irq_on_yes:
	ld	a,#1
	ret
	__endasm;
}

static void beat_on_event(unsigned int count, unsigned int missed)
{
	beat_events += count;
	beat_missed += missed;
	beat_loud = (unsigned char)(beat_loud == 0u);
	psg_write((unsigned char)(PSG_VOL0 |
				  (beat_loud != 0u ? TONE_LEVEL : PSG_SILENT)));
}

int main(int argc, char **argv)
{
	uint32_t start = 0u;
	uint32_t now = 0u;
	uint32_t due = 0u;
	unsigned int second;
	unsigned int run_seconds = RUN_SECONDS;
	const char *list_dir = (const char *)0;
	unsigned int passes = 0u;
	unsigned char reboot_mode = 0u;
	int result;

	if (argc == 2 && argv != (char **)0 && same(argv[0], RUN_TAIL)) {
		run_seconds = parse_seconds(argv[1]);
		if (run_seconds == 0u) {
			printf("beat: FAIL seconds\r\n");
			return 1;
		}
	} else if (argc == 1 && argv != (char **)0 &&
		   same(argv[0], REBOOT_TAIL)) {
		reboot_mode = 1u;
	} else if (argc == 3 && argv != (char **)0 &&
		   same(argv[0], LS_TAIL)) {
		list_dir = argv[1];
		run_seconds = parse_seconds(argv[2]);
		if (list_dir == (const char *)0 || run_seconds == 0u) {
			printf("beat: FAIL arguments\r\n");
			return 1;
		}
	}
	event_handle = timer_open();
	if (event_handle < 0) {
		printf("beat: FAIL open %d\r\n", event_handle);
		return 1;
	}
	tone_program();
	result = notify(event_handle, beat_on_event);
	if (result < 0) {
		tone_silence();
		printf("beat: FAIL notify %d\r\n", result);
		return 1;
	}
	if (timer_now(event_handle, &start) < 0) {
		(void)notify(event_handle, (kos_event_handler_t)0);
		tone_silence();
		printf("beat: FAIL clock\r\n");
		return 1;
	}
	result = timer_event(event_handle, PERIOD_COUNTS);
	if (result < 0) {
		(void)notify(event_handle, (kos_event_handler_t)0);
		tone_silence();
		printf("beat: FAIL event %d\r\n", result);
		return 1;
	}

	/* One one-second poll per second, each printing the running counts
	 * and the *clock's* own reading: a second with no delivery at all
	 * gets `gap`, which says the count column stalled and the clock
	 * column says whether time moved underneath it.  The second's
	 * length is accumulated by addition: the program links no 32-bit
	 * multiply runtime. */
	for (second = 1u; second <= run_seconds; ++second) {
		unsigned int counted = beat_events;

		due += SECOND_COUNTS;
		while (timer_now(event_handle, &now) == 0 &&
		       (uint32_t)(now - start) < due) {
			/* The deliveries happen inside this poll, and in
			 * `ls` mode the directory passes do too. */
			if (list_dir != (const char *)0 &&
			    list_once(list_dir) == 0) {
				++passes;
			}
			/* A warm reboot while subscribed: the channels are
			 * running and the subscription is live.  Only the tone
			 * is stopped first - KonaOS owns no sound driver, so
			 * nothing else would silence the chip across the reset. */
			if (reboot_mode != 0u && beat_events >= REBOOT_AFTER) {
				tone_silence();
				printf("beat: reboot after %u events\r\n",
				       beat_events);
				reboot();
			}
		}
		printf("beat: %u s %u/%u clock %lu%s%s\r\n", second,
		       beat_events, beat_missed, (unsigned long)(now - start),
		       (beat_events == counted) ? " gap" : "",
		       beat_irq_on() != 0u ? "" : " di");
	}
	if (list_dir != (const char *)0) {
		printf("beat: ls %u/%u %s\r\n", passes, run_seconds,
		       list_dir);
	}

	/* Stop the source first (the channel stops), then unsubscribe and
	 * silence: the tone must not outlive the program.  The last line of
	 * the loop is the run's counts. */
	(void)timer_event(event_handle, 0u);
	(void)notify(event_handle, (kos_event_handler_t)0);
	tone_silence();
	(void)unbind((unsigned char)event_handle);
	event_handle = 0;
	return 0;
}
