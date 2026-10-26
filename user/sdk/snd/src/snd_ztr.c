/* KonaOS libsnd: the ZTR song player (user/sdk/snd).
 *
 * The C half of the ZTR path: open/validate, the resident tables, the
 * foreground producer of the prepared-row queue (snd_ztr_prepare and
 * the ZTR arm of snd_service) and the control and status calls.  The
 * tick half is snd_ztr_dec.asm: one playback tick consumes the due row
 * record, applies the events, advances the macros, effects and
 * notation, and renders the channels into the library's descriptors
 * (compare-and-write).  No C runs on the tick.
 *
 * Open validates the whole song through the borrowed reader before any
 * hardware change: the header, the section bounds, the strings, the
 * instruments and their macros, every pattern stream and the note
 * table (docs/ZTR-Format.md).  What the tick needs stays resident: the
 * header fields, the divisor table (split into a lo page and the
 * following hi page), the order list, the remapped instrument
 * directory and the macro data.  Pattern streams are read in bounded
 * chunks, never whole.
 *
 * The queue holds prepared rows, fixed-size and already decoded: a
 * record's first two bytes are the order and row, then one 10-byte
 * slot per channel whose first byte is the decoded presence mask (0
 * for no event).  The producer publishes the tail only after the
 * record is complete; the consumer's head advances only after the row
 * is applied.  snd_service produces at most ZTR_SERVICE_ROWS rows per
 * call; snd_ztr_prepare fills the window until the queue is full.
 */

#include "kos.h"

#include "snd_private.h"

#define ZTR_HEADER_BYTES      64u
#define ZTR_NOTE_COUNT        96u
#define ZTR_CHANNELS          16u
#define ZTR_INSTRUMENT_BYTES  16u
#define ZTR_PATTERN_BYTES     8u
#define ZTR_MACRO_HEADER      12u
#define ZTR_MAX_FILE_BYTES    65535u
#define ZTR_CLOCK_HZ          3579545ul

/* The pattern-stream walk buffer and the service work bound. */
#define ZTR_STREAM_CHUNK      32u
#define ZTR_SERVICE_ROWS      8u
#define ZTR_EVENT_MAX_BYTES   13u

/* A page-aligned base inside an area; the host build (HOST_ZTR)
 * publishes no pages, so only the Z80 half exists. */
#ifndef HOST_ZTR
static uint8_t *page_base(uint8_t *area, uint16_t slack)
{
	return (uint8_t *)(uint16_t)((((uint16_t)area + slack) >> 8) << 8);
}
#endif

/* The resident pages' bases, shared by the loader and the producer's
 * caller-visible page bytes. */
static uint8_t *snd_ztr_div_lo;
static uint8_t *snd_ztr_loud;
static uint8_t *snd_ztr_instr;

/* Global state the assembly reaches. */
uint8_t snd_ztr_state;
uint16_t snd_ztr_rate;
uint8_t snd_ztr_speed;
uint16_t snd_ztr_pattern_length;
uint16_t snd_ztr_order_count;
uint8_t snd_ztr_head;
volatile uint8_t snd_ztr_tail;
uint16_t snd_ztr_queue_addrs[SND_ZTR_QUEUE_ROWS];
uint8_t snd_ztr_order;
uint8_t snd_ztr_row;
uint8_t snd_ztr_tick_in_row;
uint16_t snd_ztr_mask;
uint16_t snd_ztr_underruns;
int snd_ztr_last_error;
uint8_t snd_ztr_core_page;
uint8_t snd_ztr_core2_page;
uint8_t snd_ztr_macro_page[SND_ZTR_MACRO_SLOTS];
uint8_t snd_ztr_div_lo_page;
uint8_t snd_ztr_loud_page;
uint8_t snd_ztr_instr_page;
uint16_t snd_ztr_macro_base;

/* The resident tables.  The page-aligned ones share one area:
 * the core page, its effect/notation page, the five macro-slot
 * pages, the divisor lo page, the hi page after it, the loudness
 * page and the instrument page (eleven pages in all).  The macro
 * data and the queue need no page alignment (both are reached
 * through base addresses). */
static uint8_t snd_ztr_orders[SND_ZTR_MAX_ORDERS * ZTR_CHANNELS];
static uint8_t snd_ztr_patterns[SND_ZTR_MAX_PATTERNS * ZTR_PATTERN_BYTES];
#ifdef HOST_ZTR
static uint8_t host_pages_area[4u * 256u];
#else
static uint8_t snd_ztr_pages_area[11u * 256u + 255u];
#endif
static uint8_t snd_ztr_macro_area[SND_ZTR_MACRO_BYTES];
static uint8_t snd_ztr_queue_area[SND_ZTR_QUEUE_ROWS *
				  SND_ZTR_ROW_BYTES];

/* The borrowed reader and the producer's position. */
static kos_reader_t snd_ztr_reader;
static uint8_t header[ZTR_HEADER_BYTES];
static uint32_t ztr_size;         /* the logical size */
static uint32_t string_end;
static uint32_t pattern_dir_offset;
static uint32_t pattern_data_offset;
static uint32_t note_offset;
static uint8_t instrument_count;
static uint16_t pattern_count;
static uint32_t macro_used;
static uint16_t produce_order;
static uint16_t produce_row;

struct ztr_cursor {
	uint16_t next;               /* the next event's row byte */
	uint16_t end;                /* the stream's end */
	uint8_t active;
};
static struct ztr_cursor cursors[ZTR_CHANNELS];

/* --- the reader --------------------------------------------------------- */

/* One complete bounded read; -KOS_EIO at an early end. */
static int ztr_read(void *dst, uint32_t offset, uint16_t length)
{
	int result;

	result = kos_reader_read_full(&snd_ztr_reader, offset, dst, length);
	if (result < 0) {
		snd_ztr_last_error = result;
	}
	return result;
}

/* The stream walkers: three independent chunk walks over file ranges
 * (0 = the strings, 1 = the macros, 2 = the shared scan walker for
 * patterns, the note table and the divisor fill).  Flat statics and a
 * constant walker index keep the Z80 codegen simple absolute-memory
 * arithmetic - the pointer-based walker it replaces generated stack
 * shuffle code the SDCC port got wrong. */
static uint16_t ws_pos[3u];
static uint16_t ws_next[3u];
static uint16_t ws_end[3u];
static uint8_t ws_buf[3u][ZTR_STREAM_CHUNK];
static uint8_t ws_have[3u];
static uint8_t ws_at[3u];

static void zs_open(uint8_t w, uint16_t offset, uint16_t end)
{
	ws_pos[w] = offset;
	ws_next[w] = offset;
	ws_end[w] = end;
	ws_have[w] = 0u;
	ws_at[w] = 0u;
}

/* The next byte; 0, a negative error, or -KOS_EIO past the end. */
static int zs_get(uint8_t w, uint8_t *out)
{
	uint16_t left;
	uint16_t want;
	int result;

	if (ws_at[w] == ws_have[w]) {
		left = (uint16_t)(ws_end[w] - ws_next[w]);
		if (left == 0u) {
			return -KOS_EIO;
		}
		want = left;
		if (want > ZTR_STREAM_CHUNK) {
			want = ZTR_STREAM_CHUNK;
		}
		result = ztr_read(ws_buf[w], (uint32_t)ws_next[w], want);
		if (result < 0) {
			return result;
		}
		ws_pos[w] = ws_next[w];
		ws_next[w] = (uint16_t)(ws_next[w] + want);
		ws_at[w] = 0u;
		ws_have[w] = (uint8_t)want;
	}
	*out = ws_buf[w][ws_at[w]++];
	return 0;
}

/* --- validation --------------------------------------------------------- */

/* Shared scratch the validation reads into: a macro header and one
 * instrument entry. */
static uint8_t ztr_macro_head[ZTR_MACRO_HEADER];
static uint8_t ztr_instr_entry[ZTR_INSTRUMENT_BYTES];


static uint16_t u16_at(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t u32_at(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int string_ok(uint16_t offset, uint16_t pool_end)
{
	uint8_t byte;
	int result;

	if (offset < ZTR_HEADER_BYTES || offset >= pool_end) {
		return 0;
	}
	zs_open(0u, offset, pool_end);
	for (;;) {
		result = zs_get(0u, &byte);
		if (result < 0) {
			return 0;
		}
		if (byte == 0u) {
			return 1;
		}
	}
}

static int macro_ok(uint16_t offset, uint8_t type, uint16_t macro_start,
		    uint16_t macro_end_range)
{
	uint8_t *m = ztr_macro_head;
	uint16_t end;
	uint16_t count;
	uint16_t loop;
	uint16_t release;
	uint16_t i;
	int result;

	if (offset < macro_start || offset >= macro_end_range ||
	    offset + ZTR_MACRO_HEADER > macro_end_range) {
		return 0;
	}
	zs_open(1u, offset, macro_end_range);
	for (i = 0u; i < ZTR_MACRO_HEADER; ++i) {
		result = zs_get(1u, &m[i]);
		if (result < 0) {
			return 0;
		}
	}
	count = u16_at(m + 10);
	loop = u16_at(m + 6);
	release = u16_at(m + 8);
	end = (uint16_t)(offset + ZTR_MACRO_HEADER + count +
			 (count << 1));
	if (m[0] != type || m[1] > 2u || (m[2] & 0xFEu) != 0u ||
	    m[5] != 0u || count == 0u || end > macro_end_range ||
	    (loop != 0xFFFFu && loop >= count) ||
	    (release != 0xFFFFu && release >= count)) {
		return 0;
	}
	for (i = 0u; i < count; ++i) {
		uint8_t flags;
		uint8_t byte;

		result = zs_get(1u, &flags);
		if (result < 0 || (flags & 0xFEu) != 0u) {
			return 0;
		}
		result = zs_get(1u, &byte);
		if (result < 0) {
			return 0;
		}
		result = zs_get(1u, &byte);
		if (result < 0) {
			return 0;
		}
	}
	return 1;
}

static int pattern_ok(uint16_t entry_index, uint16_t data_start,
		      uint16_t data_end)
{
	uint8_t entry[ZTR_PATTERN_BYTES];
	uint16_t offset;
	uint16_t end;
	uint16_t expected;
	uint16_t events = 0u;
	uint16_t last_row = 0u;
	uint8_t first = 1u;
	int result;

	if (ztr_read(entry, (uint32_t)pattern_dir_offset +
		     ((uint32_t)entry_index << 3),
		     ZTR_PATTERN_BYTES) < 0) {
		return 0;
	}
	offset = u16_at(entry + 2);
	end = offset + u16_at(entry + 4);
	expected = u16_at(entry + 6);
	if (entry[0] >= ZTR_CHANNELS || offset < data_start ||
	    end > data_end) {
		return 0;
	}
	zs_open(2u, offset, end);
	while ((uint16_t)(ws_pos[2u] + ws_at[2u]) < end) {
		uint8_t row;
		uint8_t mask;
		uint8_t modifiers = 0u;
		uint8_t byte;

		result = zs_get(2u, &row);
		if (result < 0) {
			return 0;
		}
		result = zs_get(2u, &mask);
		if (result < 0) {
			return 0;
		}
		if ((uint16_t)row >= snd_ztr_pattern_length ||
		    (first == 0u && (uint16_t)row <= last_row) ||
		    mask == 0u) {
			return 0;
		}
		first = 0u;
		last_row = row;
		if ((mask & (ZTR_EVENT_OFF | ZTR_EVENT_RELEASE)) ==
		    (ZTR_EVENT_OFF | ZTR_EVENT_RELEASE)) {
			return 0;
		}
		if ((mask & ZTR_EVENT_NOTE) != 0u) {
			result = zs_get(2u, &byte);
			if (result < 0 || byte >= ZTR_NOTE_COUNT) {
				return 0;
			}
		}
		if ((mask & ZTR_EVENT_INSTRUMENT) != 0u) {
			result = zs_get(2u, &byte);
			if (result < 0 || byte >= ZTR_CHANNELS) {
				return 0;
			}
		}
		if ((mask & ZTR_EVENT_VOLUME) != 0u) {
			result = zs_get(2u, &byte);
			if (result < 0 || byte > 15u) {
				return 0;
			}
		}
		if ((mask & ZTR_EVENT_EFFECT) != 0u) {
			uint8_t effect;

			result = zs_get(2u, &effect);
			if (result < 0) {
				return 0;
			}
			result = zs_get(2u, &byte);
			if (result < 0 || effect == 0u ||
			    effect > ZTR_EFFECT_STOP) {
				return 0;
			}
		}
		if ((mask & ZTR_EVENT_LEGATO) != 0u &&
		    (mask & ZTR_EVENT_NOTE) == 0u) {
			return 0;
		}
		if ((mask & ZTR_EVENT_NOTE) != 0u &&
		    (mask & (ZTR_EVENT_OFF | ZTR_EVENT_RELEASE)) != 0u) {
			return 0;
		}
		if ((mask & ZTR_EVENT_MODIFIERS) != 0u) {
			result = zs_get(2u, &modifiers);
			if (result < 0 || (modifiers & 0x07u) != 0u ||
			    (modifiers & (ZTR_MOD_ARP_SET |
					  ZTR_MOD_ARP_CLEAR)) ==
			    (ZTR_MOD_ARP_SET | ZTR_MOD_ARP_CLEAR) ||
			    (modifiers & (ZTR_MOD_HAIRPIN_SET |
					  ZTR_MOD_HAIRPIN_CLEAR)) ==
			    (ZTR_MOD_HAIRPIN_SET |
			     ZTR_MOD_HAIRPIN_CLEAR)) {
				return 0;
			}
			if ((modifiers & ZTR_MOD_ARP_SET) != 0u) {
				result = zs_get(2u, &byte);
				if (result < 0) {
					return 0;
				}
			}
			if ((modifiers & ZTR_MOD_HAIRPIN_SET) != 0u) {
				uint8_t ceiling;

				result = zs_get(2u, &byte);
				if (result < 0) {
					return 0;
				}
				if (byte == 0u ||
				    (byte > 15u && byte < (uint8_t)0xF1u)) {
					return 0;
				}
				result = zs_get(2u, &ceiling);
				if (result < 0 || ceiling > 15u) {
					return 0;
				}
			}
		}
		++events;
	}
	return (uint16_t)(ws_pos[2u] + ws_at[2u]) == end &&
	       events == expected;
}

static int validate(void)
{
	uint32_t order_offset;
	uint32_t instrument_offset;
	uint32_t macro_offset;
	uint16_t i;
	uint8_t seen_instruments[2];
	int result;

	seen_instruments[0] = 0u;
	seen_instruments[1] = 0u;
	if (ztr_size < ZTR_HEADER_BYTES || header[0] != 'Z' ||
	    header[1] != 'T' || header[2] != 'R' || header[3] != '1' ||
	    header[4] != 1u || header[5] != ZTR_HEADER_BYTES ||
	    	    header[6] != 0u || header[7] != ZTR_CHANNELS) {
		return -KOS_EINVAL;
	}
	for (i = 55u; i < 64u; ++i) {
				if (header[i] != 0u) {
			return -KOS_EINVAL;
		}
	}
	if (u16_at(header + 8) == 0u || u16_at(header + 8) > 180u ||
	    header[9] != 0u || header[10] == 0u || header[11] == 0u ||
	    header[11] > ZTR_CHANNELS || u16_at(header + 12) == 0u ||
	    u16_at(header + 12) > 256u || u16_at(header + 14) == 0u ||
	    u16_at(header + 14) > 255u || u16_at(header + 18) > ZTR_CHANNELS ||
	    header[22] != 0u || header[23] != 0u ||
	    u32_at(header + 24) != ZTR_CLOCK_HZ ||
	    u16_at(header + 46) != ztr_size || header[50] != ZTR_CHANNELS ||
	    header[51] != ZTR_INSTRUMENT_BYTES ||
	    	    header[52] != ZTR_PATTERN_BYTES || header[53] != ZTR_NOTE_COUNT) {
		return -KOS_EINVAL;
	}
	string_end = (uint32_t)ZTR_HEADER_BYTES + u16_at(header + 48);
	order_offset = u16_at(header + 34);
	instrument_offset = u16_at(header + 36);
	macro_offset = u16_at(header + 38);
	pattern_dir_offset = u16_at(header + 40);
	pattern_data_offset = u16_at(header + 42);
	note_offset = u16_at(header + 44);
	if ((uint32_t)ZTR_HEADER_BYTES + u16_at(header + 48) > ztr_size ||
	    order_offset != string_end ||
	    order_offset + ((uint32_t)u16_at(header + 14) << 4) !=
	    instrument_offset ||
	    instrument_offset + ((uint32_t)u16_at(header + 18) << 4) !=
	    macro_offset ||
	    macro_offset > pattern_dir_offset ||
	    pattern_dir_offset + ((uint32_t)u16_at(header + 16) << 3) !=
	    pattern_data_offset ||
	    pattern_data_offset > note_offset ||
	    	    note_offset + ((uint32_t)ZTR_NOTE_COUNT << 1) != ztr_size) {
		return -KOS_EINVAL;
	}
	if (!string_ok(u16_at(header + 28), (uint16_t)string_end) ||
	    !string_ok(u16_at(header + 30), (uint16_t)string_end) ||
	    !string_ok(u16_at(header + 32), (uint16_t)string_end)) {
		return -KOS_EINVAL;
	}
	/* The header fields the engine and the producer keep. */
	snd_ztr_rate = u16_at(header + 8);
	snd_ztr_speed = header[10];
	snd_ztr_pattern_length = u16_at(header + 12);
	snd_ztr_order_count = u16_at(header + 14);
	pattern_count = u16_at(header + 16);
	instrument_count = (uint8_t)u16_at(header + 18);
	/* Instruments and their macros. */
	for (i = 0u; i < instrument_count; ++i) {
		uint8_t *entry = ztr_instr_entry;
		uint8_t number;
		uint8_t slot;

		result = ztr_read(header, instrument_offset +
				  ((uint32_t)i << 4),
				  ZTR_INSTRUMENT_BYTES);
		if (result < 0) {
			return result;
		}
		memcpy(entry, header, ZTR_INSTRUMENT_BYTES);
		number = entry[0];
		if (number >= ZTR_CHANNELS ||
		    (seen_instruments[number >> 3] &
		     (uint8_t)(1u << (number & 7u))) != 0u ||
		    entry[14] != 0u || entry[15] != 0u ||
		    !string_ok(u16_at(entry + 2), (uint16_t)string_end)) {
			return -KOS_EINVAL;
		}
		seen_instruments[number >> 3] |=
			(uint8_t)(1u << (number & 7u));
		for (slot = 0u; slot < SND_ZTR_MACRO_SLOTS; ++slot) {
			uint32_t offset = u16_at(entry + 4u + slot * 2u);

			if (offset != 0u && !macro_ok((uint16_t)offset,
						      (uint8_t)(slot + 1u),
						      (uint16_t)macro_offset,
						      (uint16_t)pattern_dir_offset)) {
				return -KOS_EINVAL;
			}
		}
	}
	/* Every pattern stream. */
	for (i = 0u; i < pattern_count; ++i) {
				if (!pattern_ok(i, (uint16_t)pattern_data_offset,
						(uint16_t)note_offset)) {
			return -KOS_EINVAL;
		}
	}
	/* The note table. */
	{
		uint16_t note;

		zs_open(2u, (uint16_t)note_offset,
			(uint16_t)(note_offset + 192u));
		for (note = 0u; note < ZTR_NOTE_COUNT; ++note) {
			uint8_t lo;
			uint8_t hi;

			result = zs_get(2u, &lo);
			if (result < 0) {
				return -KOS_EIO;
			}
			result = zs_get(2u, &hi);
			if (result < 0) {
				return -KOS_EIO;
			}
						if (lo == 0u && hi == 0u) {
				return -KOS_EINVAL;
			}
			if ((uint16_t)((uint16_t)lo |
				       				       ((uint16_t)hi << 8)) > 1023u) {
				return -KOS_EINVAL;
			}
		}
	}
	return 0;
}

/* --- resident loading --------------------------------------------------- */

/* Remap one macro's file offset to its resident offset, copying it
 * once per source offset.  48 distinct macros: both fixture songs fit
 * (35 and 17), and exceeding the bound is -KOS_ENOSPC at open. */
static uint16_t macro_remap[48u];
static uint16_t macro_remap_source[48u];
static uint8_t macro_remap_count;

static int resident_macro(uint32_t source_offset, uint16_t *out_offset)
{
	uint8_t m[ZTR_MACRO_HEADER];
	uint16_t count;
	uint32_t bytes;
	uint16_t i;
	int result;

	for (i = 0u; i < macro_remap_count; ++i) {
		if ((uint32_t)macro_remap_source[i] == source_offset) {
			*out_offset = macro_remap[i];
			return 0;
		}
	}
	if (macro_remap_count >= 48u) {
		return -KOS_ENOSPC;
	}
	result = ztr_read(m, source_offset, ZTR_MACRO_HEADER);
	if (result < 0) {
		return result;
	}
	count = u16_at(m + 10);
	bytes = ZTR_MACRO_HEADER + (uint32_t)count +
		((uint32_t)count << 1);
	if (macro_used + bytes > SND_ZTR_MACRO_BYTES) {
		return -KOS_ENOSPC;
	}
	result = ztr_read(snd_ztr_macro_area + macro_used, source_offset,
			  (uint16_t)bytes);
	if (result < 0) {
		return result;
	}
	macro_remap[macro_remap_count] = (uint16_t)macro_used;
	macro_remap_source[macro_remap_count] =
		(uint16_t)source_offset;
	++macro_remap_count;
	*out_offset = (uint16_t)macro_used;
	macro_used += bytes;
	return 0;
}

static int load_resident(void)
{
	uint8_t note;
	uint16_t i;
	int result;

	if (snd_ztr_order_count > SND_ZTR_MAX_ORDERS ||
	    pattern_count > SND_ZTR_MAX_PATTERNS) {
		return -KOS_ENOSPC;
	}
	result = ztr_read(snd_ztr_orders, string_end,
			  (uint16_t)(snd_ztr_order_count * ZTR_CHANNELS));
	if (result < 0) {
		return result;
	}
	result = ztr_read(snd_ztr_patterns, pattern_dir_offset,
			  (uint16_t)(pattern_count * ZTR_PATTERN_BYTES));
	if (result < 0) {
		return result;
	}
	/* The divisor table: a lo page, then the hi page after it.  The
	 * note values were already validated, so the pages are filled
	 * in place. */
	{
		uint8_t *lo_page = snd_ztr_div_lo;
		uint8_t *hi_page = lo_page + 256u;

		zs_open(2u, (uint16_t)note_offset,
			(uint16_t)(note_offset + 192u));
		for (note = 0u; note < ZTR_NOTE_COUNT; ++note) {
			result = zs_get(2u, &lo_page[note]);
			if (result < 0) {
				return -KOS_EIO;
			}
			result = zs_get(2u, &hi_page[note]);
			if (result < 0) {
				return -KOS_EIO;
			}
		}
	}
	/* The loudness page: base<<4|macro -> base*macro/15. */
	{
		uint8_t *loud = snd_ztr_loud;
		uint8_t base;

		for (base = 0u; base < 16u; ++base) {
			uint8_t macro;

			for (macro = 0u; macro < 16u; ++macro) {
				uint16_t product = (uint16_t)base *
					(uint16_t)macro;
				uint8_t value = 0u;

				/* base*macro/15, by repeated subtraction:
				 * the library links no 8-bit divide. */
				while (product >= 15u) {
					product -= 15u;
					++value;
				}
				loud[((uint16_t)base << 4) | macro] = value;
			}
		}
	}
	/* The instrument directory, remapped: one entry per number. */
	{
		uint8_t *instr = snd_ztr_instr;

		memset(instr, 0, 256u);
		for (i = 0u; i < instrument_count; ++i) {
			uint8_t *entry = ztr_instr_entry;
			uint8_t slot;

			result = ztr_read(entry,
					  string_end +
					  ((uint32_t)snd_ztr_order_count
					   << 4) +
					  ((uint32_t)i << 4),
					  ZTR_INSTRUMENT_BYTES);
			if (result < 0) {
				return result;
			}
			for (slot = 0u; slot < SND_ZTR_MACRO_SLOTS; ++slot) {
				uint32_t offset = u16_at(entry + 4u +
							 slot * 2u);
				uint16_t resident = 0u;

				if (offset != 0u) {
					result = resident_macro(offset,
							       &resident);
					if (result < 0) {
						return result;
					}
				}
				entry[4u + slot * 2u] =
					(uint8_t)(resident & 0xFFu);
				entry[5u + slot * 2u] =
					(uint8_t)(resident >> 8);
			}
			memcpy(instr + (uint16_t)entry[0] *
			       ZTR_INSTRUMENT_BYTES,
			       entry, ZTR_INSTRUMENT_BYTES);
		}
	}
	/* The reserved mask: every channel used by any order. */
	snd_ztr_mask = 0u;
	for (i = 0u; i < snd_ztr_order_count; ++i) {
		uint8_t channel;

		for (channel = 0u; channel < ZTR_CHANNELS; ++channel) {
			if (snd_ztr_orders[(uint16_t)i * ZTR_CHANNELS +
					   channel] != 0xFFu) {
				snd_ztr_mask |=
					(uint16_t)(1u << channel);
			}
		}
	}
	return 0;
}

/* --- the producer ------------------------------------------------------- */

static void setup_order(void)
{
	uint16_t i;
	uint8_t channel;

	for (channel = 0u; channel < ZTR_CHANNELS; ++channel) {
		cursors[channel].next = 0u;
		cursors[channel].end = 0u;
		cursors[channel].active = 0u;
	}
	/* Resolve all 16 selections in one directory pass, as the
	 * reference's setup_order does. */
	for (i = 0u; i < pattern_count; ++i) {
		const uint8_t *entry = snd_ztr_patterns + i * 8u;
		uint8_t number = entry[0];

		if (number < ZTR_CHANNELS &&
		    cursors[number].active == 0u &&
		    snd_ztr_orders[(uint16_t)produce_order * ZTR_CHANNELS +
				   number] != 0xFFu &&
		    entry[1] == snd_ztr_orders[(uint16_t)produce_order *
					       ZTR_CHANNELS + number]) {
			cursors[number].next = u16_at(entry + 2);
			cursors[number].end = cursors[number].next +
				u16_at(entry + 4);
			cursors[number].active = 1u;
		}
	}
}

/* One channel's event into the record's slot. */
static int decode_event(struct ztr_cursor *cursor, uint8_t produce_row_byte,
			uint8_t *slot)
{
	uint8_t buf[ZTR_EVENT_MAX_BYTES];
	uint16_t left = (uint16_t)(cursor->end - cursor->next);
	uint16_t amount;
	uint8_t pos = 2u;
	uint8_t mask;
	int result;

	slot[0] = 0u;
	if (left == 0u) {
		return 0;
	}
	amount = left;
	if (amount > ZTR_EVENT_MAX_BYTES) {
		amount = ZTR_EVENT_MAX_BYTES;
	}
	result = ztr_read(buf, (uint32_t)cursor->next, amount);
	if (result < 0) {
		return result;
	}
	if (buf[0] != produce_row_byte) {
		return 0; /* sparse: nothing on this row */
	}
	mask = buf[1];
	slot[0] = mask;
	slot[1] = 0xFFu;
	slot[2] = 0xFFu;
	slot[3] = 0xFFu;
	slot[4] = 0u;
	slot[5] = 0u;
	slot[6] = 0u;
	slot[7] = 0u;
	slot[8] = 0u;
	slot[9] = 0u;
	if ((mask & ZTR_EVENT_NOTE) != 0u) {
		slot[1] = buf[pos++];
	}
	if ((mask & ZTR_EVENT_INSTRUMENT) != 0u) {
		slot[2] = buf[pos++];
	}
	if ((mask & ZTR_EVENT_VOLUME) != 0u) {
		slot[3] = buf[pos++];
	}
	if ((mask & ZTR_EVENT_EFFECT) != 0u) {
		slot[4] = buf[pos++];
		slot[5] = buf[pos++];
	}
	if ((mask & ZTR_EVENT_MODIFIERS) != 0u) {
		slot[6] = buf[pos++];
		if ((slot[6] & ZTR_MOD_ARP_SET) != 0u) {
			slot[7] = buf[pos++];
		}
		if ((slot[6] & ZTR_MOD_HAIRPIN_SET) != 0u) {
			slot[8] = buf[pos++];
			slot[9] = buf[pos++];
		}
	}
	cursor->next += pos;
	return 0;
}

/* One row record into the queue slot at `tail`.  The tail is published
 * only by the caller, after the record is complete. */
/* The record at queue index i (the host producer test uses the same
 * entry point; the uint16 table is the assembly's view). */
uint8_t *snd_ztr_row_address(uint8_t index)
{
	return snd_ztr_queue_area + (uint16_t)index * SND_ZTR_ROW_BYTES;
}

static int produce_row_record(uint8_t tail)
{
	uint8_t *record = snd_ztr_row_address(tail);
	uint8_t channel;
	int result;

	/* The record is fixed-size and fully defined: inactive channels'
	 * slots are zero (the engine skips them on the mask byte alone,
	 * and the checksum contract pins every byte). */
	memset(record, 0, SND_ZTR_ROW_BYTES);
	record[0] = (uint8_t)produce_order;
	record[1] = (uint8_t)produce_row;
	for (channel = 0u; channel < ZTR_CHANNELS; ++channel) {
		uint8_t *slot = record + 2u +
			channel * SND_ZTR_SLOT_BYTES;

		if (cursors[channel].active != 0u) {
			result = decode_event(&cursors[channel],
					      (uint8_t)produce_row, slot);
			if (result < 0) {
				return result;
			}
		} else {
			slot[0] = 0u;
		}
	}
	/* Advance the producer's position. */
	++produce_row;
	if (produce_row >= snd_ztr_pattern_length) {
		produce_row = 0u;
		++produce_order;
		if (produce_order < snd_ztr_order_count) {
			setup_order();
		}
	}
	return 0;
}

/* Produce up to `bound` rows.  Returns 0, or a negative reader error. */
static uint8_t produce_done; /* every row the song has is published */

static int ztr_produce_rows(uint8_t bound)
{
	uint8_t produced = 0u;

	while (produced < bound && produce_order < snd_ztr_order_count) {
		uint8_t next = snd_ztr_tail + 1u;
		int result;

		if (next == SND_ZTR_QUEUE_ROWS) {
			next = 0u;
		}
		if (next == snd_ztr_head) {
			return 0; /* the queue is full */
		}
		result = produce_row_record(snd_ztr_tail);
		if (result < 0) {
			return result;
		}
		snd_ztr_tail = next; /* publish */
		++produced;
	}
	produce_done = produce_order >= snd_ztr_order_count;
	return 0;
}

/* The queue holds the start window when it is full or every row the
 * song has is already published. */
static uint8_t ztr_window_complete(void)
{
	uint8_t next = snd_ztr_tail + 1u;

	if (next == SND_ZTR_QUEUE_ROWS) {
		next = 0u;
	}
	return next == snd_ztr_head || produce_done != 0u;
}

/* --- the control surface ------------------------------------------------ */

static void ztr_reset_state(void)
{
	snd_ztr_head = 0u;
	snd_ztr_tail = 0u;
	snd_ztr_order = 0u;
	snd_ztr_row = 0u;
	snd_ztr_tick_in_row = 0u;
	snd_ztr_underruns = 0u;
	snd_ztr_last_error = 0;
	snd_ztr_rate = 0u;
	snd_ztr_speed = 0u;
	snd_ztr_pattern_length = 0u;
	snd_ztr_order_count = 0u;
	macro_remap_count = 0u;
	macro_used = 1u;	/* byte 0 reserved: a resident offset of
				 * zero means "no macro" to the engine */
	produce_done = 0u;
	produce_order = 0u;
	produce_row = 0u;
}

static void ztr_forget_reader(void)
{
	snd_ztr_reader.context = (void *)0;
	snd_ztr_reader.read_at = (kos_read_at_fn)0;
	snd_ztr_reader.size = 0u;
}

static void ztr_publish_pages(void)
{
	uint8_t *pages;
	uint8_t slot;
	uint8_t i;

	pages = (uint8_t *)0;
#ifdef HOST_ZTR
	/* The page bytes and the queue's uint16 table are the assembly's
	 * view; the host producer never runs the engine.  The host only
	 * needs scratch for the resident-table fillers. */
	pages = host_pages_area;
	snd_ztr_div_lo = pages;
	snd_ztr_loud = pages + 512u;
	snd_ztr_instr = pages + 768u;
	snd_ztr_div_lo_page = 0u;
	snd_ztr_loud_page = 0u;
	snd_ztr_instr_page = 0u;
	snd_ztr_core_page = 0u;
	snd_ztr_core2_page = 0u;
	for (slot = 0u; slot < SND_ZTR_MACRO_SLOTS; ++slot) {
		snd_ztr_macro_page[slot] = 0u;
	}
	snd_ztr_macro_base = 0u;
	for (i = 0u; i < SND_ZTR_QUEUE_ROWS; ++i) {
		snd_ztr_queue_addrs[i] = 0u;
	}
#else
	{
		uint8_t *page;

		page = page_base(snd_ztr_pages_area, 255u);
		snd_ztr_core_page = (uint8_t)((uint16_t)page >> 8);
		snd_ztr_core2_page = (uint8_t)(snd_ztr_core_page + 1u);
		for (slot = 0u; slot < SND_ZTR_MACRO_SLOTS; ++slot) {
			snd_ztr_macro_page[slot] =
				(uint8_t)(snd_ztr_core_page + 2u + slot);
		}
		snd_ztr_div_lo_page = (uint8_t)(snd_ztr_core_page + 7u);
		snd_ztr_loud_page = (uint8_t)(snd_ztr_core_page + 9u);
		snd_ztr_instr_page = (uint8_t)(snd_ztr_core_page + 10u);
		snd_ztr_div_lo = page + 7u * 256u;
		snd_ztr_loud = page + 9u * 256u;
		snd_ztr_instr = page + 10u * 256u;
	}
	snd_ztr_macro_base = (uint16_t)snd_ztr_macro_area;
	for (i = 0u; i < SND_ZTR_QUEUE_ROWS; ++i) {
		snd_ztr_queue_addrs[i] =
			(uint16_t)(((uint16_t)snd_ztr_queue_area +
				    (uint16_t)i * SND_ZTR_ROW_BYTES));
	}
#endif
}

int snd_ztr_open(const kos_reader_t *source)
{
	uint32_t logical;
	uint32_t rounded;
	uint32_t position;
	uint8_t byte;
	int result;

	if (source == (const kos_reader_t *)0 ||
	    	    source->read_at == (kos_read_at_fn)0 || snd_handle == 0) {
		return -KOS_EINVAL;
	}
	if (snd_ztr_state != SND_ZTR_EMPTY) {
		return -KOS_EBUSY;
	}
	if (source->size < ZTR_HEADER_BYTES ||
	    	    source->size > ZTR_MAX_FILE_BYTES) {
		return -KOS_EINVAL;
	}
	snd_ztr_reader = *source;
	snd_ztr_last_error = 0;
	result = ztr_read(header, 0u, ZTR_HEADER_BYTES);
	if (result < 0) {
		goto fail;
	}
	ztr_size = source->size;
	/* A file copied through CP/M may hold complete 128-byte records:
	 * accept exactly that representation of the logical size. */
	logical = u16_at(header + 46);
	if (logical >= ZTR_HEADER_BYTES && logical < ztr_size) {
		rounded = (logical + 127u) & 0xFF80u;
		if (ztr_size != rounded) {
			result = -KOS_EINVAL;
			goto fail;
		}
		for (position = logical; position < ztr_size; ++position) {
			result = ztr_read(&byte, position, 1u);
			if (result < 0) {
				goto fail;
			}
			if (byte != 0x1Au) {
				result = -KOS_EINVAL;
				goto fail;
			}
		}
		ztr_size = logical;
	}
	ztr_reset_state();
	result = validate();
	if (result < 0) {
		goto fail;
	}
	if ((snd_reserved_voices & snd_ztr_mask) != 0u) {
		result = -KOS_EBUSY;
		goto fail;
	}
	produce_order = 0u;
	produce_row = 0u;
	ztr_publish_pages();
	result = load_resident();
	if (result < 0) {
		goto fail;
	}
	snd_ztr_reset_channels();
	setup_order();
	snd_reserved_voices |= snd_ztr_mask;
	snd_active_rate = snd_ztr_rate;
	snd_ztr_state = SND_ZTR_PREPARING;
	return 0;
fail:
	ztr_reset_state();
	ztr_forget_reader();
	snd_ztr_state = SND_ZTR_EMPTY;
	snd_ztr_last_error = result;
	return result;
}

int snd_ztr_prepare(void)
{
	int result;

	if (snd_ztr_state != SND_ZTR_PREPARING &&
	    snd_ztr_state != SND_ZTR_UNDERRUN &&
	    	    snd_ztr_state != SND_ZTR_PLAYING) {
		return -KOS_EINVAL;
	}
	result = ztr_produce_rows(SND_ZTR_QUEUE_ROWS);
	if (result < 0) {
		snd_ztr_last_error = result;
		return result;
	}
	if (snd_ztr_state == SND_ZTR_PREPARING &&
	    ztr_window_complete()) {
		snd_ztr_state = SND_ZTR_READY;
	}
	/* SND_NEED_MORE until the start window is resident. */
	return snd_ztr_state == SND_ZTR_READY ? 0 : SND_NEED_MORE;
}

int snd_service(void)
{
	/* The ZVG arm: one bounded chunk per call. */
	if (snd_zvg_state == SND_ZVG_PREPARED ||
	    snd_zvg_state == SND_ZVG_PLAYING ||
	    snd_zvg_state == SND_ZVG_UNDERRUN) {
		(void)snd_zvg_refill();
	}
	/* The ZTR arm: at most ZTR_SERVICE_ROWS rows per call.  A reader
	 * failure stops the call; playback keeps going and underruns if
	 * the queue drains before the source recovers. */
	if (snd_ztr_state == SND_ZTR_PREPARING ||
	    snd_ztr_state == SND_ZTR_READY ||
	    snd_ztr_state == SND_ZTR_PLAYING ||
	    snd_ztr_state == SND_ZTR_UNDERRUN) {
		int result = ztr_produce_rows(ZTR_SERVICE_ROWS);

		if (result < 0) {
			snd_ztr_last_error = result;
			if (snd_ztr_state == SND_ZTR_PREPARING ||
			    snd_ztr_state == SND_ZTR_READY) {
				snd_ztr_state = SND_ZTR_ERROR;
			}
		} else if (snd_ztr_state == SND_ZTR_PREPARING &&
			   ztr_window_complete()) {
			snd_ztr_state = SND_ZTR_READY;
		} else if (snd_ztr_state == SND_ZTR_UNDERRUN) {
			/* Automatic recovery once the due row is
			 * resident. */
			uint8_t head = snd_ztr_head;

			if (head != snd_ztr_tail) {
				const uint8_t *record =
					snd_ztr_row_address(head);

				if (record[0] == snd_ztr_order &&
				    record[1] == snd_ztr_row) {
					snd_ztr_state = SND_ZTR_PLAYING;
				}
			}
		}
	}
	return 0;
}

int snd_ztr_play(void)
{
	uint16_t voice;
	uint8_t chip;

		if (snd_ztr_state != SND_ZTR_READY) {
		return -KOS_EINVAL;
	}
	/* The reference's mute left attenuation 15, period 0 and an
	 * unknown noise register: set the library's shadow to that, so
	 * tick 0's writes equal the golden's. */
	snd_shadow_invalidate();
	for (voice = 0u; voice < 16u; ++voice) {
		if ((snd_ztr_mask & (uint16_t)(1u << voice)) != 0u &&
		    (voice & 3u) != 3u) {
			snd_desired_period[voice >> 2][voice & 3u] = 0u;
		}
	}
	for (chip = 0u; chip < SND_CHIP_COUNT; ++chip) {
		if ((snd_ztr_mask & (uint16_t)(0x000Fu << (chip << 2))) !=
		    0u) {
			snd_desired_noise[chip] = 0xFFu;
		}
	}
	snd_output_flush();
#ifndef HOST_ZTR
	/* The reference's player_start leaves last_attenuation at 15
	 * after the mute: the engine's register cache follows. */
	{
		uint8_t *core2 = (uint8_t *)(uint16_t)
			((uint16_t)snd_ztr_core2_page << 8);

		for (voice = 0u; voice < 16u; ++voice) {
			core2[(voice << 4) + ZC2_LAST_ATT] = 15u;
		}
	}
#endif
	snd_ztr_state = SND_ZTR_PLAYING;
	return 0;
}

int snd_ztr_pause(void)
{
	if (snd_ztr_state != SND_ZTR_PLAYING &&
	    	    snd_ztr_state != SND_ZTR_UNDERRUN) {
		return -KOS_EINVAL;
	}
	snd_ztr_mute();
	snd_output_flush();
	snd_ztr_state = SND_ZTR_PAUSED;
	return 0;
}

int snd_ztr_resume(void)
{
		if (snd_ztr_state != SND_ZTR_PAUSED) {
		return -KOS_EINVAL;
	}
	snd_ztr_restore();
	snd_output_flush();
	snd_ztr_state = SND_ZTR_PLAYING;
	return 0;
}

int snd_ztr_stop(void)
{
	if (snd_ztr_state == SND_ZTR_EMPTY) {
		return 0;
	}
	snd_ztr_mute();
	snd_ztr_reset_channels();
	ztr_reset_state();
	ztr_forget_reader();
	snd_reserved_voices &= (uint16_t)~snd_ztr_mask;
	snd_ztr_mask = 0u;
	snd_active_rate = snd_manual_rate;
	snd_ztr_state = SND_ZTR_EMPTY;
	snd_output_flush();
	return 0;
}

void snd_ztr_close(void)
{
	(void)snd_ztr_stop();
}

int snd_ztr_get_status(struct snd_ztr_status *out)
{
		if (out == (struct snd_ztr_status *)0) {
		return -KOS_EINVAL;
	}
	out->state = snd_ztr_state;
	out->order = snd_ztr_order;
	out->row = snd_ztr_row;
	out->tick_in_row = snd_ztr_tick_in_row;
	out->rate = snd_ztr_rate;
	out->reserved_voices = snd_reserved_voices;
	out->last_error = snd_ztr_last_error;
	out->overruns = (uint16_t)snd_overruns;
	out->underruns = snd_ztr_underruns;
	return 0;
}
