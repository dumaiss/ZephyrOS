/* Host shim for the libsnd ZTR producer (tests/snd/host).
 *
 * Provides the globals and the assembly entry stubs snd_ztr.c reaches,
 * and a FILE-backed kos_reader_t, so the open/validate/prepare/service
 * half builds untouched with the host compiler (HOST_ZTR makes the
 * page computation use real pointers).  One run opens the song, drains
 * the order double buffers in the foreground and, per order and per
 * channel, compares the buffered streams with the bytes the pattern
 * directory's scan selects (read straight from the file), then prints
 * one stream checksum per order - the same checksum sndtest's ztrload
 * mode prints on the card, so the target run can be pinned to this
 * one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kos.h"
#include "kos/snd.h"

struct file_reader {
	FILE *file;
	uint32_t last_offset;
	unsigned int calls;
	unsigned int short_reads;
	unsigned int fail_call;
	int decreased;
};

static int file_read_at(void *context, uint32_t offset, void *dst,
			uint16_t requested)
{
	struct file_reader *reader = (struct file_reader *)context;
	uint16_t amount = requested;

	++reader->calls;
	if (offset < reader->last_offset) {
		reader->decreased = 1;
#ifdef HOST_ZTR_TRACE
		fprintf(stderr, "ztr: offset %lu after %lu at call %u\n",
			(unsigned long)offset,
			(unsigned long)reader->last_offset, reader->calls);
#endif
	}
	reader->last_offset = offset;
	if (reader->fail_call != 0u && reader->calls == reader->fail_call) {
		return -KOS_EIO;
	}
	if (reader->short_reads != 0u && amount > 3u) {
		amount = 3u;
	}

	if (fseek(reader->file, (long)offset, SEEK_SET) != 0) {
		return -KOS_EIO;
	}
	return (int)fread(dst, 1u, amount, reader->file);
}

int kos_reader_read_full(const kos_reader_t *reader, uint32_t offset,
			 void *dst, uint16_t length)
{
	uint16_t done = 0u;

	while (done < length) {
		int got = reader->read_at(reader->context,
					 offset + (uint32_t)done,
					 (char *)dst + done,
					 (uint16_t)(length - done));

		if (got < 0) {
			return got;
		}
		if (got == 0) {
			return -KOS_EIO;
		}
		done = (uint16_t)(done + (uint16_t)got);
	}
	return length;
}

int snd_handle = 1;
uint16_t snd_reserved_voices;
uint16_t snd_active_rate;
uint16_t snd_manual_rate;
unsigned int snd_overruns;
uint16_t snd_desired_period[SND_CHIP_COUNT][3];
uint8_t snd_desired_noise[SND_CHIP_COUNT];
uint8_t snd_output_mode;
uint8_t snd_voice_active[SND_VOICE_COUNT];
uint8_t snd_zvg_state;

void snd_shadow_invalidate(void) {}
void snd_output_flush(void) {}
void snd_ztr_mute(void) {}
void snd_ztr_restore(void) {}
void snd_ztr_reset_channels(void) {}
void snd_zvg_tick(void) {}

int snd_zvg_refill(void)
{
	return 0;
}

static unsigned int first_diff(const uint8_t *a, const uint8_t *b,
			       uint16_t length)
{
	uint16_t i;

	for (i = 0u; i < length; ++i) {
		if (a[i] != b[i]) {
			return i;
		}
	}
	return 0u;
}

#include "snd_ztr.c"

int main(int argc, char **argv)
{
	FILE *file;
	struct file_reader source;
	kos_reader_t reader;
	long size;
	int open_decreased;
	int result;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: host_ztr SONG.ZTR [short|error]\n");
		return 2;
	}
	file = fopen(argv[1], "rb");
	if (file == NULL) {
		perror(argv[1]);
		return 2;
	}
	if (fseek(file, 0L, SEEK_END) != 0 ||
	    (size = ftell(file)) < 0L || fseek(file, 0L, SEEK_SET) != 0) {
		(void)fclose(file);
		return 2;
	}
	source.file = file;
	source.last_offset = 0u;
	source.calls = 0u;
	source.short_reads = argc == 3 && strcmp(argv[2], "short") == 0;
	source.fail_call = argc == 3 && strcmp(argv[2], "error") == 0 ? 2u : 0u;
	source.decreased = 0;
	reader.context = &source;
	reader.read_at = file_read_at;
	reader.size = (uint32_t)size;
	result = snd_ztr_open(&reader);
	if (result != 0) {
		printf("ztr: refused %d\n", result);
		(void)fclose(file);
		return source.decreased ? 3 : 0;
	}
	open_decreased = source.decreased;
	/* The deferred acceptance is specifically the open pass.  Order
	 * refills follow the pattern directory's offsets and are
	 * intentionally random-access. */
	source.decreased = 0;
	result = snd_ztr_prepare();
	if (result != 0 && result != SND_NEED_MORE) {
		printf("ztr: prepare %d\n", result);
		snd_ztr_close();
		(void)fclose(file);
		return 0;
	}
	/* Drain the orders in the foreground: wait for each order's
	 * buffer, verify its streams against the bytes the directory
	 * scan selects (read straight from the file), then checksum
	 * the streams, as the card's ztrload does. */
	{
		uint16_t order;
		int failed = 0;

		for (order = 0u; order < snd_ztr_order_count &&
			     failed == 0; ++order) {
			uint8_t *buffer = snd_ztr_order_area +
				(uint16_t)(order & 1u) * SND_ZTR_ORDER_BYTES;
			uint16_t sum = 0u;
			uint16_t end = SND_ZTR_ORDER_TABLE;
			uint16_t byte;
			uint16_t expect_pos = SND_ZTR_ORDER_TABLE;
			uint8_t channel;
			unsigned int waits = 0u;

			while (snd_ztr_published != (uint8_t)order) {
				(void)snd_service();
				if (++waits > 10000u) {
					printf("ztr: stalled at order %u\n",
					       (unsigned int)order);
					failed = 1;
					break;
				}
			}
			if (failed != 0) {
				break;
			}
			setup_order_for(order);
			for (channel = 0u; channel < SND_ZTR_CHANNELS &&
				     failed == 0; ++channel) {
				uint8_t *entry = buffer + channel * 4u;
				uint16_t start = (uint16_t)entry[0] |
					((uint16_t)entry[1] << 8);
				uint16_t stream_end = (uint16_t)entry[2] |
					((uint16_t)entry[3] << 8);
				uint16_t length;
				uint8_t scratch[256u];
				uint16_t part;

				if (cursor_active[channel] == 0u) {
					if (start != 0u || stream_end != 0u) {
						printf("ztr: order %u "
						       "channel %u ffh "
						       "offsets %u %u\n",
						       (unsigned int)order,
						       (unsigned int)channel,
						       (unsigned int)start,
						       (unsigned int)stream_end);
						failed = 1;
					}
					continue;
				}
				length = (uint16_t)(cursor_end[channel] -
						    cursor_next[channel]);
				if (length == 0u) {
					if (start != 0u || stream_end != 0u) {
						printf("ztr: order %u "
						       "channel %u empty "
						       "offsets %u %u\n",
						       (unsigned int)order,
						       (unsigned int)channel,
						       (unsigned int)start,
						       (unsigned int)stream_end);
						failed = 1;
					}
					continue;
				}
				if (start != expect_pos ||
				    stream_end != expect_pos + length) {
					printf("ztr: order %u channel %u "
					       "offsets %u %u want %u %u\n",
					       (unsigned int)order,
					       (unsigned int)channel,
					       (unsigned int)start,
					       (unsigned int)stream_end,
					       (unsigned int)expect_pos,
					       (unsigned int)(expect_pos +
							      length));
					failed = 1;
					continue;
				}
				for (part = 0u; part < length &&
					     failed == 0; part += 256u) {
					uint16_t amount = length - part;

					if (amount > 256u) {
						amount = 256u;
					}
					if (fseek(file,
						  (long)(cursor_next[channel] +
							 part),
						  SEEK_SET) != 0 ||
					    fread(scratch, 1u, amount,
						  file) != amount) {
						printf("ztr: order %u "
						       "channel %u read "
						       "failed\n",
						       (unsigned int)order,
						       (unsigned int)channel);
						failed = 1;
						break;
					}
					if (memcmp(scratch, buffer + start +
						   part, amount) != 0) {
						printf("ztr: order %u "
						       "channel %u bytes "
						       "differ at %u\n",
						       (unsigned int)order,
						       (unsigned int)channel,
						       (unsigned int)(part +
							      (unsigned int)first_diff(
								      scratch,
								      buffer +
								      start +
								      part,
								      amount)));
						failed = 1;
					}
				}
				expect_pos = (uint16_t)(expect_pos + length);
				if (stream_end > end) {
					end = stream_end;
				}
			}
			if (failed != 0) {
				break;
			}
			for (byte = SND_ZTR_ORDER_TABLE; byte < end; ++byte) {
				/* the host shim's uint16_t is 32 bits:
				 * force the 16-bit rotate AND the
				 * 16-bit add the target uses */
				sum = (uint16_t)(((sum << 1) |
						  (sum >> 15)) & 0xFFFFu);
				sum = (uint16_t)((sum + buffer[byte]) &
						 0xFFFFu);
			}
			printf("checksum %u %04x\n", (unsigned int)order,
			       (unsigned int)sum);
			snd_ztr_consumed = (uint8_t)order;
			(void)snd_service();
		}
		if (failed != 0) {
			snd_ztr_close();
			(void)fclose(file);
			return 2;
		}
	}
	snd_ztr_close();
	(void)fclose(file);
	if (open_decreased) {
		fprintf(stderr, "ztr: reader offsets decreased\n");
		return 3;
	}
	return 0;
}
