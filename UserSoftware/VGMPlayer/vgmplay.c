/* VGMPlayer for KonaOS.
 *
 * Streams the ZVGC v1/v2 format produced by ../VGMPlayer/tools/compile_vgm.py
 * and writes conventional SN76489 bytes to PSG0.  The player owns no decoder:
 * the file is wrapped in a borrowed kos_reader_t and handed to libsnd's
 * pre-rendered stream player (snd_zvg_*), which validates the header, adopts
 * the file's playback rate and renders the stream from a 256-byte ring that
 * the foreground keeps filled with snd_service().  Filesystem, timer-event
 * and sound-device access all go through the KonaOS 0.2.0 SDK.
 *
 * The TIMER driver posts at the period snd_timer_period() derives from the
 * ZVGC header's 1-180 Hz rate.  Events coalesce while a filesystem refill is
 * active; the handler feeds snd_tick the delivered count and libsnd's
 * catch-up bound absorbs the coalescing.
 */

#include "kos.h"
#include "kos/event.h"
#include "kos/reader.h"
#include "kos/snd.h"
#include "kos/timer.h"

static int timer_handle;
static FILE *file_stream;
static kos_reader_t reader;
static volatile unsigned char playing;
static volatile unsigned char aborted;
static volatile unsigned int base_event_count;
static volatile unsigned int missed_event_count;

static unsigned char same(const char *left, const char *right)
{
	while (*left == *right) {
		if (*left == '\0') {
			return 1u;
		}
		++left;
		++right;
	}
	return 0u;
}

static void usage(void)
{
	console_puts("VGMPLAY 0.2 for KonaOS\r\n"
		     "Usage: vgmplay FILE.ZVG\r\n"
		     "Press any key to stop playback.\r\n");
}

static void on_timer_event(unsigned int count, unsigned int missed)
{
	missed_event_count = (unsigned int)(missed_event_count + missed);
	if (count != 0u && playing != 0u) {
		(void)snd_tick((uint16_t)count);
		base_event_count = (unsigned int)(base_event_count + count);
	}
}

static int start_timer(void)
{
	uint32_t period;
	int result;

	if (timer_handle <= 0) {
		return -KOS_ENODEV;
	}
	result = snd_timer_period(&period);
	if (result < 0) {
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
		return result;
	}
	return 0;
}

static void stop_timer(void)
{
	if (timer_handle <= 0) {
		return;
	}
	(void)timer_event(timer_handle, 0u);
	(void)notify(timer_handle, (kos_event_handler_t)0);
	(void)unbind((unsigned char)timer_handle);
	timer_handle = 0;
}

int main(int argc, char **argv)
{
	const char *path;
	struct kfs_stat info;
	struct snd_zvg_status status;
	int result;
	int rc = 0;

	if (argc != 1 || argv == (char **)0 ||
	    same(argv[0], "-h") != 0u || same(argv[0], "--help") != 0u ||
	    same(argv[0], "/?") != 0u) {
		usage();
		return argc == 1 ? 0 : 1;
	}
	path = argv[0];
	console_puts("VGMPLAY 0.2 - streamed SN76489 player for KonaOS\r\n");
	/* Prime idle()'s one-time /dev/timer probe before subscribing to
	 * timer events, so no event source is running while it probes. */
	idle();

	result = stat(path, &info);
	if (result < 0 || info.type != KFS_TYPE_FILE) {
		printf("vgmplay: cannot stat %s (%d)\r\n", path, result);
		return 2;
	}
	if (info.size < 16u) {
		console_puts("vgmplay: file is too short for a ZVGC header\r\n");
		return 2;
	}
	file_stream = fopen(path, "rb");
	if (file_stream == (FILE *)0) {
		printf("vgmplay: cannot open stream %s\r\n", path);
		return 2;
	}
	result = kos_reader_from_stdio(&reader, file_stream);
	if (result < 0) {
		printf("vgmplay: cannot size the stream (%d)\r\n", result);
		rc = 3;
		goto cleanup;
	}
	result = snd_open();
	if (result < 0) {
		printf("vgmplay: cannot acquire /dev/sound (%d)\r\n", result);
		rc = 5;
		goto cleanup;
	}
	result = snd_zvg_open(&reader, 0u);
	if (result < 0) {
		printf("vgmplay: unsupported or damaged ZVGC file (%d)\r\n",
		       result);
		rc = 4;
		goto cleanup;
	}
	result = snd_zvg_prepare();
	if (result < 0) {
		printf("vgmplay: initial read failed (%d)\r\n", result);
		rc = 3;
		goto cleanup;
	}

	printf("File: %s  Size: %lu bytes  Rate: %u Hz\r\n",
	       path, info.size, (unsigned int)snd_tick_rate());
	timer_handle = timer_open();
	if (timer_handle < 0) {
		result = timer_handle;
		timer_handle = 0;
		printf("vgmplay: timer event unavailable (%d)\r\n", result);
		rc = 5;
		goto cleanup;
	}
	result = start_timer();
	if (result < 0) {
		printf("vgmplay: timer event unavailable (%d)\r\n", result);
		rc = 5;
		goto cleanup;
	}
	console_puts("Playing; press any key to stop.\r\n");
	playing = 1u;
	result = snd_zvg_play();
	if (result < 0) {
		playing = 0u;
		printf("vgmplay: cannot start playback (%d)\r\n", result);
		rc = 5;
		goto cleanup;
	}

	while (playing != 0u) {
		char key;

		(void)snd_service();
		(void)snd_zvg_get_status(&status);
		if (status.state != SND_ZVG_PLAYING &&
		    status.state != SND_ZVG_UNDERRUN) {
			playing = 0u;
			break;
		}
		if (console_poll() > 0) {
			(void)console_read(&key, 1u);
			aborted = 1u;
			playing = 0u;
		} else {
			idle();
		}
	}

cleanup:
	playing = 0u;
	/* Capture the counters before close resets them. */
	(void)snd_zvg_get_status(&status);
	stop_timer();
	snd_zvg_close();
	snd_close();
	if (file_stream != (FILE *)0) {
		int close_result = fclose(file_stream);

		if (close_result < 0 && rc == 0) {
			printf("vgmplay: close failed (%d)\r\n", close_result);
			rc = 3;
		}
		file_stream = (FILE *)0;
	}

	if (aborted != 0u) {
		console_puts("Playback aborted.\r\n");
	} else if (status.last_error < 0) {
		printf("Playback stopped by reader error %d.\r\n",
		       status.last_error);
		rc = 3;
	} else if (status.state == SND_ZVG_ERROR) {
		printf("Malformed ZVGC stream at offset %lu.\r\n",
		       status.error_offset);
		rc = 4;
	} else if (rc == 0) {
		console_puts("Playback complete.\r\n");
	}
	printf("Timer events %04x  ticks %04x  bytes %04x  writes %04x\r\n",
	       base_event_count, (unsigned int)status.ticks,
	       (unsigned int)status.bytes, (unsigned int)status.writes);
	printf("Underruns %u  missed events %u\r\n",
	       (unsigned int)status.underruns, missed_event_count);
	if (rc == 0 && aborted == 0u &&
	    (status.underruns != 0u || missed_event_count != 0u)) {
		console_puts("Playback completed with timing underruns.\r\n");
		rc = 6;
	}
	return rc;
}
