/* The ZVG behavioral oracle (tests/snd/oracle).
 *
 * A self-contained host reference of the ZVGC v1/v2 decoder: the same
 * header checks, the same 256-byte command ring and the same per-tick
 * decoder_tick the target's libsnd implementation and the checked-in
 * goldens pin.  The original oracle exposed VGMPlayer's decoder by
 * including its translation unit; since the player now streams through
 * libsnd, the reference lives here on its own.
 *
 * One run prints the per-tick trace:
 *
 *     tick N: c0:<hh> ...
 *
 * the byte sequence the decoder wrote to PSG0 that tick, exactly as the
 * libsnd decoder must.  Errors print one outcome line:
 *
 *     zvg: open <code>                      the header was refused
 *     zvg: opcode <hh> at offset <n>        a malformed command stopped it
 *
 * and the run ends with a summary.  The whole file is fed at once:
 * the oracle's contract is the decoder's per-tick bytes, not the
 * streaming refill path (the stall contract is exercised against the
 * target's reader in tests/test_snd.py).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kos.h"

#define MAX_FILE_BYTES (256u * 1024u)
#define MAX_TICK_WRITES 256u
#define MAX_TICKS       30000ul

#define BUFFER_BYTES          6144u
#define ZVG_HEADER_BYTES      16u
#define ZVG_VERSION_V1        1u
#define ZVG_VERSION_V2        2u
#define ZVG_END               0x00u
#define ZVG_WRITE             0x01u
#define ZVG_WAIT16            0x02u
#define ZVG_WAIT_SHORT_FIRST  0x40u
#define ZVG_WAIT_SHORT_LIMIT  0x80u
#define ZVG_WRITE_RUN_FIRST   0x80u
#define ZVG_WRITE_RUN_LIMIT   0x90u
#define RING_BYTES            256u
#define TIMER_BASE_RATE       180u

static unsigned char file_bytes[MAX_FILE_BYTES];
static unsigned long file_length;

static unsigned char command_ring[RING_BYTES];
static volatile unsigned char ring_head;
static volatile unsigned char ring_tail;

static volatile unsigned char playing;
static volatile unsigned char producer_done;
static volatile unsigned char format_error;
static volatile unsigned char bad_opcode;
static volatile uint32_t bad_offset;
static volatile unsigned int wait_ticks;
static volatile unsigned int underrun_count;
static volatile unsigned int psg_write_count;
static volatile uint32_t stream_consumed;

static uint32_t stream_remaining;
static unsigned long stream_position;

static unsigned char recorded[MAX_TICK_WRITES];
static unsigned int recorded_count;
static unsigned long total_writes;

static uint32_t get_le32(const unsigned char *data)
{
	return (uint32_t)data[0] |
	       ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) |
	       ((uint32_t)data[3] << 24);
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

/* The recording PSG: the decoder's sound_out. */
static void psg_write(unsigned char value)
{
	if (recorded_count < MAX_TICK_WRITES) {
		recorded[recorded_count++] = value;
	}
	++total_writes;
	++psg_write_count;
}

/* The header checks, exactly the reference player's. */
static int validate_header(uint32_t file_size)
{
	uint32_t declared_length;

	if (file_length < ZVG_HEADER_BYTES || file_size < ZVG_HEADER_BYTES) {
		return -KOS_EIO;
	}
	if (file_bytes[0] != 'Z' || file_bytes[1] != 'V' ||
	    file_bytes[2] != 'G' || file_bytes[3] != 'C') {
		return -KOS_EINVAL;
	}
	if (file_bytes[4] != ZVG_VERSION_V1 &&
	    file_bytes[4] != ZVG_VERSION_V2) {
		return -KOS_ENOTSUP;
	}
	if (file_bytes[5] == 0u || file_bytes[5] > TIMER_BASE_RATE ||
	    file_bytes[7] != ZVG_HEADER_BYTES) {
		return -KOS_EINVAL;
	}
	declared_length = get_le32(&file_bytes[8]);
	if (declared_length == 0u ||
	    declared_length > file_size - ZVG_HEADER_BYTES) {
		return -KOS_EIO;
	}
	stream_remaining = declared_length;
	return 0;
}

/* One byte from the in-memory stream.  Zero means the declared stream
 * has been completely produced. */
static unsigned char stream_get_byte(unsigned char *value)
{
	if (stream_remaining == 0u) {
		return 0u;
	}
	if (stream_position < file_length) {
		*value = file_bytes[stream_position++];
		--stream_remaining;
		return 1u;
	}
	return 0u; /* the header promised more than the file delivered */
}

/* Copy as many raw stream bytes as fit.  tail is published once after
 * the batch, so a reader between the copies sees the old state only. */
static void producer_fill_ring(void)
{
	unsigned char tail = ring_tail;
	unsigned char value;

	while ((unsigned char)(tail + 1u) != ring_head) {
		if (stream_get_byte(&value) == 0u) {
			break;
		}
		command_ring[tail] = value;
		tail = (unsigned char)(tail + 1u);
	}
	ring_tail = tail;
	if (stream_remaining == 0u) {
		producer_done = 1u;
	}
}

static unsigned char ring_available(unsigned char head)
{
	return (unsigned char)(ring_tail - head);
}

static unsigned char ring_at(unsigned char head, unsigned char offset)
{
	return command_ring[(unsigned char)(head + offset)];
}

static void decoder_bad(unsigned char opcode, uint32_t offset)
{
	bad_opcode = opcode;
	bad_offset = offset;
	format_error = 1u;
	playing = 0u;
}

static void decoder_underrun_or_truncated(unsigned char opcode,
					 uint32_t offset)
{
	if (producer_done != 0u) {
		decoder_bad(opcode, offset);
	} else if (underrun_count != 0xFFFFu) {
		++underrun_count;
	}
}

/* One stream tick.  Consecutive writes are emitted in the same tick; a
 * wait or end command returns.  A wait reaching zero continues into
 * the command loop in the same tick. */
static void decoder_tick(void)
{
	if (wait_ticks != 0u) {
		--wait_ticks;
		if (wait_ticks != 0u) {
			return;
		}
	}
	for (;;) {
		unsigned char head = ring_head;
		unsigned char available = ring_available(head);
		unsigned char opcode;
		unsigned char needed;
		unsigned char writes = 0u;
		uint32_t offset = stream_consumed + ZVG_HEADER_BYTES;

		if (available == 0u) {
			decoder_underrun_or_truncated(0xFFu, offset);
			return;
		}
		opcode = ring_at(head, 0u);
		if (opcode == ZVG_END) {
			needed = 1u;
		} else if (opcode == ZVG_WRITE) {
			needed = 2u;
			writes = 1u;
		} else if (opcode == ZVG_WAIT16) {
			needed = 3u;
		} else if (opcode >= ZVG_WAIT_SHORT_FIRST &&
			   opcode < ZVG_WAIT_SHORT_LIMIT) {
			needed = 1u;
		} else if (opcode >= ZVG_WRITE_RUN_FIRST &&
			   opcode < ZVG_WRITE_RUN_LIMIT) {
			writes = (unsigned char)((opcode & 0x0Fu) + 1u);
			needed = (unsigned char)(writes + 1u);
		} else {
			decoder_bad(opcode, offset);
			return;
		}
		if (available < needed) {
			decoder_underrun_or_truncated(opcode, offset);
			return;
		}
		if (opcode == ZVG_END) {
			ring_head = (unsigned char)(head + 1u);
			++stream_consumed;
			playing = 0u;
			return;
		}
		if (writes != 0u) {
			unsigned char index;

			for (index = 0u; index < writes; ++index) {
				psg_write(ring_at(head,
						(unsigned char)(index + 1u)));
			}
			ring_head = (unsigned char)(head + needed);
			stream_consumed += needed;
			continue;
		}
		if (opcode == ZVG_WAIT16) {
			unsigned int ticks =
				(unsigned int)ring_at(head, 1u) |
				((unsigned int)ring_at(head, 2u) << 8);

			if (ticks == 0u) {
				decoder_bad(opcode, offset);
				return;
			}
			wait_ticks = ticks;
		} else {
			wait_ticks = (unsigned int)((opcode & 0x3Fu) + 1u);
		}
		ring_head = (unsigned char)(head + needed);
		stream_consumed += needed;
		return;
	}
}

int main(int argc, char **argv)
{
	FILE *stream;
	long size;
	const char *path;
	unsigned long tick;
	int result;

	if (argc != 2) {
		fprintf(stderr, "usage: zvg_trace SONG.ZVG\n");
		return 2;
	}
	path = argv[1];
	stream = fopen(path, "rb");
	if (stream == NULL) {
		perror(path);
		return 2;
	}
	size = 0L;
	{
		size_t got;
		while ((got = fread(file_bytes + size, 1u,
				    MAX_FILE_BYTES - (size_t)size,
				    stream)) != 0u) {
			size += (long)got;
			if ((size_t)size == MAX_FILE_BYTES) {
				break;
			}
		}
	}
	if (ferror(stream) != 0 || fclose(stream) != 0) {
		fprintf(stderr, "zvg_trace: read failed\n");
		return 2;
	}
	if (size < 0L || (size_t)size >= MAX_FILE_BYTES) {
		fprintf(stderr, "zvg_trace: file too large (%ld bytes, "
			"%u capacity)\n", size, MAX_FILE_BYTES - 1u);
		return 2;
	}
	file_length = (unsigned long)size;
	ring_head = 0u;
	ring_tail = 0u;
	playing = 0u;
	producer_done = 0u;
	format_error = 0u;
	bad_opcode = 0u;
	bad_offset = 0u;
	wait_ticks = 0u;
	underrun_count = 0u;
	psg_write_count = 0u;
	stream_consumed = 0u;
	stream_remaining = 0u;
	stream_position = ZVG_HEADER_BYTES;

	result = validate_header((uint32_t)size);
	if (result < 0) {
		printf("zvg: open %d\n", result);
		return 1;
	}
	printf("# ZVG oracle trace\n");
	printf("# source: %s (%ld bytes)\n", base_name(path), size);
	playing = 1u;
	for (tick = 0ul; playing != 0u && tick < MAX_TICKS; ++tick) {
		unsigned int i;

		recorded_count = 0u;
		producer_fill_ring();
		decoder_tick();
		printf("tick %lu:", tick);
		for (i = 0u; i < recorded_count; ++i) {
			printf(" c0:%02x", (unsigned int)recorded[i]);
		}
		putchar('\n');
		if (format_error != 0u) {
			printf("zvg: opcode %02x at offset %lu\n",
			       (unsigned int)bad_opcode,
			       (unsigned long)bad_offset);
			break;
		}
	}
	printf("# %lu ticks, %lu writes, underruns %u\n",
	       tick, total_writes, underrun_count);
	return format_error != 0u ? 1 : 0;
}
