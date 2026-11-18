/* libsnd PCM outcome program (card program, run from the production shell
 * by tests/test_snd_pcm.py).
 *
 *   sndpcm FILE          play to the end
 *   sndpcm FILE loop     play looped; stop once three passes have played
 *   sndpcm FILE stop     stop once a quarter has played
 *   sndpcm FILE small    play with the smallest staging buffer
 *
 * The foreground is a game's: snd_pcm_service between idle() waits.  One
 * line per outcome; the replay checks the lines and the DAC's bytes.
 */

#include "kos.h"
#include "kos/snd.h"

static uint8_t stage[4096];   /* also the empty mode string: BSS is zero */

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

int main(int argc, char **argv)
{
	struct snd_pcm_info info;
	struct snd_pcm_status status;
	kos_handle_source_t source;
	kos_reader_t reader;
	char *mode = argc >= 2 ? argv[1] : (char *)stage;
	uint16_t size = same(mode, "small") ? SND_PCM_STAGE_MIN : sizeof(stage);
	uint8_t flags = same(mode, "loop") ? SND_PCM_LOOP : 0u;
	uint8_t stop_early = same(mode, "stop");
	int file;
	int result;

	if (argc < 1) {
		return 1;
	}
	file = open(argv[0], KFS_O_READ);
	if (file < 0) {
		printf("pcm: open %d\r\n", file);
		return 2;
	}
	source.handle = (unsigned int)file;
	(void)kos_reader_from_handle(&reader, &source);
	result = snd_open();
	if (result < 0) {
		printf("pcm: snd_open %d\r\n", result);
		return 3;
	}
	result = snd_pcm_open(&reader, stage, size);
	printf("pcm: pcm_open %d\r\n", result);
	if (result < 0) {
		snd_close();
		return 4;
	}
	(void)snd_pcm_get_info(&info);
	printf("pcm: info codec %u rate %u samples %lu payload %lu\r\n",
	       (unsigned int)info.codec, info.rate, info.samples, info.payload);
	result = snd_pcm_play(flags);
	printf("pcm: play %d\r\n", result);
	for (;;) {
		result = snd_pcm_service();
		if (result < 0) {
			printf("pcm: service %d\r\n", result);
			break;
		}
		if (result != SND_PCM_PLAYING) {
			break;
		}
		(void)snd_pcm_get_status(&status);
		if (flags != 0u && status.played >= info.samples + info.samples + info.samples) {
			break;
		}
		if (stop_early != 0u && status.played >= info.samples / 4u) {
			break;
		}
		idle();
	}
	(void)snd_pcm_stop();
	(void)snd_pcm_get_status(&status);
	printf("pcm: end state %u played %lu under %lu loops %u reads %u "
	       "error %d\r\n", (unsigned int)status.state, status.played,
	       status.underruns, status.loops, status.reads,
	       status.last_error);
	snd_pcm_close();
	snd_close();
	(void)close((unsigned int)file);
	return 0;
}
