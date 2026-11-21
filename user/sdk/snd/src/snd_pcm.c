/* KonaOS libsnd: PCM samples through the DAC (user/sdk/snd).
 *
 * The C half of the PCM path: the KPCM header, the staging buffer, the
 * page producer and the control and status calls.  The samples reach the
 * DAC from snd_pcm_rt.asm, the real-time handler, which runs in the RT
 * region at the stream's rate; codec 1's blocks decode in snd_pcm_dec.asm.
 * docs/snd-sdk.md section 12 has the design.
 *
 * Two buffers sit between the source and the DAC.  The caller's staging
 * buffer takes large reads (each read costs a command and a card access,
 * so fewer and larger is cheaper); the ring in the RT region (12 pages of
 * 256 samples) takes one decoded page at a time.  snd_pcm_service does at
 * most one read a call, then fills the ring from staging.
 */
#include "kos.h"
#include "kos/sound.h"
#include "snd_private.h"

/* The RT region's layout (snd_pcm_rt.asm keeps the same). */
#define PCM_RT_AT        ((uint8_t *)0xFC10u)
#define PCM_RT_END       0xFD00u
#define PCM_RD_LO        (*(volatile uint8_t *)0xFC12u)
#define PCM_RD_HI        (*(volatile uint8_t *)0xFC13u)
#define PCM_EOF          (*(volatile uint8_t *)0xFC03u)
#define PCM_UNDER        ((volatile uint8_t *)0xFC04u)
#define PCM_DRAIN        ((volatile uint8_t *)0xFC06u)
#define PCM_RING_FIRST   0xF0u
#define PCM_RING_TOP     0xFCu        /* exclusive: the state page    */
#define PCM_STAGE_PAGES  4u           /* the RT-region staging buffer */
/* The ring's end and size: 12 pages, or 8 with the staging buffer in the
 * RT region above them.  The producer holds one page less. */
#define PCM_RING_END     pcm_ring_end
#define PCM_RING_PAGES   (uint8_t)(pcm_ring_end - PCM_RING_FIRST)
#define PCM_RING_FILL    (uint8_t)(pcm_ring_end - PCM_RING_FIRST - 1u)

#define PCM_HEADER_SIZE  512u
#define PCM_NAME_OFFSET  32u
#define PCM_READ_UNIT    512u         /* reads stay sector-aligned */
#define PCM_BLOCK_MAX    257u         /* a codec 1 raw block       */
#define PCM_BLOCK_RAW    0u
#define PCM_BLOCK_DPCM4  1u

extern const uint8_t snd_pcm_rt_image[];
extern const uint8_t snd_pcm_rt_image_end[];
extern const uint8_t snd_pcm_rt_rd[];
extern const uint8_t snd_pcm_rt_out[];
extern const uint8_t snd_pcm_rt_wr[];
extern const uint8_t snd_pcm_rt_end[];
void snd_pcm_dpcm4(const uint8_t *src, uint8_t *page);

/* snd_pcm_dec.asm's predictor: the last sample decoded. */
uint8_t snd_pcm_pred;

static uint8_t pcm_state;
static uint8_t pcm_flags;
static uint8_t pcm_dac;
static uint8_t pcm_rt_open;
static uint8_t pcm_closed;            /* the closing silence page is in */
static int pcm_card;                  /* the card handle PCM holds, or 0 */
static uint8_t pcm_ring_end;          /* the ring's exclusive end page  */
static volatile uint8_t *pcm_wr;      /* the handler's wr operand       */
static kos_reader_t pcm_reader;
/* The header's fields; the name stays in the stream (snd_pcm_get_info
 * reads it back), so it costs the program no memory. */
static struct {
	uint8_t codec;
	uint16_t rate;
	uint32_t samples;
	uint32_t payload;
} pcm_info;
static uint8_t *pcm_stage;
static uint16_t pcm_stage_size;
static uint16_t pcm_head;             /* staging: next byte to use      */
static uint16_t pcm_tail;             /* staging: end of the data       */
static uint32_t pcm_cursor;           /* the source offset to read next */
static uint32_t pcm_unread;           /* payload bytes not yet read     */
static uint32_t pcm_left;             /* samples this pass still owes  */
static struct snd_pcm_status pcm_status;
static uint8_t pcm_gap;               /* 0: SND_PCM_GAP_US */
static uint8_t pcm_seen_rd;           /* rd's page at the last count    */
static uint16_t pcm_seen_under;
static uint32_t pcm_pages;            /* pages the handler moved past   */

static uint16_t pcm_u16(const uint8_t *at)
{
	return (uint16_t)((uint16_t)at[0] | ((uint16_t)at[1] << 8));
}

static uint32_t pcm_u32(const uint8_t *at)
{
	return (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
	       ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

/* A handler counter, read whole while the handler may step it. */
static uint16_t pcm_word(volatile uint8_t *at)
{
	uint8_t hi;
	uint8_t lo;

	do {
		hi = at[1];
		lo = at[0];
	} while (hi != at[1]);
	return (uint16_t)(((uint16_t)hi << 8) | lo);
}

/* The played pages, from rd's moves since the last count (the ring holds
 * 12 pages, and service runs far more often than 12 pages play), and the
 * handler's underrun count into the 32-bit totals.  A replayed page does
 * not move rd. */
static void pcm_count(void)
{
	uint8_t rd = PCM_RD_HI;
	uint16_t under = pcm_word(PCM_UNDER);

	pcm_pages += (uint8_t)(rd >= pcm_seen_rd
			       ? rd - pcm_seen_rd
			       : rd + PCM_RING_PAGES - pcm_seen_rd);
	pcm_seen_rd = rd;
	pcm_status.underruns += (uint32_t)(uint16_t)(under - pcm_seen_under)
				<< 8;
	pcm_seen_under = under;
	pcm_status.played = (pcm_pages << 8) + PCM_RD_LO;
}

static uint8_t pcm_used(void)
{
	uint8_t rd = PCM_RD_HI;
	uint8_t wr = *pcm_wr;

	return (uint8_t)(wr >= rd ? wr - rd : wr + PCM_RING_PAGES - rd);
}

/* The next ring page is in place: publish it (one byte). */
static void pcm_publish(void)
{
	uint8_t wr = (uint8_t)(*pcm_wr + 1u);

	if (wr == PCM_RING_END) {
		wr = PCM_RING_FIRST;
	}
	*pcm_wr = wr;
}

static uint8_t *pcm_page(void)
{
	return (uint8_t *)((uint16_t)*pcm_wr << 8);
}

/* Restart the payload: the next pass, from its first byte. */
static void pcm_rewind(void)
{
	pcm_cursor = PCM_HEADER_SIZE;
	pcm_unread = pcm_info.payload;
	pcm_left = pcm_info.samples;
	pcm_head = 0u;
	pcm_tail = 0u;
	snd_pcm_pred = 0x80u;
}

/* One read into staging: what fits, in whole read units, never past the
 * payload.  1 when it read, 0 when there was nothing to read, or the
 * reader's error. */
static int pcm_read(void)
{
	uint16_t room;
	uint16_t want;
	int got;

	if (pcm_unread == 0u) {
		return 0;
	}
	if (pcm_head != 0u) {
		uint16_t keep = (uint16_t)(pcm_tail - pcm_head);

		memmove(pcm_stage, pcm_stage + pcm_head, keep);
		pcm_head = 0u;
		pcm_tail = keep;
	}
	room = (uint16_t)(pcm_stage_size - pcm_tail);
	want = (uint16_t)(room & (uint16_t)~(PCM_READ_UNIT - 1u));
	if (want == 0u) {
		return 0;
	}
	if ((uint32_t)want > pcm_unread) {
		want = (uint16_t)pcm_unread;
	}
	got = pcm_reader.read_at(pcm_reader.context, pcm_cursor,
				 pcm_stage + pcm_tail, want);
	++pcm_status.reads;
	if (got < 0) {
		return got;
	}
	if (got == 0) {
		pcm_unread = 0u;        /* the source ended early */
		return 0;
	}
	pcm_tail = (uint16_t)(pcm_tail + (uint16_t)got);
	pcm_cursor += (uint32_t)got;
	pcm_unread -= (uint32_t)got;
	return 1;
}

/* The bytes the next page needs from staging, or 0 when staging holds
 * them already. */
static uint16_t pcm_need(void)
{
	uint16_t have = (uint16_t)(pcm_tail - pcm_head);
	uint16_t need;

	if (pcm_info.codec == SND_PCM_CODEC_U8) {
		need = pcm_left < 256u ? (uint16_t)pcm_left : 256u;
	} else if (have < 2u) {
		need = 2u;
	} else if (pcm_stage[pcm_head] == PCM_BLOCK_RAW) {
		need = PCM_BLOCK_MAX;
	} else {
		need = (uint16_t)(pcm_stage[pcm_head + 1u] + 3u);
	}
	return have >= need ? 0u : need;
}

/* One page from staging into the ring (pcm_need said it is all there).
 * 0, or -KOS_EIO for a block of no known kind. */
static int pcm_produce(void)
{
	uint8_t *page = pcm_page();
	const uint8_t *at = pcm_stage + pcm_head;
	uint16_t n = pcm_left < 256u ? (uint16_t)pcm_left : 256u;

	if (pcm_info.codec == SND_PCM_CODEC_U8) {
		memcpy(page, at, n);
		pcm_head = (uint16_t)(pcm_head + n);
	} else if (at[0] == PCM_BLOCK_RAW) {
		memcpy(page, at + 1, 256u);
		pcm_head = (uint16_t)(pcm_head + PCM_BLOCK_MAX);
	} else if (at[0] == PCM_BLOCK_DPCM4) {
		snd_pcm_dpcm4(at + 2, page);
		pcm_head = (uint16_t)(pcm_head + at[1] + 3u);
	} else {
		return -KOS_EIO;
	}
	if (n < 256u) {
		/* The stream's last page: its final sample holds. */
		memset(page + n, page[n - 1u], (uint16_t)(256u - n));
	}
	if (pcm_info.codec != SND_PCM_CODEC_U8) {
		snd_pcm_pred = page[255];
	}
	pcm_left -= n;
	pcm_publish();
	return 0;
}

/* The stream's end: one page of the midpoint, which the handler plays on
 * (the drain) until the clock stops. */
static void pcm_close_stream(void)
{
	memset(pcm_page(), 0x80u, 256u);
	pcm_publish();
	PCM_EOF = 1u;
	pcm_closed = 1u;
}

/* Fill the ring: up to `reads` source reads.  0, or an error. */
static int pcm_fill(uint8_t reads)
{
	int result;

	while (pcm_closed == 0u && pcm_used() < PCM_RING_FILL) {
		if (pcm_left == 0u) {
			if ((pcm_flags & SND_PCM_LOOP) != 0u &&
			    pcm_info.samples != 0u) {
				pcm_rewind();
				++pcm_status.loops;
				continue;
			}
			pcm_close_stream();
			break;
		}
		if (pcm_need() != 0u) {
			if (pcm_unread == 0u) {
				/* The source ended inside the stream: what is
				 * there plays, then the end. */
				pcm_left = 0u;
				continue;
			}
			if (reads == 0u) {
				break;
			}
			--reads;
			result = pcm_read();
			if (result < 0) {
				return result;
			}
			continue;
		}
		result = pcm_produce();
		if (result < 0) {
			return result;
		}
	}
	return 0;
}

static void pcm_fail(int error)
{
	pcm_status.last_error = error;
	(void)snd_pcm_stop();
	pcm_state = SND_PCM_ERROR;
}

int snd_pcm_open(const kos_reader_t *source, void *buffer, uint16_t size)
{
	struct ksound_info card;
	uint8_t *raw;
	uint8_t i;
	int handle;
	int result;

	if (pcm_state != SND_PCM_IDLE) {
		return -KOS_EBUSY;
	}
	if (source == (const kos_reader_t *)0 ||
	    source->read_at == (kos_read_at_fn)0 ||
	    (buffer != (void *)0 && size < SND_PCM_STAGE_MIN)) {
		return -KOS_EINVAL;
	}
	pcm_ring_end = PCM_RING_TOP;
	if (buffer == (void *)0) {
		/* Stage in the RT region, above a smaller ring: the region is
		 * not ours to write while another program holds it. */
		struct kos_rt_info rt;

		if (rt_info(&rt) < 0 || rt.held == KOS_RT_OTHER) {
			return -KOS_EBUSY;
		}
		pcm_ring_end = (uint8_t)(PCM_RING_TOP - PCM_STAGE_PAGES);
		buffer = (void *)((uint16_t)pcm_ring_end << 8);
		size = (uint16_t)PCM_STAGE_PAGES << 8;
	}
	/* The session's card, or the card held for the stream alone. */
	pcm_card = 0;
	handle = snd_handle;
	if (handle == 0) {
		unsigned char type = 0u;

		handle = bind("/dev/sound", KOS_BIND_EXCLUSIVE, &type);
		if (handle < 0) {
			return handle;
		}
		pcm_card = handle;
		if (type != KOS_TYPE_SOUND) {
			result = -KOS_ENODEV;
			goto refuse;
		}
	}
	if (sound_info(handle, &card) < 0) {
		result = -KOS_EIO;
		goto refuse;
	}
	if ((card.flags & (KSOUND_CAP_DIRECT_IO | KSOUND_CAP_DAC)) !=
	    (KSOUND_CAP_DIRECT_IO | KSOUND_CAP_DAC) || card.dac_port == 0u) {
		result = -KOS_ENOTSUP;
		goto refuse;
	}
	/* The header goes through the staging buffer. */
	raw = (uint8_t *)buffer;
	result = -KOS_EINVAL;
	if (source->size < PCM_HEADER_SIZE) {
		goto refuse;
	}
	result = kos_reader_read_full(source, 0u, raw, PCM_HEADER_SIZE);
	if (result < 0) {
		goto refuse;
	}
	result = -KOS_EINVAL;
	if (raw[0] != 'K' || raw[1] != 'P' || raw[2] != 'C' ||
	    raw[3] != 'M' || raw[4] != 1u || raw[6] != 1u || raw[7] != 8u) {
		goto refuse;
	}
	if (raw[5] != SND_PCM_CODEC_U8 && raw[5] != SND_PCM_CODEC_DPCM4) {
		result = -KOS_ENOTSUP;
		goto refuse;
	}
	pcm_info.codec = raw[5];
	{
		uint32_t rate = pcm_u32(raw + 8);

		if (rate < SND_PCM_RATE_MIN || rate > SND_PCM_RATE_MAX) {
			goto refuse;
		}
		pcm_info.rate = (uint16_t)rate;
	}
	pcm_info.samples = pcm_u32(raw + 12);
	pcm_info.payload = pcm_u32(raw + 16);
	if (pcm_u32(raw + 20) != (uint32_t)PCM_HEADER_SIZE ||
	    pcm_u16(raw + 24) != PCM_HEADER_SIZE ||
	    (pcm_info.codec == SND_PCM_CODEC_U8 &&
	     pcm_info.payload != pcm_info.samples) ||
	    pcm_info.payload > source->size - PCM_HEADER_SIZE) {
		goto refuse;
	}
	for (i = 0u; i < SND_PCM_NAME_SIZE; ++i) {
		if (raw[PCM_NAME_OFFSET + i] == 0u) {
			break;
		}
	}
	if (i == SND_PCM_NAME_SIZE) {
		goto refuse;
	}

	pcm_reader = *source;
	pcm_stage = raw;
	pcm_stage_size = size;
	pcm_dac = card.dac_port;
	pcm_flags = 0u;
	pcm_rt_open = 0u;
	memset(&pcm_status, 0, sizeof(pcm_status));
	pcm_rewind();
	pcm_state = SND_PCM_OPEN;
	pcm_status.state = SND_PCM_OPEN;
	snd_pcm_closer = snd_pcm_close;
	return 0;

refuse:
	if (pcm_card != 0) {
		(void)sound_close(pcm_card);
		pcm_card = 0;
	}
	return result;
}

int snd_pcm_get_info(struct snd_pcm_info *out)
{
	int result;

	if (out == (struct snd_pcm_info *)0 || pcm_state == SND_PCM_IDLE) {
		return -KOS_EINVAL;
	}
	out->codec = pcm_info.codec;
	out->rate = pcm_info.rate;
	out->samples = pcm_info.samples;
	out->payload = pcm_info.payload;
	result = kos_reader_read_full(&pcm_reader, PCM_NAME_OFFSET, out->name,
				      SND_PCM_NAME_SIZE);
	if (result < 0) {
		return result;
	}
	out->name[SND_PCM_NAME_SIZE - 1u] = '\0';   /* open found its NUL */
	return 0;
}

/* The handler into the RT region and the ring empty, before the first
 * fill.  -KOS_EBUSY while another program holds the RT interrupt (its
 * region is not ours to write). */
static int pcm_install(void)
{
	struct kos_rt_info rt;
	uint16_t size = (uint16_t)(snd_pcm_rt_image_end - snd_pcm_rt_image);

	if (rt_info(&rt) < 0 || rt.held == KOS_RT_OTHER) {
		return -KOS_EBUSY;
	}
	if ((uint16_t)PCM_RT_AT + size > PCM_RT_END ||
	    snd_pcm_rt_rd - snd_pcm_rt_image != 1) {
		return -KOS_EINVAL;
	}
	memcpy(PCM_RT_AT, snd_pcm_rt_image, size);
	PCM_RT_AT[(uint16_t)(snd_pcm_rt_out - snd_pcm_rt_image) + 1u] = pcm_dac;
	pcm_wr = PCM_RT_AT + (uint16_t)(snd_pcm_rt_wr - snd_pcm_rt_image) + 1u;
	PCM_RD_LO = 0u;
	PCM_RD_HI = PCM_RING_FIRST;
	*pcm_wr = PCM_RING_FIRST;
	PCM_RT_AT[(uint16_t)(snd_pcm_rt_end - snd_pcm_rt_image) + 1u] =
		pcm_ring_end;
	PCM_EOF = 0u;
	memset((void *)PCM_UNDER, 0, 4u);   /* UNDER, DRAIN */
	pcm_seen_rd = PCM_RING_FIRST;
	pcm_seen_under = 0u;
	pcm_pages = 0u;
	pcm_closed = 0u;
	return 0;
}

int snd_pcm_prepare(void)
{
	int result;

	if (pcm_state == SND_PCM_READY) {
		return 0;
	}
	if (pcm_state != SND_PCM_OPEN) {
		return -KOS_EINVAL;
	}
	result = pcm_install();
	if (result < 0) {
		return result;
	}
	result = pcm_fill(0xFFu);
	if (result < 0) {
		pcm_status.last_error = result;
		pcm_state = SND_PCM_ERROR;
		return result;
	}
	pcm_state = SND_PCM_READY;
	return 0;
}

int snd_pcm_play(uint8_t flags)
{
	int result;

	pcm_flags = flags;
	result = snd_pcm_prepare();
	if (result < 0) {
		return result;
	}
	(void)rt_gap(pcm_gap != 0u ? pcm_gap : SND_PCM_GAP_US);
	result = rt_open(PCM_RT_AT, pcm_info.rate);
	if (result < 0) {
		return result;
	}
	pcm_rt_open = 1u;
	pcm_state = SND_PCM_PLAYING;
	return 0;
}

int snd_pcm_set_gap(uint8_t us)
{
	if (us == 0u) {
		return -KOS_EINVAL;
	}
	pcm_gap = us;
	return 0;
}

int snd_pcm_service(void)
{
	int result;

	if (pcm_state != SND_PCM_PLAYING) {
		return pcm_state;
	}
	pcm_count();
	if (pcm_closed != 0u) {
		if (pcm_word(PCM_DRAIN) != 0u) {
			(void)snd_pcm_stop();     /* the end has played */
		}
		return pcm_state;
	}
	result = pcm_fill(1u);
	if (result < 0) {
		pcm_fail(result);
		return result;
	}
	return pcm_state;
}

/* Glide the DAC from `from` to the midpoint rather than step to it. */
/* Glide the DAC from `from` to `to` rather than step there: about 250 us a
 * step, so a full half-scale glide (128 steps) is a 30 ms fade, not a
 * click. */
#define PCM_GLIDE_SPIN 64u

static void pcm_glide(uint8_t from, uint8_t to)
{
	while (from != to) {
		volatile uint8_t spin;

		from = (uint8_t)(from < to ? from + 1u : from - 1u);
		sound_out(pcm_dac, from);
		for (spin = 0u; spin < PCM_GLIDE_SPIN; ++spin) {
		}
	}
}

/* The last sample the DAC took (the clock is stopped). */
static uint8_t pcm_last(void)
{
	uint8_t lo = PCM_RD_LO;
	uint8_t *at = (uint8_t *)(((uint16_t)PCM_RD_HI << 8) | lo);

	if (lo != 0u) {
		return at[-1];
	}
	return PCM_RD_HI == PCM_RING_FIRST
		? *(uint8_t *)(((uint16_t)PCM_RING_END << 8) - 1u)
		: at[-1];
}

/* Stop the RT interrupt and bring the DAC to its midpoint. */
static void pcm_halt(void)
{
	(void)rt_close();
	pcm_rt_open = 0u;
	pcm_count();
	pcm_glide(pcm_last(), 0x80u);
}

int snd_pcm_pause(void)
{
	int result;

	if (pcm_state != SND_PCM_PLAYING) {
		return -KOS_EINVAL;
	}
	/* The clock stops; the interrupt and its handles stay held, so the
	 * resume needs no free handle. */
	result = rt_rate(0u);
	if (result < 0) {
		return result;
	}
	pcm_count();
	pcm_glide(pcm_last(), 0x80u);
	pcm_state = SND_PCM_PAUSED;
	return 0;
}

int snd_pcm_resume(void)
{
	int result;

	if (pcm_state != SND_PCM_PAUSED) {
		return -KOS_EINVAL;
	}
	/* Back up to the next sample from the midpoint, then the clock. */
	pcm_glide(0x80u, *(uint8_t *)(((uint16_t)PCM_RD_HI << 8) | PCM_RD_LO));
	result = rt_rate(pcm_info.rate);
	if (result < 0) {
		pcm_glide(*(uint8_t *)(((uint16_t)PCM_RD_HI << 8) | PCM_RD_LO),
			  0x80u);
		return result;
	}
	pcm_state = SND_PCM_PLAYING;
	return 0;
}

int snd_pcm_stop(void)
{
	if (pcm_rt_open != 0u) {
		pcm_halt();
	}
	if (pcm_state == SND_PCM_PLAYING || pcm_state == SND_PCM_READY ||
	    pcm_state == SND_PCM_PAUSED) {
		pcm_state = SND_PCM_DONE;
	}
	return 0;
}

void snd_pcm_close(void)
{
	snd_pcm_closer = (void (*)(void))0;
	(void)snd_pcm_stop();
	pcm_state = SND_PCM_IDLE;
	if (pcm_card != 0) {
		(void)sound_close(pcm_card);   /* the release resets the card */
		pcm_card = 0;
	}
	pcm_reader.context = (void *)0;
	pcm_reader.read_at = (kos_read_at_fn)0;
	pcm_stage = (uint8_t *)0;
}

int snd_pcm_get_status(struct snd_pcm_status *out)
{
	if (out == (struct snd_pcm_status *)0) {
		return -KOS_EINVAL;
	}
	if (pcm_rt_open != 0u) {
		pcm_count();
	}
	pcm_status.state = pcm_state;
	*out = pcm_status;
	return 0;
}
