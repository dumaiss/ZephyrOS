/* SNTracker for KonaOS: a streamed libsnd ZTR client.
 *
 * The timer handler advances libsnd directly.  The foreground services the
 * prepared-row queue, polls the UI and reports the player status.  The file
 * and its stdio reader remain owned by this process for the whole playback.
 */

#include "kos.h"
#include "kos/event.h"
#include "kos/reader.h"
#include "kos/snd.h"
#include "kos/timer.h"

static int timer_handle;
static FILE *song_file;
static kos_reader_t song_reader;
static volatile unsigned char playing;
static volatile unsigned char aborted;
static volatile unsigned int timer_event_count;
static volatile unsigned int missed_event_count;
static unsigned char shown_order;
static unsigned char shown_row;

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
	if (count != 0u && playing != 0u) {
		(void)snd_tick((uint16_t)count);
		timer_event_count = (unsigned int)(timer_event_count + count);
	}
}

static int start_timer(void)
{
	uint32_t period;
	int result;

	result = snd_timer_period(&period);
	if (result < 0) return result;
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
	result = timer_event(timer_handle, (unsigned int)period);
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

static void refresh_ui(const struct snd_ztr_status *status,
		       unsigned char force)
{
	if (force == 0u && status->order == shown_order &&
	    status->row == shown_row) return;
	shown_order = status->order;
	shown_row = status->row;
	printf("\rO%02x R%03u U%u   ", (unsigned int)status->order,
	       (unsigned int)status->row, (unsigned int)status->underruns);
}

int main(int argc, char **argv)
{
	const char *path;
	struct snd_ztr_status status;
	int result;
	int rc = 0;

	if (argc != 1 || argv == (char **)0 || same(argv[0], "-h") != 0u ||
	    same(argv[0], "--help") != 0u || same(argv[0], "/?") != 0u) {
		usage();
		return argc == 1 ? 0 : 1;
	}
	path = argv[0];
	console_puts("SNTRACK 0.2 - streamed ZTR player for KonaOS\r\n");
	/* Prime idle() before this process subscribes to the timer source. */
	idle();

	song_file = fopen(path, "rb");
	if (song_file == (FILE *)0) {
		printf("sntrack: cannot open %s\r\n", path);
		return 2;
	}
	result = kos_reader_from_stdio(&song_reader, song_file);
	if (result < 0) {
		printf("sntrack: cannot size %s (%d)\r\n", path, result);
		rc = 3;
		goto cleanup;
	}
	result = snd_open();
	if (result < 0) {
		printf("sntrack: cannot acquire /dev/sound (%d)\r\n", result);
		rc = 5;
		goto cleanup;
	}
	result = snd_ztr_open(&song_reader);
	if (result < 0) {
		printf("sntrack: unsupported or damaged ZTR file (%d)\r\n",
		       result);
		rc = 4;
		goto cleanup;
	}
	result = snd_ztr_prepare();
	while (result == SND_NEED_MORE) {
		(void)snd_service();
		result = snd_ztr_prepare();
	}
	if (result < 0) {
		printf("sntrack: initial read failed (%d)\r\n", result);
		rc = 3;
		goto cleanup;
	}
	printf("Playing: %s  %lu bytes  %u Hz\r\n", path,
	       song_reader.size, (unsigned int)snd_tick_rate());
	console_puts("Q/Esc: stop; the foreground keeps the row queue filled\r\n");

	timer_event_count = 0u;
	missed_event_count = 0u;
	aborted = 0u;
	shown_order = 0xFFu;
	shown_row = 0xFFu;
	result = snd_ztr_play();
	if (result < 0) {
		printf("sntrack: cannot start playback (%d)\r\n", result);
		rc = 5;
		goto cleanup;
	}
	playing = 1u;
	result = start_timer();
	if (result < 0) {
		printf("sntrack: timer event unavailable (%d)\r\n", result);
		rc = 5;
		goto cleanup;
	}

	while (playing != 0u) {
		char key;

		(void)snd_service();
		(void)snd_ztr_get_status(&status);
		refresh_ui(&status, 0u);
		if (status.state != SND_ZTR_PLAYING &&
		    status.state != SND_ZTR_UNDERRUN) {
			playing = 0u;
			break;
		}
		if (console_poll() > 0) {
			(void)console_read(&key, 1u);
			if (key == 'q' || key == 'Q' || key == 27) {
				aborted = 1u;
				playing = 0u;
			}
		} else {
			idle();
		}
	}

cleanup:
	playing = 0u;
	(void)snd_ztr_get_status(&status);
	stop_timer();
	(void)snd_ztr_stop();
	snd_ztr_close();
	snd_close();
	if (song_file != (FILE *)0) {
		if (fclose(song_file) < 0 && rc == 0) rc = 3;
		song_file = (FILE *)0;
	}
	if (aborted != 0u) {
		console_puts("\r\nPlayback aborted.\r\n");
	} else if (status.last_error < 0) {
		printf("\r\nPlayback stopped by reader error %d.\r\n",
		       status.last_error);
		rc = 3;
	} else if (status.state == SND_ZTR_ERROR) {
		console_puts("\r\nMalformed ZTR stream.\r\n");
		rc = 4;
	} else if (rc == 0) {
		console_puts("\r\nPlayback complete.\r\n");
	}
	printf("Timer events %04x  underruns %u  overruns %u  missed %04x\r\n",
	       timer_event_count, (unsigned int)status.underruns,
	       (unsigned int)status.overruns, missed_event_count);
	return rc;
}
