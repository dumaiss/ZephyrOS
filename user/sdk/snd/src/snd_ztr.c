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
/* The resident pattern directory entry (the file's count field is
 * validation-only and dropped). */
#define ZTR_DIR_BYTES         6u
#define ZTR_MACRO_HEADER      12u
#define ZTR_MAX_FILE_BYTES    65535u
#define ZTR_CLOCK_HZ          3579545ul

/* The validation window: whole chunks through the borrowed reader
 * (the order buffers double as the scratch), so the walk loops make
 * no C call per byte.  256-byte windows keep the walk cursors 8-bit. */
#define ZTR_WINDOW_BYTES      256u

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
uint8_t snd_ztr_published;
volatile uint8_t snd_ztr_consumed;
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
#ifndef HOST_ZTR
uint16_t snd_ztr_order_base[2];
#endif

/* The resident tables.  The page-aligned ones share one area:
 * the core page, its effect/notation page, the five macro-slot
 * pages, the divisor lo page, the hi page after it, the loudness
 * page and the instrument page (eleven pages in all).  The macro
 * data and the queue need no page alignment (both are reached
 * through base addresses). */
static uint8_t snd_ztr_orders[SND_ZTR_MAX_ORDERS * ZTR_CHANNELS];
static uint8_t snd_ztr_patterns[SND_ZTR_MAX_PATTERNS * ZTR_DIR_BYTES];
#ifdef HOST_ZTR
static uint8_t host_pages_area[4u * 256u];
#else
static uint8_t snd_ztr_pages_area[11u * 256u + 255u];
#endif
static uint8_t snd_ztr_macro_area[SND_ZTR_MACRO_BYTES];
static uint8_t snd_ztr_order_area[2u * SND_ZTR_ORDER_BYTES];

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

/* The setup_order scan's selection, as three parallel arrays: the
 * channel index stays 8-bit, and each field is one indexed load. */
static uint16_t cursor_next[ZTR_CHANNELS];
static uint16_t cursor_end[ZTR_CHANNELS];
static uint8_t cursor_active[ZTR_CHANNELS];

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

/* Resident sections can exceed one filesystem request.  Keep their
 * first-pass reads bounded and monotonic. */
static int ztr_read_forward(void *dst, uint32_t offset, uint16_t length)
{
	uint16_t done = 0u;

	while (done < length) {
		uint16_t amount = (uint16_t)(length - done);
		int result;

		if (amount > 256u) amount = 256u;
		result = ztr_read((uint8_t *)dst + done, offset + done, amount);
		if (result < 0) return result;
		done = (uint16_t)(done + amount);
	}
	return (int)length;
}

/* The window walkers: forward walks over file ranges (the strings
 * and the pattern streams).  Each walker reads whole windows into an
 * order buffer (free until the first refill) and the loops consume
 * bytes in memory, so no C call runs per byte; the pattern walk
 * shares buffer 1 with the directory/note windows, above them.  The
 * state is two named sets, not indexed arrays: every access is a
 * constant address. */
static uint16_t ws0_pos;
static uint16_t ws0_next;
static uint16_t ws0_end;
static uint8_t *ws0_cur;
static uint8_t ws0_have;
static uint8_t ws0_at;
static uint16_t ws1_pos;
static uint16_t ws1_next;
static uint16_t ws1_end;
static uint8_t *ws1_cur;
static uint8_t ws1_have;
static uint8_t ws1_at;

static uint8_t *ztr_order_buffer(uint8_t slot)
{
#ifdef HOST_ZTR
	return snd_ztr_order_area + (uint16_t)slot * SND_ZTR_ORDER_BYTES;
#else
	return (uint8_t *)(uint16_t)snd_ztr_order_base[slot];
#endif
}

static void zs_open_strings(uint16_t offset, uint16_t end)
{
	ws0_pos = offset;
	ws0_next = offset;
	ws0_end = end;
	ws0_cur = ztr_order_buffer(0u);
	ws0_have = 0u;
	ws0_at = 0u;
}

static void zs_open_patterns(uint16_t offset, uint16_t end)
{
	ws1_pos = offset;
	ws1_next = offset;
	ws1_end = end;
	ws1_cur = ztr_order_buffer(1u) + ZTR_WINDOW_BYTES;
	ws1_have = 0u;
	ws1_at = 0u;
}

/* Fill the strings walk's window; 0 or a negative reader error. */
static int zs_fill_strings(void)
{
	uint16_t left = (uint16_t)(ws0_end - ws0_next);
	uint16_t want = left;
	int result;

	if (want > 254u) {
		/* ws0_have is 8-bit: a full window would truncate */
		want = 254u;
	}
	result = ztr_read(ws0_cur, (uint32_t)ws0_next, want);
	if (result < 0) {
		return result;
	}
	ws0_pos = ws0_next;
	ws0_next = (uint16_t)(ws0_next + want);
	ws0_have = (uint8_t)want;
	ws0_at = 0u;
	return 0;
}

/* Fill the pattern walk's window; 0 or a negative reader error.
 * An event may straddle the window end: compact the unread tail (at
 * most 12 bytes, the event cap) to the window's front and read the
 * continuation after it, so every reader request starts exactly
 * where the last one ended (the forward-only contract) and no event
 * bytes are dropped. */
static int zs_fill_patterns(void)
{
	uint16_t slack = (uint16_t)(ws1_have - ws1_at);
	uint16_t left;
	uint16_t want;
	int result;
	{
		uint8_t *to = ws1_cur;
		const uint8_t *from = ws1_cur + ws1_at;
		uint8_t count = (uint8_t)slack;

		while (count-- != 0u) {
			*to++ = *from++;
		}
	}
	left = (uint16_t)(ws1_end - ws1_next);
	want = left;
	if (want > 254u - slack) {
		/* ws1_have is 8-bit: a full window would truncate */
		want = 254u - slack;
	}
	result = ztr_read(ws1_cur + slack, (uint32_t)ws1_next, want);
	if (result < 0) {
		return result;
	}
	ws1_pos = (uint16_t)(ws1_next - slack);
	ws1_next = (uint16_t)(ws1_next + want);
	ws1_have = (uint8_t)(slack + want);
	ws1_at = 0u;
	return 0;
}

/* --- validation --------------------------------------------------------- */

static uint16_t u16_at(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t u32_at(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int string_ref_ok(uint16_t offset, uint16_t pool_end)
{
	return offset >= ZTR_HEADER_BYTES && offset < pool_end;
}

static int resident_macro_ok(uint16_t offset, uint8_t type,
			     uint16_t macro_start, uint16_t macro_end_range)
{
	const uint8_t *m;
	uint16_t end;
	uint16_t count;
	uint16_t loop;
	uint16_t release;
	uint16_t i;

	if (offset < macro_start || offset >= macro_end_range ||
	    offset + ZTR_MACRO_HEADER > macro_end_range) {
		return 0;
	}
	m = snd_ztr_macro_area + 1u + (uint16_t)(offset - macro_start);
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
		if ((m[ZTR_MACRO_HEADER + i * 3u] & 0xFEu) != 0u) {
			return 0;
		}
	}
	return 1;
}

/* Every pattern stream, in file order: one window walker over the
 * whole pattern-data region, so the open pass pays at most one reader
 * call per 255 stream bytes.  The directory pass left each entry's
 * expected event count in the order scratch; the streams may leave
 * gaps (the walker jumps forward over them, all reads monotonic). */
static int pattern_streams_ok(const uint8_t *counts)
{
	const uint8_t *entry = snd_ztr_patterns;
	uint16_t i;

	zs_open_patterns((uint16_t)pattern_data_offset,
			 (uint16_t)note_offset);
	for (i = 0u; i < pattern_count; ++i) {
		uint16_t offset = u16_at(entry + 2);
		uint16_t end = (uint16_t)(offset + u16_at(entry + 4));
		uint16_t expected = u16_at(counts);
		uint16_t events = 0u;
		uint16_t last_row = 0u;
		uint8_t first = 1u;
		int result;

		entry += ZTR_DIR_BYTES;
		counts += 2u;

		if (ws1_next < offset) {
			/* jump the walker to this stream's first byte */
			ws1_pos = offset;
			ws1_next = offset;
			ws1_have = 0u;
			ws1_at = 0u;
		} else {
			ws1_at = (uint8_t)(offset - ws1_pos);
		}
		while ((uint16_t)(ws1_pos + ws1_at) < end) {
			uint8_t row;
			uint8_t mask;
			uint8_t modifiers = 0u;
			uint8_t byte;

			/* One fill per event: an event is at most 13
			 * bytes, so with the slack resident the walk
			 * needs no further C call (a stream ending
			 * inside the slack reads only its own bytes;
			 * the end equality catches a truncated
			 * event). */
			if ((int)(ws1_have - ws1_at) < 13 &&
			    ws1_next < ws1_end) {
				result = zs_fill_patterns();
				if (result < 0) {
					return 0;
				}
			}
			row = ws1_cur[ws1_at++];
			mask = ws1_cur[ws1_at++];
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
				byte = ws1_cur[ws1_at++];
				if (byte >= ZTR_NOTE_COUNT) {
					return 0;
				}
			}
			if ((mask & ZTR_EVENT_INSTRUMENT) != 0u) {
				byte = ws1_cur[ws1_at++];
				if (byte >= ZTR_CHANNELS) {
					return 0;
				}
			}
			if ((mask & ZTR_EVENT_VOLUME) != 0u) {
				byte = ws1_cur[ws1_at++];
				if (byte > 15u) {
					return 0;
				}
			}
			if ((mask & ZTR_EVENT_EFFECT) != 0u) {
				uint8_t effect;

				effect = ws1_cur[ws1_at++];
				byte = ws1_cur[ws1_at++];
				if (effect == 0u || effect > ZTR_EFFECT_STOP) {
					return 0;
				}
			}
			if ((mask & ZTR_EVENT_LEGATO) != 0u &&
			    (mask & ZTR_EVENT_NOTE) == 0u) {
				return 0;
			}
			if ((mask & ZTR_EVENT_NOTE) != 0u &&
			    (mask & (ZTR_EVENT_OFF | ZTR_EVENT_RELEASE)) !=
			    0u) {
				return 0;
			}
			if ((mask & ZTR_EVENT_MODIFIERS) != 0u) {
				modifiers = ws1_cur[ws1_at++];
				if ((modifiers & 0x07u) != 0u ||
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
					byte = ws1_cur[ws1_at++];
				}
				if ((modifiers & ZTR_MOD_HAIRPIN_SET) != 0u) {
					uint8_t ceiling;

					byte = ws1_cur[ws1_at++];
					if (byte == 0u ||
					    (byte > 15u &&
					     byte < (uint8_t)0xF1u)) {
						return 0;
					}
					ceiling = ws1_cur[ws1_at++];
					if (ceiling > 15u) {
						return 0;
					}
				}
			}
			++events;
		}
		if ((uint16_t)(ws1_pos + ws1_at) != end ||
		    events != expected) {
			return 0;
		}
	}
	return 1;
}

static int validate(void)
{
	uint32_t order_offset;
	uint32_t instrument_offset;
	uint32_t macro_offset;
	uint16_t i;
	uint8_t seen_instruments[2];
	uint8_t byte;
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
	/* The header fields the engine and the producer keep. */
	snd_ztr_rate = u16_at(header + 8);
	snd_ztr_speed = header[10];
	snd_ztr_pattern_length = u16_at(header + 12);
	snd_ztr_order_count = u16_at(header + 14);
	pattern_count = u16_at(header + 16);
	instrument_count = (uint8_t)u16_at(header + 18);
	if (snd_ztr_order_count > SND_ZTR_MAX_ORDERS ||
	    pattern_count > SND_ZTR_MAX_PATTERNS ||
	    (uint32_t)(pattern_dir_offset - macro_offset) + 1u >
	    SND_ZTR_MACRO_BYTES) {
		return -KOS_ENOSPC;
	}
	/* Strings are one forward window.  The pool's final NUL guarantees
	 * every in-range reference terminates without a seek back. */
	if (!string_ref_ok(u16_at(header + 28), (uint16_t)string_end) ||
	    !string_ref_ok(u16_at(header + 30), (uint16_t)string_end) ||
	    !string_ref_ok(u16_at(header + 32), (uint16_t)string_end)) {
		return -KOS_EINVAL;
	}
	zs_open_strings(ZTR_HEADER_BYTES, (uint16_t)string_end);
	byte = 1u;
	while ((uint16_t)(ws0_pos + ws0_at) < (uint16_t)string_end) {
		if (ws0_at == ws0_have) {
			result = zs_fill_strings();
			if (result < 0) return result;
		}
		byte = ws0_cur[ws0_at++];
	}
	if (byte != 0u) return -KOS_EINVAL;

	/* Populate each resident section on its first encounter. */
	result = ztr_read_forward(snd_ztr_orders, order_offset,
			  (uint16_t)(snd_ztr_order_count * ZTR_CHANNELS));
	if (result < 0) return result;
	memset(snd_ztr_instr, 0, 256u);
	/* The instrument table is one contiguous read: the window
	 * loops must not pay one reader call per instrument. */
	result = ztr_read_forward(ztr_order_buffer(1u), instrument_offset,
				  (uint16_t)((uint16_t)instrument_count *
					     ZTR_INSTRUMENT_BYTES));
	if (result < 0) {
		return result;
	}
	for (i = 0u; i < instrument_count; ++i) {
		const uint8_t *entry = ztr_order_buffer(1u) +
			i * ZTR_INSTRUMENT_BYTES;
		uint8_t number = entry[0];

		if (number >= ZTR_CHANNELS ||
		    (seen_instruments[number >> 3] &
		     (uint8_t)(1u << (number & 7u))) != 0u ||
		    entry[14] != 0u || entry[15] != 0u ||
		    !string_ref_ok(u16_at(entry + 2),
				   (uint16_t)string_end)) {
			return -KOS_EINVAL;
		}
		seen_instruments[number >> 3] |=
			(uint8_t)(1u << (number & 7u));
		memcpy(snd_ztr_instr + (uint16_t)number *
		       ZTR_INSTRUMENT_BYTES, entry, ZTR_INSTRUMENT_BYTES);
	}
	macro_used = (uint32_t)(pattern_dir_offset - macro_offset) + 1u;
	snd_ztr_macro_area[0] = 0u;
	if (macro_used > 1u) {
		result = ztr_read_forward(snd_ztr_macro_area + 1u, macro_offset,
				  (uint16_t)(macro_used - 1u));
		if (result < 0) return result;
	}
	/* Validate and remap the directory's absolute macro offsets to the
	 * one-based resident macro area. */
	for (i = 0u; i < ZTR_CHANNELS; ++i) {
		uint8_t *entry;
		uint8_t slot;

		if ((seen_instruments[i >> 3] &
		     (uint8_t)(1u << (i & 7u))) == 0u) continue;
		entry = snd_ztr_instr + i * ZTR_INSTRUMENT_BYTES;
		for (slot = 0u; slot < SND_ZTR_MACRO_SLOTS; ++slot) {
			uint16_t offset = u16_at(entry + 4u + slot * 2u);
			uint16_t resident = 0u;

			if (offset != 0u) {
				if (!resident_macro_ok(offset,
						       (uint8_t)(slot + 1u),
						       (uint16_t)macro_offset,
						       (uint16_t)pattern_dir_offset)) {
					return -KOS_EINVAL;
				}
				resident = (uint16_t)(1u + offset - macro_offset);
			}
			entry[4u + slot * 2u] = (uint8_t)resident;
			entry[5u + slot * 2u] = (uint8_t)(resident >> 8);
		}
	}
	/* The pattern directory, windowed through the order scratch:
	 * every window is read first (one forward pass), each entry
	 * validated and kept resident compacted (the count field is
	 * validation-only and held in the other order buffer), then
	 * every stream is walked in file order. */
	{
		uint8_t *counts = ztr_order_buffer(0u);
		uint8_t *count = counts;	/* advancing */
		uint8_t *resident = snd_ztr_patterns;
		uint16_t done = 0u;

		while (done < pattern_count) {
			uint16_t window = pattern_count - done;
			uint16_t j;

			if (window > ZTR_WINDOW_BYTES / ZTR_PATTERN_BYTES) {
				window = ZTR_WINDOW_BYTES /
					ZTR_PATTERN_BYTES;
			}
			result = ztr_read(ztr_order_buffer(1u),
					  pattern_dir_offset +
					  (uint32_t)done * ZTR_PATTERN_BYTES,
					  (uint16_t)(window *
						     ZTR_PATTERN_BYTES));
			if (result < 0) return result;
			for (j = 0u; j < window; ++j) {
				const uint8_t *entry =
					ztr_order_buffer(1u) + j *
					ZTR_PATTERN_BYTES;
				uint16_t index = (uint16_t)(done + j);
				uint16_t offset = u16_at(entry + 2);
				uint16_t length = u16_at(entry + 4);

				if (entry[0] >= ZTR_CHANNELS ||
				    offset < pattern_data_offset ||
				    (uint16_t)(offset + length) >
				    note_offset) {
					return -KOS_EINVAL;
				}
				if (index != 0u) {
					uint16_t prior = (uint16_t)(
						u16_at(resident - 6u + 2u) +
						u16_at(resident - 6u + 4u));

					if (offset < prior) {
						return -KOS_EINVAL;
					}
				}
				memcpy(resident, entry, ZTR_DIR_BYTES);
				count[0] = entry[6];
				count[1] = entry[7];
				resident += ZTR_DIR_BYTES;
				count += 2u;
			}
			done = (uint16_t)(done + window);
		}
		if (!pattern_streams_ok(counts)) {
			return -KOS_EINVAL;
		}
	}
	/* The note table: one whole-window read into the scratch, then
	 * the divisors into the split pages. */
	{
		uint16_t note;
		uint8_t *lo_page = snd_ztr_div_lo;
		uint8_t *hi_page = lo_page + 256u;
		const uint8_t *table = ztr_order_buffer(1u);

		result = ztr_read(ztr_order_buffer(1u), note_offset,
				  (uint16_t)(ZTR_NOTE_COUNT << 1));
		if (result < 0) {
			return -KOS_EIO;
		}
		for (note = 0u; note < ZTR_NOTE_COUNT; ++note) {
			uint8_t lo = table[note << 1];
			uint8_t hi = table[(note << 1) + 1u];

			if (lo == 0u && hi == 0u) {
				return -KOS_EINVAL;
			}
			if ((uint16_t)((uint16_t)lo |
				       ((uint16_t)hi << 8)) > 1023u) {
				return -KOS_EINVAL;
			}
			lo_page[note] = lo;
			hi_page[note] = hi;
		}
	}
	return 0;
}

/* --- resident loading --------------------------------------------------- */

static int load_resident(void)
{
	uint16_t i;
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

static void setup_order_for(uint16_t order)
{
	const uint8_t *entry = snd_ztr_patterns;
	uint16_t i;
	uint8_t channel;

	for (channel = 0u; channel < ZTR_CHANNELS; ++channel) {
		cursor_next[channel] = 0u;
		cursor_end[channel] = 0u;
		cursor_active[channel] = 0u;
	}
	/* Resolve all 16 selections in one directory pass, as the
	 * reference's setup_order does. */
	for (i = 0u; i < pattern_count; ++i, entry += ZTR_DIR_BYTES) {
		uint8_t number = entry[0];

		if (number < ZTR_CHANNELS &&
		    cursor_active[number] == 0u &&
		    snd_ztr_orders[order * ZTR_CHANNELS + number] != 0xFFu &&
		    entry[1] == snd_ztr_orders[order * ZTR_CHANNELS +
					      number]) {
			cursor_next[number] = u16_at(entry + 2);
			cursor_end[number] = cursor_next[number] +
				u16_at(entry + 4);
			cursor_active[number] = 1u;
		}
	}
}

/* Fill one order buffer: the cursor table (a start and an end offset
 * per channel, both zero for an FFh selection), then each selected
 * stream whole, one read per stream.  The streams pack right after
 * the table; the offsets are buffer-relative.  Returns 0 or a
 * negative reader error (the caller publishes nothing on error). */
static int refill_order(uint8_t order, uint8_t slot)
{
	uint8_t *buffer = ztr_order_buffer(slot);
	uint8_t *entry = buffer;
	uint16_t pos = SND_ZTR_ORDER_TABLE;
	uint8_t channel;

	setup_order_for(order);
	for (channel = 0u; channel < ZTR_CHANNELS; ++channel, entry += 4u) {
		uint16_t length;
		int result;

		if (cursor_active[channel] == 0u) {
			/* both offsets stay zero: no stream */
			entry[0] = 0u;
			entry[1] = 0u;
			entry[2] = 0u;
			entry[3] = 0u;
			continue;
		}
		length = (uint16_t)(cursor_end[channel] -
				    cursor_next[channel]);
		if (length == 0u) {
			/* both offsets stay zero: no stream */
			entry[0] = 0u;
			entry[1] = 0u;
			entry[2] = 0u;
			entry[3] = 0u;
			continue;
		}
		entry[0] = (uint8_t)pos;
		entry[1] = (uint8_t)(pos >> 8);
		result = ztr_read(buffer + pos,
				  (uint32_t)cursor_next[channel],
				  length);
		if (result < 0) {
			return result;
		}
		pos = (uint16_t)(pos + length);
		entry[2] = (uint8_t)pos;
		entry[3] = (uint8_t)(pos >> 8);
	}
	return 0;
}

/* --- the control surface ------------------------------------------------ */

static void ztr_reset_state(void)
{
	snd_ztr_published = 0xFFu;   /* nothing pending */
	snd_ztr_consumed = 0u;
	snd_ztr_order = 0u;
	snd_ztr_row = 0u;
	snd_ztr_tick_in_row = 0u;
	snd_ztr_underruns = 0u;
	snd_ztr_last_error = 0;
	snd_ztr_rate = 0u;
	snd_ztr_speed = 0u;
	snd_ztr_pattern_length = 0u;
	snd_ztr_order_count = 0u;
	macro_used = 1u;	/* byte 0 reserved: a resident offset of
				 * zero means "no macro" to the engine */
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

	pages = (uint8_t *)0;
#ifdef HOST_ZTR
	/* The page bytes are the assembly's view; the host producer
	 * never runs the engine.  The host only needs scratch for the
	 * resident-table fillers. */
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
	snd_ztr_order_base[0] = (uint16_t)snd_ztr_order_area;
	snd_ztr_order_base[1] = (uint16_t)(snd_ztr_order_area +
					   SND_ZTR_ORDER_BYTES);
#endif
}

int snd_ztr_open(const kos_reader_t *source)
{
	uint32_t logical;
	uint32_t rounded;
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
	 * accept exactly that representation of the logical size.  Its
	 * padding is checked after the logical song's forward pass. */
	logical = u16_at(header + 46);
	if (logical >= ZTR_HEADER_BYTES && logical < ztr_size) {
		rounded = (logical + 127u) & 0xFF80u;
		if (ztr_size != rounded) {
			result = -KOS_EINVAL;
			goto fail;
		}
		ztr_size = logical;
	}
	ztr_reset_state();
	ztr_publish_pages();
	result = validate();
	if (result < 0) {
		goto fail;
	}
	if ((snd_reserved_voices & snd_ztr_mask) != 0u) {
		result = -KOS_EBUSY;
		goto fail;
	}
	result = load_resident();
	if (result < 0) {
		goto fail;
	}
	/* The largest order's stream total, over the resident
	 * directory: a larger one is -KOS_ENOSPC. */
	{
		uint16_t largest = 0u;
		uint16_t order;

		for (order = 0u; order < snd_ztr_order_count; ++order) {
			uint16_t total = 0u;
			uint8_t channel;

			setup_order_for(order);
			for (channel = 0u; channel < ZTR_CHANNELS;
			     ++channel) {
				if (cursor_active[channel] != 0u) {
					total = (uint16_t)(total +
						(uint16_t)(cursor_end[channel] -
							   cursor_next[channel]));
				}
			}
			if (total > largest) {
				largest = total;
			}
		}
		if (largest > SND_ZTR_ORDER_STREAM) {
			result = -KOS_ENOSPC;
			goto fail;
		}
	}
	if (source->size > ztr_size) {
		/* The CP/M padding (at most one 128-byte record, all
		 * 1Ah): one whole read through the scratch, then a
		 * pointer walk. */
		const uint8_t *pad = ztr_order_buffer(0u);
		uint16_t left = (uint16_t)(source->size - ztr_size);

		result = ztr_read(ztr_order_buffer(0u), ztr_size, left);
		if (result < 0) {
			goto fail;
		}
		while (left != 0u) {
			if (*pad != 0x1Au) {
				result = -KOS_EINVAL;
				goto fail;
			}
			++pad;
			--left;
		}
	}
	snd_ztr_reset_channels();
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
	/* Order 0 into buffer 0, published after it is complete. */
	if (snd_ztr_state == SND_ZTR_PREPARING) {
		result = refill_order(0u, 0u);
		if (result < 0) {
			snd_ztr_last_error = result;
			return result;
		}
		snd_ztr_published = 0u;
		snd_ztr_state = SND_ZTR_READY;
	}
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
	/* The ZTR arm: one order refill per call, always the order
	 * after the one the engine consumed.  A reader failure is
	 * recorded; playback underruns when the next order change
	 * finds nothing published and resumes here once the due order
	 * is resident. */
	if (snd_ztr_state == SND_ZTR_PLAYING ||
	    snd_ztr_state == SND_ZTR_UNDERRUN ||
	    snd_ztr_state == SND_ZTR_READY) {
		if (snd_ztr_published == snd_ztr_consumed &&
		    (uint16_t)snd_ztr_consumed + 1u <
		    snd_ztr_order_count) {
			uint8_t order = (uint8_t)(snd_ztr_consumed + 1u);
			int result = refill_order(order, order & 1u);

			if (result < 0) {
				snd_ztr_last_error = result;
			} else {
				snd_ztr_published = order;
			}
		} else if (snd_ztr_state == SND_ZTR_UNDERRUN &&
			   snd_ztr_published == snd_ztr_order) {
			/* The due order is resident: resume. */
			snd_ztr_consumed = snd_ztr_published;
			snd_ztr_restore();
			snd_output_flush();
			snd_ztr_state = SND_ZTR_PLAYING;
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
