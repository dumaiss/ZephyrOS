/* The ZTR behavioral oracle (tests/snd/oracle).
 *
 * Builds SNTracker's player unchanged - ztr.c (load and validate),
 * player.c (rows, macros, effects) and psg.c (the channel-to-register
 * render with its register cache) - against a host sound_write that
 * records.  One run prints the per-tick trace:
 *
 *     tick N: c<chip>:<hh> ...
 *
 * in the order psg.c queued the bytes (the tone latch+data pair, then
 * the attenuation; noise control first).  Every later libsnd package
 * reproduces these traces, writes included: the C player's register
 * cache suppresses unchanged bytes, and libsnd must match it.
 *
 * With a third argument, the 16 channels' state is written to that file
 * after every tick, one line per channel:
 *
 *     tick N: ch<i> n=<hh> v=<h> p=<hhh> f=<hh> m=<hh> <hh> <hh> <hh> <hh>
 *
 * (note, macro volume, pitch, flags, and the five macro positions).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sntracker.h"
#include "kos/sound.h"

#define MAX_TICK_WRITES 48u
#define MAX_TICKS       60000u

static struct ksound_write recorded[MAX_TICK_WRITES];
static unsigned int recorded_count;
static FILE *state_file;
static unsigned int tick_index;
static unsigned int total_writes;
static FILE *rows_file;

void ztr_dump_event(unsigned char channel, unsigned char order,
		    unsigned char row, unsigned char mask,
		    unsigned char note, unsigned char instrument,
		    unsigned char volume, unsigned char effect,
		    unsigned char parameter, unsigned char modifiers,
		    unsigned char arpeggio, signed char hairpin_rate,
		    unsigned char hairpin_ceiling)
{
	if (rows_file == NULL) {
		return;
	}
	fprintf(rows_file,
		"row %u %u ch %u mask %02x note %02x instr %02x "
		"vol %02x effect %u param %02x mod %02x arp %02x "
		"rate %d ceil %u\n",
		(unsigned int)order, (unsigned int)row,
		(unsigned int)channel, (unsigned int)mask,
		(unsigned int)note, (unsigned int)instrument,
		(unsigned int)volume, (unsigned int)effect,
		(unsigned int)parameter, (unsigned int)modifiers,
		(unsigned int)arpeggio, (int)hairpin_rate,
		(unsigned int)hairpin_ceiling);
}

int stat(const char *path, struct kfs_stat *info)
{
	FILE *stream = fopen(path, "rb");
	long size;

	if (stream == NULL) {
		return -1;
	}
	if (fseek(stream, 0L, SEEK_END) != 0) {
		(void)fclose(stream);
		return -1;
	}
	size = ftell(stream);
	(void)fclose(stream);
	if (size < 0L) {
		return -1;
	}
	info->size = (uint32_t)size;
	info->type = KFS_TYPE_FILE;
	return 0;
}

int sound_open(unsigned char exclusive)
{
	(void)exclusive;
	return 1;
}

int sound_close(int handle)
{
	(void)handle;
	return 0;
}

int sound_reset(int handle)
{
	(void)handle;
	return 0;
}

/* The batch one psg_render_all produced for this tick. */
int sound_write(int handle, const struct ksound_write *writes,
		unsigned int count)
{
	(void)handle;
	if (writes == NULL && count != 0u) {
		return -KOS_EINVAL;
	}
	if (count > MAX_TICK_WRITES) {
		fprintf(stderr, "ztr_trace: %u writes exceed the record "
			"capacity\n", count);
		return -KOS_EINVAL;
	}
	memcpy(recorded, writes, count * sizeof(*writes));
	recorded_count = count;
	total_writes += count;
	return (int)count;
}

static void emit_tick(void)
{
	unsigned int i;

	printf("tick %u:", tick_index);
	for (i = 0u; i < recorded_count; ++i) {
		printf(" c%u:%02x", (unsigned int)recorded[i].target,
		       (unsigned int)recorded[i].value);
	}
	putchar('\n');
}

static void emit_state(void)
{
	unsigned int i;

	for (i = 0u; i < ZTR_CHANNELS; ++i) {
		const struct tracker_channel *channel = &player_channels[i];
		fprintf(state_file, "tick %u: ch%u n=%02x v=%u p=%hx f=%02x"
			" m=%04x %04x %04x %04x %04x\n",
			tick_index, i, (unsigned int)channel->note,
			(unsigned int)channel->macro_volume,
			(unsigned int)channel->pitch_value,
			(unsigned int)channel->flags,
			channel->macro_position[0], channel->macro_position[1],
			channel->macro_position[2], channel->macro_position[3],
			channel->macro_position[4]);
	}
}

static const char *base_name(const char *path)
{
	const char *slash = path;
	const char *p;

	for (p = path; *p != '\0'; ++p) {
		if (*p == '/') {
			slash = p + 1;
		}
	}
	return slash;
}

int main(int argc, char **argv)
{
	unsigned long ticks = 3000ul;
	const char *path;
	int result;

	if (argc < 2 || argc > 4) {
		fprintf(stderr, "usage: ztr_trace SONG.ZTR [TICKS [STATE|--rows FILE]]\n");
		return 2;
	}
	path = argv[1];
	if (argc >= 3) {
		if (strncmp(argv[2], "--rows", 6) == 0) {
			ticks = MAX_TICKS;
		} else {
			ticks = strtoul(argv[2], NULL, 10);
			if (ticks == 0ul || ticks > MAX_TICKS) {
				fprintf(stderr, "ztr_trace: ticks out of range\n");
				return 2;
			}
		}
	}
	state_file = NULL;
	rows_file = NULL;
	if (argc >= 4) {
		FILE *out = fopen(argv[3], "w");

		if (out == NULL) {
			perror(argv[3]);
			return 2;
		}
		if (argc >= 3 && strncmp(argv[2], "--rows", 6) == 0) {
			rows_file = out;
		} else {
			state_file = out;
		}
	}
	result = ztr_load(path);
	if (result < 0) {
		printf("ztr: %s (%d)\n", ztr_error, result);
		if (state_file != NULL) {
			(void)fclose(state_file);
		}
		return 1;
	}
	printf("# ZTR oracle trace\n");
	printf("# source: %s (%u bytes)\n", base_name(path), ztr_size);
	if (psg_open() < 0) {
		printf("# player refused to start\n");
		if (state_file != NULL) {
			(void)fclose(state_file);
		}
		return 1;
	}
	player_start();
	if (player_playing == 0u) {
		printf("# player refused to start\n");
		if (state_file != NULL) {
			(void)fclose(state_file);
		}
		return 1;
	}
	for (tick_index = 0u; tick_index < ticks && player_playing != 0u;
	     ++tick_index) {
		recorded_count = 0u;
		player_tick();
		emit_tick();
		if (state_file != NULL) {
			emit_state();
		}
	}
	printf("# %u ticks, %u writes, ended=%s\n", tick_index, total_writes,
	       player_playing != 0u ? "limit" : "song-end");
	if (state_file != NULL && fclose(state_file) != 0) {
		return 1;
	}
	if (rows_file != NULL && fclose(rows_file) != 0) {
		return 1;
	}
	return 0;
}
