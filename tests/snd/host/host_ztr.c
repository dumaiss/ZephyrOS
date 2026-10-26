/* Host shim for the libsnd ZTR producer (tests/snd/host).
 *
 * Provides the globals and the assembly entry stubs snd_ztr.c reaches,
 * and a FILE-backed kos_reader_t, so the open/validate/prepare/service
 * half builds untouched with the host compiler (HOST_ZTR makes the
 * page computation use real pointers).  One run opens the song, drains
 * the prepared-row queue in the foreground and prints every decoded
 * event in the record order, the same text the oracle's decode_row
 * dump (ztr_trace --rows) writes, plus the per-order row-record
 * checksum sndtest's ztrload mode prints on the card, so the target
 * run can be pinned to this one.
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
	/* The deferred acceptance is specifically the open pass.  Row
	 * production follows 16 independent pattern cursors and is
	 * intentionally random-access. */
	source.decreased = 0;
	result = snd_ztr_prepare();
	if (result != 0 && result != SND_NEED_MORE) {
		printf("ztr: prepare %d\n", result);
		snd_ztr_close();
		(void)fclose(file);
		return 0;
	}
	/* Drain the queue in the foreground, service refilling it, and
	 * print every event, then the per-order checksum. */
	{
		uint16_t order = 0u;
		uint16_t sum = 0u;

		for (;;) {
			uint8_t head = snd_ztr_head;

			while (head != snd_ztr_tail) {
				uint8_t *record = snd_ztr_row_address(head);
				unsigned int byte;
				uint8_t channel;

				if (record[0] != (uint8_t)order) {
					printf("checksum %u %04x\n",
					       (unsigned int)order,
					       (unsigned int)sum);
					order = record[0];
					sum = 0u;
				}
				for (byte = 0u; byte < SND_ZTR_ROW_BYTES;
				     ++byte) {
					/* the host shim's uint16_t is
					 * 32 bits: force the 16-bit
					 * rotate AND the 16-bit add
					 * the target uses */
					sum = (uint16_t)(((sum << 1) |
							  (sum >> 15)) &
							 0xFFFFu);
					sum = (uint16_t)((sum +
							  record[byte]) &
							 0xFFFFu);
				}
				for (channel = 0u; channel < SND_ZTR_CHANNELS;
				     ++channel) {
					uint8_t *slot = record + 2u +
						channel * SND_ZTR_SLOT_BYTES;

					if (slot[0] == 0u) {
						continue;
					}
					printf("row %u %u ch %u mask %02x "
					       "note %02x instr %02x "
					       "vol %02x effect %u "
					       "param %02x mod %02x "
					       "arp %02x rate %d "
					       "ceil %u\n",
					       (unsigned int)record[0],
					       (unsigned int)record[1],
					       (unsigned int)channel,
					       (unsigned int)slot[0],
					       (unsigned int)slot[1],
					       (unsigned int)slot[2],
					       (unsigned int)slot[3],
					       (unsigned int)slot[4],
					       (unsigned int)slot[5],
					       (unsigned int)slot[6],
					       (unsigned int)slot[7],
					       (int)(signed char)slot[8],
					       (unsigned int)slot[9]);
				}
				++head;
				if (head == SND_ZTR_QUEUE_ROWS) {
					head = 0u;
				}
				snd_ztr_head = head;
			}
			{
				uint8_t before = snd_ztr_tail;

				(void)snd_service();
				if (snd_ztr_tail == before) {
					break;
				}
			}
		}
		printf("checksum %u %04x\n", (unsigned int)order,
		       (unsigned int)sum);
	}
	snd_ztr_close();
	(void)fclose(file);
	if (open_decreased) {
		fprintf(stderr, "ztr: reader offsets decreased\n");
		return 3;
	}
	return 0;
}
