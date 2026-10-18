/* Native KonaOS port of the Zephyr-80 assembly SNTracker player.
 * Timer callbacks publish work only. ZTR decoding, macros, UI and all PSG I/O
 * stay in foreground process context, matching the original's design. */

#include "sntracker.h"
#include "kos/event.h"
#include "kos/timer.h"

#define TIMER_BASE_RATE       180u
#define TIMER_BASE_PERIOD     217u

static int timer_handle;
static volatile unsigned char tick_produced;
static volatile unsigned char tick_consumed;
static volatile unsigned int tick_phase;
static volatile unsigned int timer_event_count;
static volatile unsigned int missed_event_count;
static volatile unsigned int queue_overrun_count;
static unsigned char aborted;
static unsigned int ui_refresh_wait;

static unsigned char same(const char *left, const char *right)
{
	while (*left == *right) {
		if (*left == '\0') return 1u;
		++left;
		++right;
	}
	return 0u;
}

static void usage(void)
{
	console_puts("SNTRACK 0.2 for KonaOS\r\n"
		     "Usage: sntrack FILE.ZTR\r\n"
		     "Q or Esc stops playback.\r\n");
}

static void on_timer_event(unsigned int count, unsigned int missed)
{
	missed_event_count = (unsigned int)(missed_event_count + missed);
	while (count != 0u) {
		--count;
		++timer_event_count;
		tick_phase = (unsigned int)(tick_phase + ztr_song.tick_rate);
		if (tick_phase < TIMER_BASE_RATE) continue;
		tick_phase = (unsigned int)(tick_phase - TIMER_BASE_RATE);
		if ((unsigned char)(tick_produced + 1u) == tick_consumed)
			++queue_overrun_count;
		else
			tick_produced = (unsigned char)(tick_produced + 1u);
	}
}

static int start_timer(void)
{
	int result;
	timer_handle = timer_open();
	if (timer_handle < 0) {
		result = timer_handle;
		timer_handle = 0;
		return result;
	}
	result = notify(timer_handle, on_timer_event);
	if (result < 0) {
		(void)unbind((unsigned char)timer_handle);
		timer_handle = 0;
		return result;
	}
	result = timer_event(timer_handle, TIMER_BASE_PERIOD);
	if (result < 0) {
		(void)notify(timer_handle, (kos_event_handler_t)0);
		(void)unbind((unsigned char)timer_handle);
		timer_handle = 0;
	}
	return result;
}

static void stop_timer(void)
{
	if (timer_handle <= 0) return;
	(void)timer_event(timer_handle, 0u);
	(void)notify(timer_handle, (kos_event_handler_t)0);
	(void)unbind((unsigned char)timer_handle);
	timer_handle = 0;
}

static void refresh_ui(unsigned char force)
{
	if (player_row_changed == 0u || (force == 0u && ui_refresh_wait != 0u))
		return;
	player_row_changed = 0u;
	printf("\rO%02x T%02u:%02u   ", (unsigned int)player_order,
	       (unsigned int)player_minutes, (unsigned int)player_seconds);
	ui_refresh_wait = (unsigned int)ztr_song.tick_rate << 2;
}

int main(int argc, char **argv)
{
	const char *path;
	int result;
	int status = 0;

	if (argc != 1 || argv == (char **)0 || same(argv[0], "-h") != 0u ||
	    same(argv[0], "--help") != 0u || same(argv[0], "/?") != 0u) {
		usage();
		return argc == 1 ? 0 : 1;
	}
	path = argv[0];
	console_puts("SNTRACK 0.2 - ZTR tracker player for KonaOS\r\n");
	/* idle() probes /dev/timer through a temporary handle on its first call.
	 * Do that before this process owns the timer's event subscription: release
	 * of any timer handle currently stops the driver's single event source. */
	idle();
	result = psg_open();
	if (result < 0) {
		printf("sntrack: cannot acquire /dev/sound (%d)\r\n", result);
		return 5;
	}
	result = ztr_load(path);
	if (result < 0) {
		printf("sntrack: cannot load %s: %s (%d)\r\n", path, ztr_error, result);
		psg_close();
		return 2;
	}
	printf("Playing: %s", ztr_song.title);
	if (ztr_song.author[0] != '\0') printf(" by %s", ztr_song.author);
	printf("\r\n%u Hz, speed %u, %u orders, %u patterns, %u bytes\r\n",
	       (unsigned int)ztr_song.tick_rate, (unsigned int)ztr_song.speed,
	       (unsigned int)ztr_song.order_count, ztr_song.pattern_count, ztr_size);
	console_puts("Q/Esc: stop; status updates are rate limited\r\n");

	tick_produced = 0u;
	tick_consumed = 0u;
	tick_phase = 0u;
	timer_event_count = 0u;
	missed_event_count = 0u;
	queue_overrun_count = 0u;
	aborted = 0u;
	ui_refresh_wait = 0u;
	player_start();
	refresh_ui(1u);
	if (player_playing == 0u) {
		status = 5;
		goto cleanup;
	}
	result = start_timer();
	if (result < 0) {
		printf("\r\nsntrack: timer event unavailable (%d)\r\n", result);
		status = 5;
		goto cleanup;
	}

	while (player_playing != 0u) {
		char key;

		if (tick_consumed != tick_produced) {
			tick_consumed = (unsigned char)(tick_consumed + 1u);
			player_tick();
			if (ui_refresh_wait != 0u) --ui_refresh_wait;
			refresh_ui(0u);
		}
		if (console_poll() > 0) {
			(void)console_read(&key, 1u);
			if (key == 'q' || key == 'Q' || key == 27) {
				aborted = 1u;
				player_stop();
			}
		} else if (tick_consumed == tick_produced && player_playing != 0u) {
			idle();
		}
	}

cleanup:
	player_stop();
	stop_timer();
	(void)psg_mute_all();
	psg_close();
	if (player_psg_error != 0u) {
		console_puts("\r\nPlayback stopped by PSG output error.\r\n");
		status = 5;
	} else if (aborted != 0u) {
		console_puts("\r\nPlayback aborted.\r\n");
	} else if (status == 0) {
		console_puts("\r\nPlayback complete.\r\n");
	}
	printf("Timer %04x  PSG writes %04x  queue overruns %04x  missed %04x\r\n",
	       timer_event_count, player_psg_writes,
	       queue_overrun_count, missed_event_count);
	return status;
}
