#include "sntracker.h"
#include "kos/stream.h"

/* The current KonaOS storage path is optimized for one full IPC descriptor.
 * fread uses its direct path for an item of this size, avoiding the stream's
 * 512-byte staging buffer during the one-time whole-song load. */
#define ZTR_READ_CHUNK 4096u

unsigned char ztr_buffer[ZTR_MAX_FILE_BYTES];
unsigned int ztr_size;
struct ztr_song ztr_song;
const char *ztr_error;

unsigned int ztr_u16(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

int ztr_i16(const unsigned char *p)
{
	unsigned int value = ztr_u16(p);
	if (value == 0x8000u) return -32767 - 1;
	if ((value & 0x8000u) != 0u)
		return -(int)(((value ^ 0xFFFFu) + 1u) & 0xFFFFu);
	return (int)value;
}

static uint32_t ztr_u32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int fail(const char *message)
{
	ztr_error = message;
	return -KOS_EINVAL;
}

static unsigned char string_ok(unsigned int offset,
			       unsigned int pool_end)
{
	if (offset < ZTR_HEADER_BYTES || offset >= pool_end) {
		return 0u;
	}
	while (offset < pool_end) {
		if (ztr_buffer[offset++] == 0u) {
			return 1u;
		}
	}
	return 0u;
}

static unsigned char macro_ok(unsigned int offset, unsigned char type,
			      unsigned int macro_start,
			      unsigned int macro_end)
{
	const unsigned char *m;
	unsigned int count;
	unsigned int loop;
	unsigned int release;
	uint32_t end;
	unsigned int i;

	if (offset < macro_start || offset >= macro_end ||
	    (uint32_t)offset + 12ul > (uint32_t)macro_end) {
		return 0u;
	}
	m = ztr_buffer + offset;
	count = ztr_u16(m + 10);
	loop = ztr_u16(m + 6);
	release = ztr_u16(m + 8);
	end = (uint32_t)offset + 12ul + (uint32_t)count +
	      ((uint32_t)count << 1);
	if (m[0] != type || m[1] > 2u || (m[2] & 0xFEu) != 0u ||
	    m[5] != 0u || count == 0u || end > (uint32_t)macro_end ||
	    (loop != 0xFFFFu && loop >= count) ||
	    (release != 0xFFFFu && release >= count)) {
		return 0u;
	}
	for (i = 0u; i < count; ++i) {
		if ((m[12u + i * 3u] & 0xFEu) != 0u) {
			return 0u;
		}
	}
	return 1u;
}

static unsigned char pattern_ok(const unsigned char *entry,
				unsigned int data_start,
				unsigned int data_end)
{
	unsigned int offset = ztr_u16(entry + 2);
	unsigned int length = ztr_u16(entry + 4);
	unsigned int expected = ztr_u16(entry + 6);
	unsigned int position = offset;
	unsigned int end = (unsigned int)(offset + length);
	unsigned int events = 0u;
	unsigned int last_row = 0u;
	unsigned char first = 1u;

	if (entry[0] >= ZTR_CHANNELS || offset < data_start ||
	    (uint32_t)offset + length > data_end) {
		return 0u;
	}
	while (position < end) {
		unsigned char row;
		unsigned char mask;
		unsigned char modifiers;

		if ((unsigned int)(end - position) < 2u) return 0u;
		row = ztr_buffer[position++];
		mask = ztr_buffer[position++];
		if ((unsigned int)row >= ztr_song.pattern_length ||
		    (first == 0u && (unsigned int)row <= last_row) || mask == 0u) {
			return 0u;
		}
		first = 0u;
		last_row = row;
		if ((mask & (EVENT_OFF | EVENT_RELEASE)) ==
		    (EVENT_OFF | EVENT_RELEASE)) return 0u;
		if ((mask & EVENT_NOTE) != 0u) {
			if (position >= end || ztr_buffer[position++] >= ZTR_NOTE_COUNT)
				return 0u;
		}
		if ((mask & EVENT_INSTRUMENT) != 0u) {
			if (position >= end || ztr_buffer[position++] >= ZTR_CHANNELS)
				return 0u;
		}
		if ((mask & EVENT_VOLUME) != 0u) {
			if (position >= end || ztr_buffer[position++] > 15u) return 0u;
		}
		if ((mask & EVENT_EFFECT) != 0u) {
			if ((unsigned int)(end - position) < 2u ||
			    ztr_buffer[position] == 0u ||
			    ztr_buffer[position] > EFFECT_STOP) return 0u;
			position = (unsigned int)(position + 2u);
		}
		if ((mask & EVENT_LEGATO) != 0u &&
		    (mask & EVENT_NOTE) == 0u) return 0u;
		if ((mask & EVENT_NOTE) != 0u &&
		    (mask & (EVENT_OFF | EVENT_RELEASE)) != 0u) return 0u;
		if ((mask & EVENT_MODIFIERS) != 0u) {
			if (position >= end) return 0u;
			modifiers = ztr_buffer[position++];
			if ((modifiers & 0x07u) != 0u ||
			    (modifiers & (MOD_ARP_SET | MOD_ARP_CLEAR)) ==
			    (MOD_ARP_SET | MOD_ARP_CLEAR) ||
			    (modifiers & (MOD_HAIRPIN_SET | MOD_HAIRPIN_CLEAR)) ==
			    (MOD_HAIRPIN_SET | MOD_HAIRPIN_CLEAR)) return 0u;
			if ((modifiers & MOD_ARP_SET) != 0u) {
				if (position >= end) return 0u;
				++position;
			}
			if ((modifiers & MOD_HAIRPIN_SET) != 0u) {
				signed char rate;
				if ((unsigned int)(end - position) < 2u) return 0u;
				rate = (signed char)ztr_buffer[position];
				if (rate == 0 || rate < -15 || rate > 15 ||
				    ztr_buffer[position + 1u] > 15u) return 0u;
				position = (unsigned int)(position + 2u);
			}
		}
		++events;
	}
	return position == end && events == expected;
}

static int validate(void)
{
	const unsigned char *h = ztr_buffer;
	unsigned int string_size;
	unsigned int string_end;
	unsigned int order_offset;
	unsigned int instrument_offset;
	unsigned int macro_offset;
	unsigned int pattern_dir_offset;
	unsigned int pattern_data_offset;
	unsigned int note_offset;
	unsigned int i;
	unsigned int seen_instruments = 0u;

	if (ztr_size < ZTR_HEADER_BYTES || h[0] != 'Z' || h[1] != 'T' ||
	    h[2] != 'R' || h[3] != '1' || h[4] != 1u ||
	    h[5] != ZTR_HEADER_BYTES || h[6] != 0u || h[7] != ZTR_CHANNELS) {
		return fail("bad ZTR1 header");
	}
	for (i = 55u; i < 64u; ++i) if (h[i] != 0u) return fail("reserved header byte");
	if (ztr_u16(h + 8) == 0u || ztr_u16(h + 8) > 180u || h[9] != 0u ||
	    h[10] == 0u || h[11] == 0u || h[11] > ZTR_CHANNELS ||
	    ztr_u16(h + 12) == 0u || ztr_u16(h + 12) > 256u ||
	    ztr_u16(h + 14) == 0u || ztr_u16(h + 14) > 255u ||
	    ztr_u16(h + 18) > ZTR_CHANNELS || h[22] != 0u || h[23] != 0u ||
	    ztr_u32(h + 24) != 3579545ul || ztr_u16(h + 46) != ztr_size ||
	    h[50] != ZTR_CHANNELS || h[51] != ZTR_INSTRUMENT_BYTES ||
	    h[52] != ZTR_PATTERN_BYTES || h[53] != ZTR_NOTE_COUNT) {
		return fail("unsupported ZTR values");
	}
	string_size = ztr_u16(h + 48);
	string_end = (unsigned int)(ZTR_HEADER_BYTES + string_size);
	order_offset = ztr_u16(h + 34);
	instrument_offset = ztr_u16(h + 36);
	macro_offset = ztr_u16(h + 38);
	pattern_dir_offset = ztr_u16(h + 40);
	pattern_data_offset = ztr_u16(h + 42);
	note_offset = ztr_u16(h + 44);
	if ((uint32_t)ZTR_HEADER_BYTES + string_size > ztr_size ||
	    order_offset != string_end ||
	    (uint32_t)order_offset + ((uint32_t)ztr_u16(h + 14) << 4) != instrument_offset ||
	    (uint32_t)instrument_offset + ((uint32_t)ztr_u16(h + 18) << 4) != macro_offset ||
	    macro_offset > pattern_dir_offset ||
	    (uint32_t)pattern_dir_offset + ((uint32_t)ztr_u16(h + 16) << 3) != pattern_data_offset ||
	    pattern_data_offset > note_offset ||
	    (uint32_t)note_offset + ZTR_NOTE_COUNT * 2ul != ztr_size) {
		return fail("overlapping or truncated sections");
	}
	if (!string_ok(ztr_u16(h + 28), string_end) ||
	    !string_ok(ztr_u16(h + 30), string_end) ||
	    !string_ok(ztr_u16(h + 32), string_end)) {
		return fail("invalid song string");
	}
	ztr_song.tick_rate = h[8];
	ztr_song.speed = h[10];
	ztr_song.pattern_length = ztr_u16(h + 12);
	ztr_song.order_count = h[14];
	ztr_song.pattern_count = ztr_u16(h + 16);
	ztr_song.instrument_count = h[18];
	ztr_song.title = (const char *)(ztr_buffer + ztr_u16(h + 28));
	ztr_song.author = (const char *)(ztr_buffer + ztr_u16(h + 30));
	ztr_song.orders = ztr_buffer + order_offset;
	ztr_song.instruments = ztr_buffer + instrument_offset;
	ztr_song.pattern_directory = ztr_buffer + pattern_dir_offset;
	ztr_song.note_table = ztr_buffer + note_offset;

	for (i = 0u; i < ztr_song.instrument_count; ++i) {
		const unsigned char *entry = ztr_song.instruments + i * 16u;
		unsigned char number = entry[0];
		unsigned char slot;
		if (number >= ZTR_CHANNELS || (seen_instruments & (1u << number)) != 0u ||
		    entry[14] != 0u || entry[15] != 0u ||
		    !string_ok(ztr_u16(entry + 2), string_end))
			return fail("invalid instrument directory");
		seen_instruments |= (unsigned int)(1u << number);
		for (slot = 0u; slot < ZTR_MACRO_SLOTS; ++slot) {
			unsigned int offset = ztr_u16(entry + 4u + slot * 2u);
			if (offset != 0u && !macro_ok(offset, (unsigned char)(slot + 1u),
						 macro_offset, pattern_dir_offset))
				return fail("invalid instrument macro");
		}
	}
	for (i = 0u; i < ztr_song.pattern_count; ++i) {
		if (!pattern_ok(ztr_song.pattern_directory + i * 8u,
				pattern_data_offset, note_offset))
			return fail("invalid pattern stream");
	}
	for (i = 0u; i < ZTR_NOTE_COUNT; ++i) {
		unsigned int divisor = ztr_u16(ztr_song.note_table + i * 2u);
		if (divisor == 0u || divisor > 1023u) return fail("invalid note table");
	}
	return 0;
}

int ztr_load(const char *path)
{
	struct kfs_stat info;
	FILE *stream;
	unsigned int physical_size;
	unsigned int done = 0u;
	int result;

	ztr_error = "file read failed";
	result = stat(path, &info);
	if (result < 0 || info.type != KFS_TYPE_FILE ||
	    info.size < ZTR_HEADER_BYTES || info.size > ZTR_MAX_FILE_BYTES) {
		return -KOS_EINVAL;
	}
	physical_size = (unsigned int)info.size;
	ztr_size = physical_size;
	stream = fopen(path, "rb");
	if (stream == (FILE *)0) return -KOS_EIO;
	while (done < physical_size) {
		unsigned int amount = (unsigned int)(physical_size - done);
		if (amount > ZTR_READ_CHUNK) amount = ZTR_READ_CHUNK;
		if (fread(ztr_buffer + done, amount, 1u, stream) != 1u) break;
		done = (unsigned int)(done + amount);
	}
	result = fclose(stream);
	if (done != physical_size || result < 0) return -KOS_EIO;

	/* A file copied through CP/M commonly occupies complete 128-byte
	 * records.  ZTR retains its exact logical size in the header, and the
	 * unused tail of the last record is filled with CP/M EOF bytes (1Ah).
	 * Accept exactly that representation, but do not silently ignore other
	 * appended data or more than one partial record. */
	if (physical_size >= ZTR_HEADER_BYTES) {
		unsigned int logical_size = ztr_u16(ztr_buffer + 46);
		if (logical_size >= ZTR_HEADER_BYTES && logical_size < physical_size) {
			unsigned int rounded =
				(unsigned int)((logical_size + 127u) & 0xFF80u);
			unsigned int position;
			if (physical_size != rounded) {
				return fail("data follows logical ZTR file");
			}
			for (position = logical_size; position < physical_size; ++position) {
				if (ztr_buffer[position] != 0x1Au)
					return fail("invalid CP/M record padding");
			}
			ztr_size = logical_size;
		}
	}
	return validate();
}
