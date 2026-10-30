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
 * Open checks the magic, the fixed layout and the capacities, then
 * reads the resident sections through the borrowed reader in one
 * forward pass.  When the header stamps a nonzero content CRC-16
 * (docs/ZTR-Format.md), the reads feed it and open verifies it before
 * the remap; structural validation is ztrcheck's, not the player's.
 * What the tick needs stays resident: the header fields, the divisor
 * table (split into a lo page and the following hi page), the order
 * list, the remapped instrument directory and the macro data.  Pattern
 * streams are read in bounded chunks, never whole.
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
#define ZTR_DIR_BYTES         SND_ZTR_DIR_BYTES
#define ZTR_MAX_FILE_BYTES    65535u

/* The pattern directory windows and the string-pool feed: whole
 * windows through the borrowed reader (the order buffers double as
 * the scratch), so the loops make no C call per byte. */
#define ZTR_WINDOW_BYTES      1024u

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

/* The largest-order scan's transient pointers and counter, static: the
 * loop bodies are all indexed access, which an ix frame taxes. */
static const uint8_t *scan_entry;
static const uint8_t *scan_selection;
static uint8_t *scan_total_at;
static uint8_t *scan_work;
static uint16_t scan_i;

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

		if (amount > ZTR_WINDOW_BYTES) amount = ZTR_WINDOW_BYTES;
		result = ztr_read((uint8_t *)dst + done, offset + done, amount);
		if (result < 0) return result;
		done = (uint16_t)(done + amount);
	}
	return (int)length;
}

/* The open pass reads whole windows into an order buffer (free until
 * the first refill), so the loops make no C call per byte. */
static uint8_t *ztr_order_buffer(uint8_t slot)
{
#ifdef HOST_ZTR
	return snd_ztr_order_area + (uint16_t)slot * SND_ZTR_ORDER_BYTES;
#else
	return (uint8_t *)(uint16_t)snd_ztr_order_base[slot];
#endif
}

/* --- the content CRC ---------------------------------------------------- */

/* CRC-16/CCITT-FALSE (poly 1021h, init FFFFh) over the song's fixed
 * sections: the header less the CRC field itself, the strings, the
 * orders, the instrument directory, the macro area, the pattern
 * directory and the note table (docs/ZTR-Format.md).  The pattern
 * streams are not covered.  Table-driven, libsnd-private; the same
 * semantics as the KonaCommander lane CRC. */
/* Split hi/lo byte tables: the step is all 8-bit (uint8_t is a
 * genuine byte under the ABI on the card and the host alike), so
 * neither width needs a mask. */
static const uint8_t ztr_crc_hi[256] = {
	0x00u, 0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u, 0x70u, 0x81u, 0x91u, 0xA1u, 0xB1u, 0xC1u, 0xD1u, 0xE1u, 0xF1u,
	0x12u, 0x02u, 0x32u, 0x22u, 0x52u, 0x42u, 0x72u, 0x62u, 0x93u, 0x83u, 0xB3u, 0xA3u, 0xD3u, 0xC3u, 0xF3u, 0xE3u,
	0x24u, 0x34u, 0x04u, 0x14u, 0x64u, 0x74u, 0x44u, 0x54u, 0xA5u, 0xB5u, 0x85u, 0x95u, 0xE5u, 0xF5u, 0xC5u, 0xD5u,
	0x36u, 0x26u, 0x16u, 0x06u, 0x76u, 0x66u, 0x56u, 0x46u, 0xB7u, 0xA7u, 0x97u, 0x87u, 0xF7u, 0xE7u, 0xD7u, 0xC7u,
	0x48u, 0x58u, 0x68u, 0x78u, 0x08u, 0x18u, 0x28u, 0x38u, 0xC9u, 0xD9u, 0xE9u, 0xF9u, 0x89u, 0x99u, 0xA9u, 0xB9u,
	0x5Au, 0x4Au, 0x7Au, 0x6Au, 0x1Au, 0x0Au, 0x3Au, 0x2Au, 0xDBu, 0xCBu, 0xFBu, 0xEBu, 0x9Bu, 0x8Bu, 0xBBu, 0xABu,
	0x6Cu, 0x7Cu, 0x4Cu, 0x5Cu, 0x2Cu, 0x3Cu, 0x0Cu, 0x1Cu, 0xEDu, 0xFDu, 0xCDu, 0xDDu, 0xADu, 0xBDu, 0x8Du, 0x9Du,
	0x7Eu, 0x6Eu, 0x5Eu, 0x4Eu, 0x3Eu, 0x2Eu, 0x1Eu, 0x0Eu, 0xFFu, 0xEFu, 0xDFu, 0xCFu, 0xBFu, 0xAFu, 0x9Fu, 0x8Fu,
	0x91u, 0x81u, 0xB1u, 0xA1u, 0xD1u, 0xC1u, 0xF1u, 0xE1u, 0x10u, 0x00u, 0x30u, 0x20u, 0x50u, 0x40u, 0x70u, 0x60u,
	0x83u, 0x93u, 0xA3u, 0xB3u, 0xC3u, 0xD3u, 0xE3u, 0xF3u, 0x02u, 0x12u, 0x22u, 0x32u, 0x42u, 0x52u, 0x62u, 0x72u,
	0xB5u, 0xA5u, 0x95u, 0x85u, 0xF5u, 0xE5u, 0xD5u, 0xC5u, 0x34u, 0x24u, 0x14u, 0x04u, 0x74u, 0x64u, 0x54u, 0x44u,
	0xA7u, 0xB7u, 0x87u, 0x97u, 0xE7u, 0xF7u, 0xC7u, 0xD7u, 0x26u, 0x36u, 0x06u, 0x16u, 0x66u, 0x76u, 0x46u, 0x56u,
	0xD9u, 0xC9u, 0xF9u, 0xE9u, 0x99u, 0x89u, 0xB9u, 0xA9u, 0x58u, 0x48u, 0x78u, 0x68u, 0x18u, 0x08u, 0x38u, 0x28u,
	0xCBu, 0xDBu, 0xEBu, 0xFBu, 0x8Bu, 0x9Bu, 0xABu, 0xBBu, 0x4Au, 0x5Au, 0x6Au, 0x7Au, 0x0Au, 0x1Au, 0x2Au, 0x3Au,
	0xFDu, 0xEDu, 0xDDu, 0xCDu, 0xBDu, 0xADu, 0x9Du, 0x8Du, 0x7Cu, 0x6Cu, 0x5Cu, 0x4Cu, 0x3Cu, 0x2Cu, 0x1Cu, 0x0Cu,
	0xEFu, 0xFFu, 0xCFu, 0xDFu, 0xAFu, 0xBFu, 0x8Fu, 0x9Fu, 0x6Eu, 0x7Eu, 0x4Eu, 0x5Eu, 0x2Eu, 0x3Eu, 0x0Eu, 0x1Eu,
};
static const uint8_t ztr_crc_lo[256] = {
	0x00u, 0x21u, 0x42u, 0x63u, 0x84u, 0xA5u, 0xC6u, 0xE7u, 0x08u, 0x29u, 0x4Au, 0x6Bu, 0x8Cu, 0xADu, 0xCEu, 0xEFu,
	0x31u, 0x10u, 0x73u, 0x52u, 0xB5u, 0x94u, 0xF7u, 0xD6u, 0x39u, 0x18u, 0x7Bu, 0x5Au, 0xBDu, 0x9Cu, 0xFFu, 0xDEu,
	0x62u, 0x43u, 0x20u, 0x01u, 0xE6u, 0xC7u, 0xA4u, 0x85u, 0x6Au, 0x4Bu, 0x28u, 0x09u, 0xEEu, 0xCFu, 0xACu, 0x8Du,
	0x53u, 0x72u, 0x11u, 0x30u, 0xD7u, 0xF6u, 0x95u, 0xB4u, 0x5Bu, 0x7Au, 0x19u, 0x38u, 0xDFu, 0xFEu, 0x9Du, 0xBCu,
	0xC4u, 0xE5u, 0x86u, 0xA7u, 0x40u, 0x61u, 0x02u, 0x23u, 0xCCu, 0xEDu, 0x8Eu, 0xAFu, 0x48u, 0x69u, 0x0Au, 0x2Bu,
	0xF5u, 0xD4u, 0xB7u, 0x96u, 0x71u, 0x50u, 0x33u, 0x12u, 0xFDu, 0xDCu, 0xBFu, 0x9Eu, 0x79u, 0x58u, 0x3Bu, 0x1Au,
	0xA6u, 0x87u, 0xE4u, 0xC5u, 0x22u, 0x03u, 0x60u, 0x41u, 0xAEu, 0x8Fu, 0xECu, 0xCDu, 0x2Au, 0x0Bu, 0x68u, 0x49u,
	0x97u, 0xB6u, 0xD5u, 0xF4u, 0x13u, 0x32u, 0x51u, 0x70u, 0x9Fu, 0xBEu, 0xDDu, 0xFCu, 0x1Bu, 0x3Au, 0x59u, 0x78u,
	0x88u, 0xA9u, 0xCAu, 0xEBu, 0x0Cu, 0x2Du, 0x4Eu, 0x6Fu, 0x80u, 0xA1u, 0xC2u, 0xE3u, 0x04u, 0x25u, 0x46u, 0x67u,
	0xB9u, 0x98u, 0xFBu, 0xDAu, 0x3Du, 0x1Cu, 0x7Fu, 0x5Eu, 0xB1u, 0x90u, 0xF3u, 0xD2u, 0x35u, 0x14u, 0x77u, 0x56u,
	0xEAu, 0xCBu, 0xA8u, 0x89u, 0x6Eu, 0x4Fu, 0x2Cu, 0x0Du, 0xE2u, 0xC3u, 0xA0u, 0x81u, 0x66u, 0x47u, 0x24u, 0x05u,
	0xDBu, 0xFAu, 0x99u, 0xB8u, 0x5Fu, 0x7Eu, 0x1Du, 0x3Cu, 0xD3u, 0xF2u, 0x91u, 0xB0u, 0x57u, 0x76u, 0x15u, 0x34u,
	0x4Cu, 0x6Du, 0x0Eu, 0x2Fu, 0xC8u, 0xE9u, 0x8Au, 0xABu, 0x44u, 0x65u, 0x06u, 0x27u, 0xC0u, 0xE1u, 0x82u, 0xA3u,
	0x7Du, 0x5Cu, 0x3Fu, 0x1Eu, 0xF9u, 0xD8u, 0xBBu, 0x9Au, 0x75u, 0x54u, 0x37u, 0x16u, 0xF1u, 0xD0u, 0xB3u, 0x92u,
	0x2Eu, 0x0Fu, 0x6Cu, 0x4Du, 0xAAu, 0x8Bu, 0xE8u, 0xC9u, 0x26u, 0x07u, 0x64u, 0x45u, 0xA2u, 0x83u, 0xE0u, 0xC1u,
	0x1Fu, 0x3Eu, 0x5Du, 0x7Cu, 0x9Bu, 0xBAu, 0xD9u, 0xF8u, 0x17u, 0x36u, 0x55u, 0x74u, 0x93u, 0xB2u, 0xD1u, 0xF0u,
};

/* The stored field (0 when the file is unprotected: nothing is fed or
 * verified) and the running value. */
static uint16_t ztr_crc_stored;
static uint16_t ztr_crc;

static void ztr_crc_feed(const uint8_t *data, uint16_t length)
{
	uint8_t hi;
	uint8_t lo;

	if (ztr_crc_stored == 0u) {
		return;
	}
	hi = (uint8_t)(ztr_crc >> 8);
	lo = (uint8_t)ztr_crc;
	while (length-- != 0u) {
		uint8_t index = (uint8_t)(hi ^ *data++);

		hi = (uint8_t)(lo ^ ztr_crc_hi[index]);
		lo = ztr_crc_lo[index];
	}
	ztr_crc = (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
}

/* A forward read that also feeds the content CRC. */
static int ztr_read_crc(void *dst, uint32_t offset, uint16_t length)
{
	int result = ztr_read_forward(dst, offset, length);

	if (result >= 0) {
		ztr_crc_feed((const uint8_t *)dst, length);
	}
	return result;
}

/* --- resident loading --------------------------------------------------- */

static uint16_t u16_at(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Load the resident sections through the borrowed reader in one forward
 * pass, feeding the content CRC: the strings (read only to feed a
 * stamped CRC - the player keeps no strings), the orders, the
 * instrument directory (scattered into its page by instrument number),
 * the macro area, the pattern directory (compacted resident: the
 * file's count field is the checker's, dropped here) and the note
 * table (whole into the scratch; split after the CRC check). */
static int load_sections(void)
{
	uint32_t order_offset = u16_at(header + 34);
	uint32_t instrument_offset = u16_at(header + 36);
	uint32_t macro_offset = u16_at(header + 38);
	uint16_t i;
	int result;

	string_end = (uint32_t)ZTR_HEADER_BYTES + u16_at(header + 48);
	pattern_dir_offset = u16_at(header + 40);
	pattern_data_offset = u16_at(header + 42);
	note_offset = u16_at(header + 44);
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
	if (ztr_crc_stored != 0u) {
		uint32_t at = ZTR_HEADER_BYTES;

		while (at < string_end) {
			uint16_t amount = (uint16_t)(string_end - at);

			if (amount > ZTR_WINDOW_BYTES) {
				amount = ZTR_WINDOW_BYTES;
			}
			result = ztr_read_crc(ztr_order_buffer(0u), at, amount);
			if (result < 0) {
				return result;
			}
			at += amount;
		}
	}
	result = ztr_read_crc(snd_ztr_orders, order_offset,
			      (uint16_t)(snd_ztr_order_count * ZTR_CHANNELS));
	if (result < 0) {
		return result;
	}
	memset(snd_ztr_instr, 0, 256u);
	/* The instrument table is one contiguous read: the loop must not
	 * pay one reader call per instrument. */
	result = ztr_read_crc(ztr_order_buffer(1u), instrument_offset,
			      (uint16_t)((uint16_t)instrument_count *
					 ZTR_INSTRUMENT_BYTES));
	if (result < 0) {
		return result;
	}
	for (i = 0u; i < instrument_count; ++i) {
		const uint8_t *entry = ztr_order_buffer(1u) +
			i * ZTR_INSTRUMENT_BYTES;

		if (entry[0] >= ZTR_CHANNELS) {
			return -KOS_EINVAL;
		}
		memcpy(snd_ztr_instr + (uint16_t)entry[0] *
		       ZTR_INSTRUMENT_BYTES, entry, ZTR_INSTRUMENT_BYTES);
	}
	macro_used = (uint32_t)(pattern_dir_offset - macro_offset) + 1u;
	snd_ztr_macro_area[0] = 0u;
	if (macro_used > 1u) {
		result = ztr_read_crc(snd_ztr_macro_area + 1u, macro_offset,
				      (uint16_t)(macro_used - 1u));
		if (result < 0) {
			return result;
		}
	}
	{
		uint8_t *resident = snd_ztr_patterns;
		uint16_t done = 0u;

		while (done < pattern_count) {
			uint16_t window = pattern_count - done;
			uint16_t j;

			if (window > ZTR_WINDOW_BYTES / ZTR_PATTERN_BYTES) {
				window = ZTR_WINDOW_BYTES /
					ZTR_PATTERN_BYTES;
			}
			result = ztr_read_crc(ztr_order_buffer(1u),
					      pattern_dir_offset +
					      (uint32_t)done *
					      ZTR_PATTERN_BYTES,
					      (uint16_t)(window *
							 ZTR_PATTERN_BYTES));
			if (result < 0) {
				return result;
			}
			for (j = 0u; j < window; ++j) {
				const uint8_t *entry = ztr_order_buffer(1u) +
					j * ZTR_PATTERN_BYTES;

				/* Six byte copies inline: a memcpy call per
				 * entry costs more than the bytes. */
				resident[0] = entry[0];
				resident[1] = entry[1];
				resident[2] = entry[2];
				resident[3] = entry[3];
				resident[4] = entry[4];
				resident[5] = entry[5];
				resident += ZTR_DIR_BYTES;
			}
			done = (uint16_t)(done + window);
		}
	}
	return ztr_read_crc(ztr_order_buffer(1u), note_offset,
			    (uint16_t)(ZTR_NOTE_COUNT << 1));
}

/* After the last covered read: verify a stamped CRC, then remap the
 * resident instrument entries' file macro offsets to the one-based
 * resident area and split the note table into its lo/hi pages. */
static int remap_and_split(void)
{
	uint16_t macro_offset = u16_at(header + 38);
	uint16_t i;
	uint16_t note;

	if (ztr_crc_stored != 0u && ztr_crc != ztr_crc_stored) {
		return -KOS_EINVAL;
	}
	for (i = 0u; i < ZTR_CHANNELS; ++i) {
		uint8_t *entry = snd_ztr_instr + i * ZTR_INSTRUMENT_BYTES;
		uint8_t slot;

		for (slot = 0u; slot < SND_ZTR_MACRO_SLOTS; ++slot) {
			uint16_t offset = u16_at(entry + 4u + slot * 2u);
			uint16_t resident = 0u;

			if (offset != 0u) {
				resident = (uint16_t)(1u + offset -
						      macro_offset);
			}
			entry[4u + slot * 2u] = (uint8_t)resident;
			entry[5u + slot * 2u] = (uint8_t)(resident >> 8);
		}
	}
	{
		const uint8_t *table = ztr_order_buffer(1u);
		uint8_t *lo_page = snd_ztr_div_lo;
		uint8_t *hi_page = lo_page + 256u;

		for (note = 0u; note < ZTR_NOTE_COUNT; ++note) {
			lo_page[note] = table[note << 1];
			hi_page[note] = table[(note << 1) + 1u];
		}
	}
	return 0;
}

static int load_resident(void)
{
	/* The loudness page: base<<4|macro -> base*macro/15.  The
	 * remainder walks base per step (base < 16, so one conditional
	 * subtraction): the library links no 8-bit divide. */
	{
		uint8_t *loud = snd_ztr_loud;
		uint8_t base;

		for (base = 0u; base < 16u; ++base) {
			uint16_t remainder = 0u;
			uint8_t value = 0u;
			uint8_t macro;

			for (macro = 0u; macro < 16u; ++macro) {
				*loud++ = value;
				remainder = (uint16_t)(remainder + base);
				if (remainder >= 15u) {
					remainder -= 15u;
					++value;
				}
			}
		}
	}
	/* The reserved mask: every channel used by any order.  Channel
	 * outer so the bit walks a register and the first use ends the
	 * channel's scan. */
	snd_ztr_mask = 0u;
	{
		const uint8_t *sel = snd_ztr_orders;
		uint16_t bit = 1u;
		uint8_t channel;

		for (channel = 0u; channel < ZTR_CHANNELS;
		     ++channel, ++sel, bit = (uint16_t)(bit << 1)) {
			const uint8_t *scan = sel;
			uint16_t i;

			for (i = 0u; i < snd_ztr_order_count;
			     ++i, scan += ZTR_CHANNELS) {
				if (*scan != 0xFFu) {
					snd_ztr_mask |= bit;
					break;
				}
			}
		}
	}
	return 0;
}

/* The largest order's stream total, from the resident directory.  A
 * per-channel scan of the whole directory would multiply the entry
 * count by sixteen, so the entries are bucketed by channel first - one
 * count pass and one scatter pass over the resident copy, the index
 * pairs and cursors in order-buffer slot 1 (48 B of tables, then two
 * bytes per entry).  Per channel the bucket fills pattern-number ->
 * stream-length byte tables (lo page at +0, hi page at +256, so no
 * 16-bit index arithmetic) in reverse, so the first directory match
 * wins as setup_order_for resolves; then every order's sixteen
 * selections total from them, the totals kept as byte pairs (lo at
 * +512, hi past the lo run) so the accumulation is 8-bit throughout.
 * The tables live in order-buffer slot 0, free until the first refill.
 * FFh/FFh marks an unset slot: a stream length is bounded by the
 * 64 KiB file less its offset. */
static uint16_t largest_order_total(void)
{
	uint8_t *bucket = ztr_order_buffer(1u);
	uint16_t largest = 0u;
	uint16_t order;
	uint8_t channel;

	memset(bucket, 0, ZTR_CHANNELS);
	scan_entry = snd_ztr_patterns;
	for (scan_i = 0u; scan_i < pattern_count;
	     ++scan_i, scan_entry += ZTR_DIR_BYTES) {
		if (scan_entry[0] < ZTR_CHANNELS) {
			++bucket[scan_entry[0]];
		}
	}
	{
		uint16_t at = (uint16_t)(16u + (ZTR_CHANNELS << 1));
		uint8_t c;

		for (c = 0u; c < ZTR_CHANNELS; ++c) {
			bucket[ZTR_CHANNELS + (c << 1)] = (uint8_t)at;
			bucket[ZTR_CHANNELS + (c << 1) + 1u] =
				(uint8_t)(at >> 8);
			at = (uint16_t)(at + ((uint16_t)bucket[c] << 1));
		}
	}
	scan_entry = snd_ztr_patterns;
	for (scan_i = 0u; scan_i < pattern_count;
	     ++scan_i, scan_entry += ZTR_DIR_BYTES) {
		if (scan_entry[0] < ZTR_CHANNELS) {
			uint8_t *cursor = bucket + ZTR_CHANNELS +
				((uint16_t)scan_entry[0] << 1);
			uint16_t at = u16_at(cursor);
			uint16_t next = (uint16_t)(at + 2u);

			bucket[at] = (uint8_t)scan_i;
			bucket[at + 1u] = (uint8_t)(scan_i >> 8);
			cursor[0] = (uint8_t)next;
			cursor[1] = (uint8_t)(next >> 8);
		}
	}
	scan_work = ztr_order_buffer(0u);
	memset(scan_work + 512u, 0, SND_ZTR_MAX_ORDERS << 1);
	{
		uint16_t bit = 1u;

		for (channel = 0u; channel < ZTR_CHANNELS;
		     ++channel, bit = (uint16_t)(bit << 1)) {
			uint16_t end;
			uint16_t at;

			/* A channel no order selects contributes nothing. */
			if ((snd_ztr_mask & bit) == 0u) {
				continue;
			}
			end = u16_at(bucket + ZTR_CHANNELS +
				     ((uint16_t)channel << 1));
			at = (uint16_t)(end -
				((uint16_t)bucket[channel] << 1));

			memset(scan_work, 0xFF, 512u);
			while (end != at) {
				uint16_t index;

				end = (uint16_t)(end - 2u);
				index = (uint16_t)((uint16_t)bucket[end] |
					((uint16_t)bucket[end + 1u] << 8));
				scan_entry = snd_ztr_patterns +
					(uint16_t)((index << 2) + (index << 1));
				scan_work[scan_entry[1]] = scan_entry[4];
				scan_work[256u + scan_entry[1]] = scan_entry[5];
			}
			scan_selection = snd_ztr_orders + channel;
			scan_total_at = scan_work + 512u;
			for (order = 0u; order < snd_ztr_order_count;
			     ++order, scan_selection += ZTR_CHANNELS,
			     ++scan_total_at) {
				uint8_t sel = *scan_selection;
				uint8_t len_lo;
				uint8_t len_hi;

				if (sel == 0xFFu) {
					continue;
				}
				len_lo = scan_work[sel];
				len_hi = scan_work[256u + sel];
				if (len_lo != 0xFFu || len_hi != 0xFFu) {
					uint8_t old = *scan_total_at;
					uint8_t sum = (uint8_t)(old + len_lo);

					*scan_total_at = sum;
					scan_total_at[SND_ZTR_MAX_ORDERS] =
						(uint8_t)(
						scan_total_at[SND_ZTR_MAX_ORDERS] +
						len_hi + (sum < old ? 1u : 0u));
				}
			}
		}
	}
	for (order = 0u; order < snd_ztr_order_count; ++order) {
		uint16_t total = (uint16_t)(
			(uint16_t)scan_work[512u + order] |
			((uint16_t)scan_work[512u + SND_ZTR_MAX_ORDERS +
					    order] << 8));

		if (total > largest) {
			largest = total;
		}
	}
	return largest;
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
	 * accept exactly that representation of the logical size. */
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
	/* The magic and the fixed layout the loader's arithmetic
	 * assumes; the content itself is the stamped CRC's job. */
	if (header[0] != 'Z' || header[1] != 'T' || header[2] != 'R' ||
	    header[3] != '1' || header[4] != 1u ||
	    header[5] != ZTR_HEADER_BYTES || header[7] != ZTR_CHANNELS ||
	    u16_at(header + 14) == 0u ||
	    u16_at(header + 18) > ZTR_CHANNELS ||
	    header[50] != ZTR_CHANNELS ||
	    header[51] != ZTR_INSTRUMENT_BYTES ||
	    header[52] != ZTR_PATTERN_BYTES || header[53] != ZTR_NOTE_COUNT) {
		result = -KOS_EINVAL;
		goto fail;
	}
	ztr_crc_stored = u16_at(header + 22);
	ztr_crc = 0xFFFFu;
	ztr_crc_feed(header, 22u);
	ztr_crc_feed(header + 24u, ZTR_HEADER_BYTES - 24u);
	result = load_sections();
	if (result < 0) {
		goto fail;
	}
	result = remap_and_split();
	if (result < 0) {
		goto fail;
	}
	result = load_resident();
	if (result < 0) {
		goto fail;
	}
	if ((snd_reserved_voices & snd_ztr_mask) != 0u) {
		result = -KOS_EBUSY;
		goto fail;
	}
	/* A larger order than the order buffer is -KOS_ENOSPC. */
	if (largest_order_total() > SND_ZTR_ORDER_STREAM) {
		result = -KOS_ENOSPC;
		goto fail;
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

int snd_ztr_service(void)
{
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

int snd_ztr_tables(struct snd_ztr_tables *out)
{
	if (out == (struct snd_ztr_tables *)0 ||
	    snd_ztr_state == SND_ZTR_EMPTY || snd_ztr_state == SND_ZTR_ERROR) {
		return -KOS_EINVAL;
	}
	out->orders = snd_ztr_orders;
	out->patterns = snd_ztr_patterns;
	out->order_count = snd_ztr_order_count;
	out->pattern_count = pattern_count;
	return 0;
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
