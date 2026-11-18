/* PCMPlay: a KonaOS PCM player for the Afternoon Blend DAC (README.md).
 *
 *   pcmplay [-l] [-g US] FILE.PCM   play; any key stops
 *   pcmplay -i FILE.PCM             print the header and exit
 *
 * A thin client of libsnd's PCM player (kos/snd.h, snd_pcm_*): the
 * library validates the file, runs the real-time interrupt at its rate
 * and decodes it into the RT ring; this program keeps the player fed
 * (snd_pcm_service between idle() waits), shows the progress once a
 * second, polls the keyboard as often and reports.  Every exit path closes
 * the player and the session, which returns the DAC to its midpoint and
 * releases the interrupt and the card.
 */

#include "kos.h"
#include "kos/snd.h"
#include "kos/timer.h"

#define PCM_STAGE 4096u                 /* staging: large reads */

static uint8_t stage[PCM_STAGE];
static struct snd_pcm_info info;
static struct snd_pcm_status status;
/* Handles, -1 when not held (set at the top of main: the SDK's image
 * carries no initialized data). */
static int file;
static int clock;
static uint8_t session;                 /* snd_open succeeded */

static uint8_t same(const char *a, const char *b)
{
	while (*a == *b) {
		if (*a == '\0') {
			return 1u;
		}
		++a;
		++b;
	}
	return 0u;
}

static void usage(void)
{
	console_puts("PCMPLAY 0.2 - KonaOS PCM on the Afternoon Blend DAC\r\n"
		     "Usage: pcmplay [-i] [-l] [-g US] FILE.PCM\r\n"
		     "  -i     print the header only\r\n"
		     "  -l     loop until a key is pressed\r\n"
		     "  -g US  the RT clock's lane gap, 1-255 us (default 20)\r\n"
		     "Press any key to stop playback.\r\n");
}

/* A decimal 1..255, or 0 when `text` is not one. */
static uint8_t parse_gap(const char *text)
{
	uint16_t value = 0u;

	if (*text == '\0') {
		return 0u;
	}
	while (*text >= '0' && *text <= '9') {
		value = (uint16_t)(value * 10u + (uint16_t)(*text - '0'));
		if (value > 255u) {
			return 0u;
		}
		++text;
	}
	return *text == '\0' ? (uint8_t)value : 0u;
}

/* mm:ss of `samples` at `rate`. */
static void print_time(uint32_t samples, uint16_t rate)
{
	uint32_t seconds = samples / rate;

	printf("%lu:%02u", seconds / 60u, (unsigned int)(seconds % 60u));
}

static uint32_t now(void)
{
	uint32_t t = 0u;

	if (clock >= 0) {
		(void)timer_now(clock, &t);
	}
	return t;
}

static void release(void)
{
	if (session != 0u) {
		snd_pcm_close();
		snd_close();
		session = 0u;
	}
	if (file >= 0) {
		(void)close((unsigned int)file);
		file = -1;
	}
}

int main(int argc, char **argv)
{
	kos_handle_source_t source;
	kos_reader_t reader;
	const char *path = (const char *)0;
	uint8_t info_only = 0u;
	uint8_t flags = 0u;
	uint8_t gap = SND_PCM_GAP_US;
	uint8_t aborted = 0u;
	uint32_t next_report = 0u;
	uint32_t busy = 0u;                /* samples played during service */
	uint32_t before;
	uint32_t t_start;
	uint32_t elapsed;
	int result;
	int rc = 0;
	int i;

	file = -1;
	clock = -1;
	session = 0u;
	for (i = 0; i < argc; ++i) {
		if (same(argv[i], "-i") != 0u) {
			info_only = 1u;
		} else if (same(argv[i], "-l") != 0u) {
			flags |= SND_PCM_LOOP;
		} else if (same(argv[i], "-g") != 0u && i + 1 < argc) {
			gap = parse_gap(argv[++i]);
			if (gap == 0u) {
				usage();
				return 1;
			}
		} else if (argv[i][0] != '-' && path == (const char *)0) {
			path = argv[i];
		} else {
			usage();
			return 1;
		}
	}
	if (path == (const char *)0) {
		usage();
		return argc == 0 ? 0 : 1;
	}

	file = open(path, KFS_O_READ);
	if (file < 0) {
		printf("pcmplay: %s: cannot open %d\r\n", path, file);
		return 2;
	}
	source.handle = (unsigned int)file;
	(void)kos_reader_from_handle(&reader, &source);
	result = snd_open();
	if (result < 0) {
		printf("pcmplay: no sound session (%d)\r\n", result);
		release();
		return 5;
	}
	session = 1u;
	result = snd_pcm_open(&reader, stage, PCM_STAGE);
	if (result == -KOS_EINVAL) {
		printf("pcmplay: %s: not a valid KonaOS PCM v1 file\r\n", path);
		release();
		return 2;
	}
	if (result < 0) {
		printf("pcmplay: %s: unsupported codec or no DAC (%d)\r\n",
		       path, result);
		release();
		return 2;
	}
	(void)snd_pcm_get_info(&info);
	printf("Title: %s\r\n", info.name);
	console_puts("Length: ");
	print_time(info.samples, info.rate);
	printf("  (%lu samples)\r\n", info.samples);
	printf("Codec: %s, %lu bytes\r\n",
	       info.codec == SND_PCM_CODEC_U8 ? "u8" : "dpcm4", info.payload);
	printf("Rate: %u Hz on the RT clock, gap %u us\r\n", info.rate,
	       (unsigned int)gap);
	if (info_only != 0u) {
		release();
		return 0;
	}

	clock = timer_open();
	(void)snd_pcm_set_gap(gap);
	result = snd_pcm_prepare();
	if (result < 0) {
		printf("pcmplay: read failed (%d)\r\n", result);
		release();
		return 3;
	}
	t_start = now();
	result = snd_pcm_play(flags);
	if (result < 0) {
		if (result == -KOS_EBUSY) {
			console_puts("pcmplay: another program holds the "
				     "real-time interrupt\r\n");
		} else if (result == -KOS_ENOTSUP) {
			console_puts("pcmplay: this IO controller has no RT "
				     "clock\r\n");
		} else {
			printf("pcmplay: no real-time interrupt (%d)\r\n",
			       result);
		}
		release();
		return 5;
	}
	console_puts("Playing; press any key to stop.\r\n");

	for (;;) {
		/* The samples the DAC plays while service runs are its cost,
		 * measured on the RT clock (the system timer loses ticks while
		 * a card read runs masked). */
		(void)snd_pcm_get_status(&status);
		before = status.played + status.underruns;
		result = snd_pcm_service();
		(void)snd_pcm_get_status(&status);
		busy += status.played + status.underruns - before;
		if (result != SND_PCM_PLAYING) {
			break;
		}
		/* Once a second of audio: the progress line and the key.  The
		 * IOC holds a key until it is asked for, so a slow poll loses
		 * nothing and keeps the console's IOC exchanges rare. */
		if (status.played >= next_report) {
			console_puts("\r");
			print_time(status.played, info.rate);
			console_puts(" / ");
			print_time(info.samples, info.rate);
			printf("  underruns %lu ", status.underruns);
			next_report += info.rate;
			if (console_poll() > 0) {      /* also flushes the line */
				char key;

				(void)console_read(&key, 1u);
				aborted = 1u;
				break;
			}
		}
		idle();
	}
	(void)snd_pcm_stop();
	elapsed = now() - t_start;
	(void)snd_pcm_get_status(&status);

	console_puts("\r\n");
	if (aborted != 0u) {
		console_puts("Playback stopped.\r\n");
	} else if (status.state == SND_PCM_ERROR) {
		printf("Playback stopped by a read error (%d).\r\n",
		       status.last_error);
		rc = 3;
	} else if (status.played < info.samples) {
		console_puts("Playback ended early: the file is short.\r\n");
		rc = 6;
	} else {
		console_puts("Playback complete.\r\n");
	}
	printf("Samples %lu  underruns %lu  rate %u Hz\r\n",
	       (status.played > info.samples && flags == 0u)
	       ? info.samples : status.played,
	       status.underruns, info.rate);
	/* The foreground's share: service time over playback time, both in
	 * RT ticks. */
	{
		uint32_t total = status.played + status.underruns;

		printf("Service %lu%% of the playback (%u reads)\r\n",
		       total >= 100u ? busy / (total / 100u) : 0u,
		       status.reads);
	}
	/* One clock count is 25.6 us: ms = counts * 32 / 1250. */
	printf("Elapsed %lu ms  loops %u\r\n",
	       (elapsed * 32u) / 1250u, status.loops);
	release();
	return rc;
}
