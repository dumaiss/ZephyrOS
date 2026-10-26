/* KonaOS libsnd: the ZVG pre-rendered stream (user/sdk/snd).
 *
 * The C half of the ZVG path: open/validate, the foreground prefetch
 * (snd_zvg_prepare and the ZVG arm of snd_service) and the control and
 * status calls.  The tick half is snd_zvg.asm: one wait tick, then
 * complete commands from the ring up to the next wait or the end, the
 * bytes to the chosen PSG.  No C runs on the tick.
 *
 * The format is ZVGC v1/v2 (docs/snd-sdk.md 4.5): a 16-byte header,
 * then END, WRITE b, WAIT16 n, short waits 40h-7Fh and write runs
 * 80h-8Fh.  The bytes are one SN76489's; `chip` (0-3) selects the PSG
 * and reserves its four voices.
 */

#include "kos.h"

#include "snd_private.h"

#define ZVG_HEADER_BYTES    16u
#define ZVG_VERSION_V1      1u
#define ZVG_VERSION_V2      2u

/* snd_service reads at most this many bytes per call. */
/* (snd_service itself lives in snd_ztr.c, beside the ZTR arm.) */
#define ZVG_SERVICE_CHUNK   64u

static uint8_t header[ZVG_HEADER_BYTES];

/* The ring is 256-byte aligned inside its area: the decoder forms a
 * command byte's address as page:index and steps with an 8-bit
 * increment, which wraps at the ring's end for free. */
static uint8_t snd_zvg_ring_area[2u * SND_ZVG_RING];
uint8_t snd_zvg_ring_page;
#define ZVG_RING ((uint8_t *)((uint16_t)snd_zvg_ring_page << 8))
volatile uint8_t snd_zvg_head;
volatile uint8_t snd_zvg_tail;
uint8_t snd_zvg_chip;
uint8_t snd_zvg_state;
uint16_t snd_zvg_rate;
uint16_t snd_zvg_wait;
uint32_t snd_zvg_remaining;
uint32_t snd_zvg_consumed;
uint32_t snd_zvg_ticks;
uint32_t snd_zvg_writes;
uint16_t snd_zvg_underruns;
int snd_zvg_last_error;
uint32_t snd_zvg_error_offset;
uint8_t snd_zvg_error_op;
uint32_t snd_zvg_cursor;
uint8_t snd_zvg_eof;
kos_reader_t snd_zvg_reader;

static uint32_t get_le32(const uint8_t *data)
{
	return (uint32_t)data[0] |
	       ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) |
	       ((uint32_t)data[3] << 24);
}

static void zvg_reset_state(void)
{
	snd_zvg_head = 0u;
	snd_zvg_tail = 0u;
	snd_zvg_wait = 0u;
	snd_zvg_consumed = 0u;
	snd_zvg_ticks = 0u;
	snd_zvg_writes = 0u;
	snd_zvg_underruns = 0u;
	snd_zvg_last_error = 0;
	snd_zvg_error_offset = 0u;
	snd_zvg_error_op = 0u;
	snd_zvg_cursor = ZVG_HEADER_BYTES;
	snd_zvg_reader.context = (void *)0;
	snd_zvg_reader.read_at = (kos_read_at_fn)0;
	snd_zvg_reader.size = 0u;
	snd_zvg_eof = 0u;
}

int snd_zvg_open(const kos_reader_t *source, uint8_t chip)
{
	uint32_t declared_length;
	uint8_t voice;
	int result;

	if (source == (const kos_reader_t *)0 ||
	    source->read_at == (kos_read_at_fn)0 ||
	    snd_handle == 0) {
		return -KOS_EINVAL;
	}
	if (snd_zvg_state != SND_ZVG_IDLE) {
		return -KOS_EBUSY;
	}
	if (chip >= SND_CHIP_COUNT) {
		return -KOS_EINVAL;
	}
	/* The header checks, exactly the reference's. */
	if (source->size < ZVG_HEADER_BYTES) {
		return -KOS_EIO;
	}
	result = kos_reader_read_full(source, 0u, header, ZVG_HEADER_BYTES);
	if (result < 0) {
		return result;
	}
	if (header[0] != 'Z' || header[1] != 'V' ||
	    header[2] != 'G' || header[3] != 'C') {
		return -KOS_EINVAL;
	}
	if (header[4] != ZVG_VERSION_V1 && header[4] != ZVG_VERSION_V2) {
		return -KOS_ENOTSUP;
	}
	if (header[5] == 0u || header[5] > SND_MAX_TICK_RATE ||
	    header[7] != ZVG_HEADER_BYTES) {
		return -KOS_EINVAL;
	}
	declared_length = get_le32(&header[8]);
	if (declared_length == 0u ||
	    declared_length > source->size - ZVG_HEADER_BYTES) {
		return -KOS_EIO;
	}
	/* The chip's four voices must be free: no manual voice active and
	 * no ZTR/ZVG reservation. */
	for (voice = 0u; voice < 4u; ++voice) {
		uint8_t index = (uint8_t)((chip << 2) + voice);

		if (snd_voice_active[index] != 0u ||
		    (snd_reserved_voices &
		     (uint16_t)(1u << index)) != 0u) {
			return -KOS_EBUSY;
		}
	}
	/* Commit: the borrowed reader's copy, the reservation and the
	 * rate.  Nothing else changes until prepare/play. */
	snd_zvg_reader = *source;
	snd_zvg_chip = chip;
	snd_zvg_rate = header[5];
	snd_zvg_remaining = declared_length;
	snd_zvg_consumed = 0u;
	snd_zvg_wait = 0u;
	snd_zvg_writes = 0u;
	snd_zvg_underruns = 0u;
	snd_zvg_last_error = 0;
	snd_zvg_error_offset = 0u;
	snd_zvg_error_op = 0u;
	snd_zvg_cursor = ZVG_HEADER_BYTES;
	snd_zvg_ring_page = (uint8_t)
		(((uint16_t)snd_zvg_ring_area + (SND_ZVG_RING - 1u)) >> 8);
	snd_zvg_head = 0u;
	snd_zvg_tail = 0u;
	snd_zvg_eof = 0u;
	snd_reserved_voices |=
		(uint16_t)(0x000Fu << (chip << 2));
	snd_zvg_state = SND_ZVG_PREPARED;
	snd_active_rate = snd_zvg_rate;
	return 0;
}

/* One bounded read into the ring; the tail is published only after the
 * bytes are in.  Returns 0 when there is nothing more to produce, a
 * negative error, or the bytes moved. */
int snd_zvg_refill(void)
{
	uint8_t tail = snd_zvg_tail;
	uint8_t head = snd_zvg_head;
	unsigned int space = (unsigned int)(uint8_t)(head - tail - 1u);
	uint32_t wanted;
	int result;

	if (space == 0u) {
		return 0;
	}
	if (snd_zvg_remaining == 0u) {
		return 0;
	}
	wanted = snd_zvg_remaining;
	if (wanted > (uint32_t)space) {
		wanted = (uint32_t)space;
	}
	if (wanted > ZVG_SERVICE_CHUNK) {
		wanted = ZVG_SERVICE_CHUNK;
	}
	/* One contiguous copy up to the ring's end. */
	{
		unsigned int fit = SND_ZVG_RING - (unsigned int)tail;

		if ((uint32_t)fit > wanted) {
			fit = (unsigned int)wanted;
		}
		result = snd_zvg_reader.read_at(
			snd_zvg_reader.context, snd_zvg_cursor,
			&ZVG_RING[tail], (uint16_t)fit);
		if (result < 0) {
			snd_zvg_last_error = result;
			return result;
		}
		if (result == 0) {
			/* Nothing this time: a stalled source, or the reader's
			 * own end.  The decoder underruns; the next service
			 * retries here. */
			if (snd_zvg_cursor >= snd_zvg_reader.size) {
				snd_zvg_eof = 1u;
			}
			return 0;
		}
		tail = (uint8_t)(tail + (uint8_t)result);
		snd_zvg_tail = tail;	/* publish */
		snd_zvg_cursor += (uint32_t)result;
		snd_zvg_remaining -= (uint32_t)result;
		if (snd_zvg_remaining == 0u ||
		    snd_zvg_cursor >= snd_zvg_reader.size) {
			snd_zvg_eof = 1u; /* everything we'll produce is out */
		}
		return result;
	}
}

int snd_zvg_prepare(void)
{
	if (snd_zvg_state != SND_ZVG_PREPARED &&
	    snd_zvg_state != SND_ZVG_UNDERRUN) {
		return -KOS_EINVAL;
	}
	while (snd_zvg_remaining != 0u) {
		int result = snd_zvg_refill();

		if (result < 0) {
			return result;
		}
		if (result == 0) {
			break; /* the ring is full */
		}
	}
	return 0;
}

int snd_zvg_play(void)
{
	if (snd_zvg_state != SND_ZVG_PREPARED) {
		return -KOS_EINVAL;
	}
	/* A pending shadow rewrite (the card's state after open or a
	 * stop) goes out now, before the decoder's first writes, so it
	 * never silences the stream's opening commands. */
	snd_dirty = 1u;
	snd_output_flush();
	snd_zvg_state = SND_ZVG_PLAYING;
	return 0;
}

int snd_zvg_stop(void)
{
	uint8_t chip;

	if (snd_zvg_state == SND_ZVG_IDLE) {
		return 0;
	}
	chip = snd_zvg_chip;
	/* The chip's real registers are unknown to the shadow: the next
	 * flush rewrites it, muted. */
	zvg_reset_state();
	snd_zvg_state = SND_ZVG_IDLE;
	snd_reserved_voices &=
		(uint16_t)~(uint16_t)(0x000Fu << (chip << 2));
	snd_active_rate = snd_manual_rate;
	/* The invalidate mutes every desired attenuation and rewrites the
	 * whole card on the next flush. */
	snd_shadow_invalidate();
	snd_output_flush();
	return 0;
}

void snd_zvg_close(void)
{
	(void)snd_zvg_stop();
}

int snd_zvg_get_status(struct snd_zvg_status *out)
{
	if (out == (struct snd_zvg_status *)0) {
		return -KOS_EINVAL;
	}
	out->state = snd_zvg_state;
	out->ticks = snd_zvg_ticks;
	out->bytes = snd_zvg_consumed;
	out->writes = snd_zvg_writes;
	out->underruns = snd_zvg_underruns;
	out->last_error = snd_zvg_last_error;
	out->error_offset = snd_zvg_error_offset;
	return 0;
}
