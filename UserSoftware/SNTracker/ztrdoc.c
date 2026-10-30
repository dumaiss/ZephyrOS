/* SN-Track's read-only ZTR document (docs/sntrack-ui.md section 11).
 *
 * Open keeps the header values, the orders, the pattern directory and the
 * instruments resident, verifies the stamped content CRC, and validates
 * every sparse stream through one in-memory decoder.  Streams are read
 * again per order for display.
 */

#include "kos.h"
#include "kos/errno.h"
#include "ztrdoc.h"

_Static_assert(sizeof(struct ztrdoc_pattern) == 8u,
	       "the pattern directory is read straight into memory");

static uint16_t word(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t dword(const uint8_t *p)
{
	return (uint32_t)word(p) | ((uint32_t)word(p + 2u) << 16);
}

static int read_full(const struct ztrdoc *doc, uint32_t at, void *dst,
		     uint16_t count)
{
	int n;
	if (at > doc->reader->size || count > doc->reader->size - at)
		return -KOS_EINVAL;
	n = kos_reader_read_full(doc->reader, at, dst, count);
	return n < 0 ? n : 0;
}

static int read_string(const struct ztrdoc *doc, uint16_t at,
		       char *dst, uint8_t capacity)
{
	uint16_t end = (uint16_t)(doc->strings_offset + doc->strings_size);
	uint8_t c, used = 0u;
	if (at < doc->strings_offset || at >= end) return -KOS_EINVAL;
	for (;;) {
		if (at >= end || read_full(doc, at++, &c, 1u) < 0)
			return -KOS_EINVAL;
		if (c == 0u) break;
		if (used + 1u < capacity) dst[used++] = (char)c;
	}
	dst[used] = '\0';
	return 0;
}

static int macro_first(const struct ztrdoc *doc, uint16_t at, uint8_t type,
		       int16_t *value, uint8_t *flags, uint8_t *delay)
{
	uint8_t h[15];
	uint16_t count;
	*value = 0; *flags = 0u; *delay = 0u;
	if (at == 0u) return 0;
	if (at < doc->macros_offset || at > doc->patterns_offset - 15u ||
	    read_full(doc, at, h, 15u) < 0)
		return -KOS_EINVAL;
	count = word(h + 10u);
	if (h[0] != type || h[1] > 2u || h[5] != 0u || count == 0u ||
	    count > (doc->patterns_offset - at - 12u) / 3u)
		return -KOS_EINVAL;
	*flags = h[12];
	*value = (int16_t)word(h + 13u);
	*delay = h[4];
	return 0;
}

/* One event from buffer[*at..end): the mask rules and the modifier flags of
 * docs/ZTR-Format.md are checked; every field is kept. */
static int next_event(const uint8_t *buffer, uint16_t *at, uint16_t end,
		      uint8_t *row, struct ztrdoc_event *event)
{
	uint16_t p = *at;
	uint8_t mask, flags;
#define GET(dst) do { if (p >= end) return -KOS_EINVAL; (dst) = buffer[p++]; } while (0)
	memset(event, 0, sizeof(*event));
	GET(*row); GET(mask);
	event->mask = mask;
	if (mask == 0u || ((mask & 0x80u) != 0u && (mask & 0x0Cu) != 0u) ||
	    (mask & 0x0Cu) == 0x0Cu ||
	    ((mask & 0x02u) != 0u && (mask & 0x80u) == 0u))
		return -KOS_EINVAL;
	if ((mask & 0x80u) != 0u) GET(event->note);
	if ((mask & 0x40u) != 0u) GET(event->instrument);
	if ((mask & 0x20u) != 0u) GET(event->volume);
	if ((mask & 0x10u) != 0u) { GET(event->effect); GET(event->parameter); }
	if ((mask & 0x01u) != 0u) {
		GET(flags);
		event->modifier_flags = flags;
		if ((flags & 0x07u) != 0u || (flags & 0x60u) == 0x60u ||
		    (flags & 0x18u) == 0x18u) return -KOS_EINVAL;
		if ((flags & 0x40u) != 0u) GET(event->modifier_xy);
		if ((flags & 0x10u) != 0u) {
			GET(event->hairpin_rate); GET(event->hairpin_ceiling);
		}
	}
#undef GET
	if (((mask & 0x80u) != 0u && event->note > 95u) ||
	    ((mask & 0x20u) != 0u && event->volume > 15u))
		return -KOS_EINVAL;
	*at = p;
	return 0;
}

static uint16_t pattern_key(const struct ztrdoc_pattern *pattern)
{
	return (uint16_t)(((uint16_t)pattern->channel << 8) | pattern->number);
}

/* The directory sorted by (channel, number), once at open: an insertion
 * sort, linear on the tools' already sorted directories.  A repeated key
 * is malformed. */
static int sort_patterns(struct ztrdoc *doc)
{
	uint16_t i, j, key;
	struct ztrdoc_pattern moving;
	for (i = 1u; i < doc->pattern_count; ++i) {
		moving = doc->patterns[i];
		key = pattern_key(&moving);
		for (j = i; j != 0u && pattern_key(&doc->patterns[j - 1u]) > key; --j)
			doc->patterns[j] = doc->patterns[j - 1u];
		doc->patterns[j] = moving;
		if (j != 0u && pattern_key(&doc->patterns[j - 1u]) == key)
			return -KOS_EINVAL;
	}
	return 0;
}

/* The sorted directory's first entry per channel; channel_start[16] is
 * the count. */
static void index_channels(struct ztrdoc *doc)
{
	uint16_t i = 0u;
	uint8_t channel;
	for (channel = 0u; channel <= ZTRDOC_CHANNELS; ++channel) {
		while (i < doc->pattern_count && doc->patterns[i].channel < channel)
			++i;
		doc->channel_start[channel] = i;
	}
}

/* Patterns are usually numbered densely from 0, so the entry is first
 * looked for at start + number, then searched for within the channel. */
int ztrdoc_find_pattern(const struct ztrdoc *doc, uint8_t channel,
			uint8_t number, struct ztrdoc_pattern *out)
{
	uint16_t low = doc->channel_start[channel];
	uint16_t high = doc->channel_start[channel + 1u];
	uint16_t middle = (uint16_t)(low + number);
	uint8_t at;
	if (middle >= high || doc->patterns[middle].number != number) {
		while (low < high) {
			middle = (uint16_t)((low + high) >> 1);
			at = doc->patterns[middle].number;
			if (at == number) break;
			if (at < number) low = (uint16_t)(middle + 1u);
			else high = middle;
		}
		if (low >= high) return -KOS_ENOENT;
	}
	*out = doc->patterns[middle];
	return 0;
}

static void put_word(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

int ztrdoc_load_order(const struct ztrdoc *doc, uint16_t order,
		      uint8_t *buffer, uint16_t size)
{
	uint8_t channel;
	uint16_t used = ZTRDOC_ORDER_TABLE;
	if (order >= doc->order_count || size < ZTRDOC_ORDER_TABLE)
		return -KOS_EINVAL;
	memset(buffer, 0, ZTRDOC_ORDER_TABLE);
	for (channel = 0u; channel < ZTRDOC_CHANNELS; ++channel) {
		struct ztrdoc_pattern pattern;
		uint8_t number = doc->orders[order][channel];
		if (number == 0xFFu) continue;
		if (ztrdoc_find_pattern(doc, channel, number, &pattern) < 0)
			return -KOS_EINVAL;
		if (pattern.length > size - used) return -KOS_ENOSPC;
		put_word(buffer + channel * 4u, used);
		put_word(buffer + channel * 4u + 2u,
			 (uint16_t)(used + pattern.length));
		if (read_full(doc, pattern.offset, buffer + used,
			      pattern.length) < 0) return -KOS_EIO;
		used = (uint16_t)(used + pattern.length);
	}
	return (int)used;
}

int ztrdoc_decode_channel(const uint8_t *buffer, uint8_t channel,
			  uint16_t first_row,
			  struct ztrdoc_event out[ZTRDOC_VIEW_ROWS])
{
	uint16_t at = word(buffer + channel * 4u);
	uint16_t end = word(buffer + channel * 4u + 2u);
	struct ztrdoc_event event;
	uint8_t row;
	memset(out, 0, sizeof(*out) * ZTRDOC_VIEW_ROWS);
	while (at < end) {
		if (next_event(buffer, &at, end, &row, &event) < 0)
			return -KOS_EINVAL;
		if (row >= first_row && row < first_row + ZTRDOC_VIEW_ROWS)
			out[row - first_row] = event;
	}
	return 0;
}

const struct ztrdoc_instrument *ztrdoc_instrument(
	const struct ztrdoc *doc, uint8_t number)
{
	int8_t index;
	if (number >= ZTRDOC_MAX_INSTRUMENTS) return 0;
	index = doc->instrument_index[number];
	return index < 0 ? 0 : &doc->instruments[(uint8_t)index];
}

/* N = (white << 2) | rate, as the engine sets it at the note's onset
 * (render_noise, effective_note); not shown without a note and an
 * instrument, or when a macro's first value waits behind a delay. */
uint8_t ztrdoc_noise_control(const struct ztrdoc *doc,
			     const struct ztrdoc_event *event,
			     uint8_t *shown)
{
	const struct ztrdoc_instrument *instrument;
	int16_t note;
	*shown = 0u;
	if ((event->mask & 0xC0u) != 0xC0u) return 0u;
	instrument = ztrdoc_instrument(doc, event->instrument);
	if (instrument == 0 ||
	    (instrument->arpeggio_offset != 0u && instrument->arpeggio_delay != 0u) ||
	    (instrument->noise_offset != 0u && instrument->noise_delay != 0u))
		return 0u;
	note = event->note;
	if (instrument->arpeggio_offset != 0u) {
		if ((instrument->arpeggio_flags & 1u) != 0u)
			note = instrument->arpeggio_first;
		else note = (int16_t)(note + instrument->arpeggio_first);
	}
	if (note < 0) note = 0;
	if (note > 95) note = 95;
	*shown = 1u;
	return (uint8_t)(((instrument->noise_first & 1) << 2) |
			 ((uint16_t)note & 3u));
}

static int check_header(struct ztrdoc *doc, const uint8_t *h)
{
	uint8_t i;
	uint16_t expected;
	if (memcmp(h, "ZTR1", 4u) != 0 || h[4] != 1u || h[5] != 0x40u ||
	    h[6] != 0u || h[7] != 16u || h[10] == 0u || h[11] == 0u ||
	    h[11] > 16u || word(h + 12u) == 0u || word(h + 12u) > 256u ||
	    word(h + 14u) == 0u || word(h + 14u) > ZTRDOC_MAX_ORDERS ||
	    word(h + 16u) > ZTRDOC_MAX_PATTERNS || word(h + 18u) > 16u ||
	    h[50] != 16u || h[51] != 16u || h[52] != 8u || h[53] != 96u)
		return -KOS_EINVAL;
	for (i = 0x37u; i < 0x40u; ++i) if (h[i] != 0u) return -KOS_EINVAL;
	doc->rate = word(h + 8u); doc->speed = h[10]; doc->rows = word(h + 12u);
	doc->order_count = word(h + 14u); doc->pattern_count = word(h + 16u);
	doc->instrument_count = (uint8_t)word(h + 18u);
	doc->clock_hz = dword(h + 24u); doc->strings_offset = 64u;
	doc->strings_size = word(h + 48u); doc->orders_offset = word(h + 34u);
	doc->instruments_offset = word(h + 36u); doc->macros_offset = word(h + 38u);
	doc->patterns_offset = word(h + 40u); doc->pattern_data_offset = word(h + 42u);
	doc->note_table_offset = word(h + 44u);
	if (doc->rate == 0u || doc->rate > 180u ||
	    word(h + 46u) != doc->reader->size || doc->strings_size == 0u)
		return -KOS_EINVAL;
	/* The sections in order, without gaps where their sizes are known. */
	expected = (uint16_t)(64u + doc->strings_size);
	if (doc->orders_offset != expected) return -KOS_EINVAL;
	expected = (uint16_t)(expected + doc->order_count * ZTRDOC_CHANNELS);
	if (doc->instruments_offset != expected) return -KOS_EINVAL;
	expected = (uint16_t)(expected + doc->instrument_count * 16u);
	if (doc->macros_offset != expected || doc->patterns_offset < expected)
		return -KOS_EINVAL;
	expected = (uint16_t)(doc->patterns_offset + doc->pattern_count * 8u);
	if (doc->pattern_data_offset != expected ||
	    doc->note_table_offset < expected ||
	    (uint32_t)doc->note_table_offset + 192u != doc->reader->size)
		return -KOS_EINVAL;
	return 0;
}

/* The content CRC-16 of docs/ZTR-Format.md (16h), table for
 * poly 1021h init FFFFh, the same values libsnd feeds. */
static const uint16_t crc_table[256] = {
	0x0000u, 0x1021u, 0x2042u, 0x3063u, 0x4084u, 0x50A5u, 0x60C6u, 0x70E7u,
	0x8108u, 0x9129u, 0xA14Au, 0xB16Bu, 0xC18Cu, 0xD1ADu, 0xE1CEu, 0xF1EFu,
	0x1231u, 0x0210u, 0x3273u, 0x2252u, 0x52B5u, 0x4294u, 0x72F7u, 0x62D6u,
	0x9339u, 0x8318u, 0xB37Bu, 0xA35Au, 0xD3BDu, 0xC39Cu, 0xF3FFu, 0xE3DEu,
	0x2462u, 0x3443u, 0x0420u, 0x1401u, 0x64E6u, 0x74C7u, 0x44A4u, 0x5485u,
	0xA56Au, 0xB54Bu, 0x8528u, 0x9509u, 0xE5EEu, 0xF5CFu, 0xC5ACu, 0xD58Du,
	0x3653u, 0x2672u, 0x1611u, 0x0630u, 0x76D7u, 0x66F6u, 0x5695u, 0x46B4u,
	0xB75Bu, 0xA77Au, 0x9719u, 0x8738u, 0xF7DFu, 0xE7FEu, 0xD79Du, 0xC7BCu,
	0x48C4u, 0x58E5u, 0x6886u, 0x78A7u, 0x0840u, 0x1861u, 0x2802u, 0x3823u,
	0xC9CCu, 0xD9EDu, 0xE98Eu, 0xF9AFu, 0x8948u, 0x9969u, 0xA90Au, 0xB92Bu,
	0x5AF5u, 0x4AD4u, 0x7AB7u, 0x6A96u, 0x1A71u, 0x0A50u, 0x3A33u, 0x2A12u,
	0xDBFDu, 0xCBDCu, 0xFBBFu, 0xEB9Eu, 0x9B79u, 0x8B58u, 0xBB3Bu, 0xAB1Au,
	0x6CA6u, 0x7C87u, 0x4CE4u, 0x5CC5u, 0x2C22u, 0x3C03u, 0x0C60u, 0x1C41u,
	0xEDAEu, 0xFD8Fu, 0xCDECu, 0xDDCDu, 0xAD2Au, 0xBD0Bu, 0x8D68u, 0x9D49u,
	0x7E97u, 0x6EB6u, 0x5ED5u, 0x4EF4u, 0x3E13u, 0x2E32u, 0x1E51u, 0x0E70u,
	0xFF9Fu, 0xEFBEu, 0xDFDDu, 0xCFFCu, 0xBF1Bu, 0xAF3Au, 0x9F59u, 0x8F78u,
	0x9188u, 0x81A9u, 0xB1CAu, 0xA1EBu, 0xD10Cu, 0xC12Du, 0xF14Eu, 0xE16Fu,
	0x1080u, 0x00A1u, 0x30C2u, 0x20E3u, 0x5004u, 0x4025u, 0x7046u, 0x6067u,
	0x83B9u, 0x9398u, 0xA3FBu, 0xB3DAu, 0xC33Du, 0xD31Cu, 0xE37Fu, 0xF35Eu,
	0x02B1u, 0x1290u, 0x22F3u, 0x32D2u, 0x4235u, 0x5214u, 0x6277u, 0x7256u,
	0xB5EAu, 0xA5CBu, 0x95A8u, 0x8589u, 0xF56Eu, 0xE54Fu, 0xD52Cu, 0xC50Du,
	0x34E2u, 0x24C3u, 0x14A0u, 0x0481u, 0x7466u, 0x6447u, 0x5424u, 0x4405u,
	0xA7DBu, 0xB7FAu, 0x8799u, 0x97B8u, 0xE75Fu, 0xF77Eu, 0xC71Du, 0xD73Cu,
	0x26D3u, 0x36F2u, 0x0691u, 0x16B0u, 0x6657u, 0x7676u, 0x4615u, 0x5634u,
	0xD94Cu, 0xC96Du, 0xF90Eu, 0xE92Fu, 0x99C8u, 0x89E9u, 0xB98Au, 0xA9ABu,
	0x5844u, 0x4865u, 0x7806u, 0x6827u, 0x18C0u, 0x08E1u, 0x3882u, 0x28A3u,
	0xCB7Du, 0xDB5Cu, 0xEB3Fu, 0xFB1Eu, 0x8BF9u, 0x9BD8u, 0xABBBu, 0xBB9Au,
	0x4A75u, 0x5A54u, 0x6A37u, 0x7A16u, 0x0AF1u, 0x1AD0u, 0x2AB3u, 0x3A92u,
	0xFD2Eu, 0xED0Fu, 0xDD6Cu, 0xCD4Du, 0xBDAAu, 0xAD8Bu, 0x9DE8u, 0x8DC9u,
	0x7C26u, 0x6C07u, 0x5C64u, 0x4C45u, 0x3CA2u, 0x2C83u, 0x1CE0u, 0x0CC1u,
	0xEF1Fu, 0xFF3Eu, 0xCF5Du, 0xDF7Cu, 0xAF9Bu, 0xBFBAu, 0x8FD9u, 0x9FF8u,
	0x6E17u, 0x7E36u, 0x4E55u, 0x5E74u, 0x2E93u, 0x3EB2u, 0x0ED1u, 0x1EF0u,
};

static uint16_t crc_feed(uint16_t crc, const uint8_t *data, uint16_t length)
{
	while (length-- != 0u)
		crc = (uint16_t)((crc << 8) ^
			crc_table[(uint8_t)(crc >> 8) ^ *data++]);
	return crc;
}

/* One covered range, through the scratch window. */
static int crc_sweep(const struct ztrdoc *doc, uint16_t *crc, uint16_t at,
		     uint16_t end, uint8_t *scratch, uint16_t size)
{
	while (at < end) {
		uint16_t chunk = (uint16_t)(end - at);
		if (chunk > size) chunk = size;
		if (read_full(doc, at, scratch, chunk) < 0) return -KOS_EIO;
		*crc = crc_feed(*crc, scratch, chunk);
		at = (uint16_t)(at + chunk);
	}
	return 0;
}

/* The stored CRC, when nonzero, over [0,16h) + [18h,pattern data) +
 * [note table,+192): a mismatch means the file is corrupt. */
static int check_crc(struct ztrdoc *doc, const uint8_t *h,
		     uint8_t *scratch, uint16_t size)
{
	uint16_t crc;
	if (word(h + 0x16u) == 0u) return 0;
	if (size == 0u) return -KOS_ENOSPC;
	crc = crc_feed(0xFFFFu, h, 0x16u);
	crc = crc_feed(crc, h + 0x18u, (uint16_t)(64u - 0x18u));
	if (crc_sweep(doc, &crc, 64u, doc->pattern_data_offset,
		      scratch, size) < 0 ||
	    crc_sweep(doc, &crc, doc->note_table_offset,
		      (uint16_t)(doc->note_table_offset + 192u),
		      scratch, size) < 0)
		return -KOS_EIO;
	return crc == word(h + 0x16u) ? 0 : -KOS_EINVAL;
}

static int load_instruments(struct ztrdoc *doc)
{
	uint8_t b[16], i;
	for (i = 0u; i < doc->instrument_count; ++i) {
		struct ztrdoc_instrument *ins = &doc->instruments[i];
		if (read_full(doc, (uint16_t)(doc->instruments_offset + i * 16u),
			      b, 16u) < 0 || b[0] >= 16u ||
		    doc->instrument_index[b[0]] >= 0 || b[14] != 0u || b[15] != 0u)
			return -KOS_EINVAL;
		ins->number = b[0];
		ins->type = ZTRDOC_INSTR_UNUSED;
		ins->arpeggio_offset = word(b + 8u);
		ins->noise_offset = word(b + 10u);
		doc->instrument_index[b[0]] = (int8_t)i;
		if (read_string(doc, word(b + 2u), ins->name, sizeof(ins->name)) < 0 ||
		    macro_first(doc, ins->arpeggio_offset, 3u, &ins->arpeggio_first,
				&ins->arpeggio_flags, &ins->arpeggio_delay) < 0 ||
		    macro_first(doc, ins->noise_offset, 4u, &ins->noise_first,
				&b[0], &ins->noise_delay) < 0)
			return -KOS_EINVAL;
	}
	return 0;
}

/* Every stream: its events in row order, inside the pattern, as many as
 * the directory says; each explicit instrument classifies its entry. */
static int check_streams(struct ztrdoc *doc, uint8_t *scratch, uint16_t size)
{
	uint16_t i, at, count, window_at = 0u, window_end = 0u;
	uint8_t row, previous, noise;
	const uint8_t *stream;
	struct ztrdoc_event event;
	struct ztrdoc_instrument *ins;
	for (i = 0u; i < doc->pattern_count; ++i) {
		const struct ztrdoc_pattern *pattern = &doc->patterns[i];
		if (pattern->channel >= 16u ||
		    pattern->offset < doc->pattern_data_offset ||
		    pattern->length > doc->note_table_offset - pattern->offset)
			return -KOS_EINVAL;
		if (pattern->length > size) return -KOS_ENOSPC;
		/* A window of the pattern data: streams stored in sequence cost
		 * one read per buffer, not one per stream. */
		if (pattern->offset < window_at ||
		    pattern->offset + pattern->length > window_end) {
			window_at = pattern->offset;
			window_end = doc->note_table_offset - window_at > size ?
				(uint16_t)(window_at + size) : doc->note_table_offset;
			if (read_full(doc, window_at, scratch,
				      (uint16_t)(window_end - window_at)) < 0)
				return -KOS_EIO;
		}
		stream = scratch + (pattern->offset - window_at);
		noise = (uint8_t)((pattern->channel & 3u) == 3u ?
				  ZTRDOC_INSTR_NOISE : ZTRDOC_INSTR_TONE);
		at = 0u; count = 0u; previous = 0u;
		while (at < pattern->length) {
			if (next_event(stream, &at, pattern->length, &row, &event) < 0 ||
			    row >= doc->rows || (count != 0u && row <= previous))
				return -KOS_EINVAL;
			previous = row;
			++count;
			if ((event.mask & 0x40u) == 0u) continue;
			ins = (struct ztrdoc_instrument *)ztrdoc_instrument(
				doc, event.instrument);
			if (ins == 0) return -KOS_EINVAL;
			if (ins->type == ZTRDOC_INSTR_UNUSED) ins->type = noise;
			else if (ins->type != noise) return -KOS_ENOTSUP;
		}
		if (count != pattern->events) return -KOS_EINVAL;
	}
	return 0;
}

/* Orders name stored patterns, and each order's streams fit one buffer. */
static int check_orders(const struct ztrdoc *doc)
{
	uint16_t order, used;
	uint8_t channel;
	struct ztrdoc_pattern pattern;
	for (order = 0u; order < doc->order_count; ++order) {
		used = ZTRDOC_ORDER_TABLE;
		for (channel = 0u; channel < ZTRDOC_CHANNELS; ++channel) {
			uint8_t number = doc->orders[order][channel];
			if (number == 0xFFu) continue;
			if (ztrdoc_find_pattern(doc, channel, number, &pattern) < 0)
				return -KOS_EINVAL;
			if (pattern.length > ZTRDOC_ORDER_BUFFER - used)
				return -KOS_ENOSPC;
			used = (uint16_t)(used + pattern.length);
		}
	}
	return 0;
}

int ztrdoc_open(struct ztrdoc *doc, const kos_reader_t *reader,
		uint8_t *scratch, uint16_t size)
{
	uint8_t h[64];
	char discard[2];
	uint8_t i;
	int result;
	memset(doc, 0, sizeof(*doc));
	doc->reader = reader;
	for (i = 0u; i < ZTRDOC_MAX_INSTRUMENTS; ++i) doc->instrument_index[i] = -1;
	if (reader->size < 256u || reader->size > 65535u ||
	    read_full(doc, 0u, h, 64u) < 0) return -KOS_EINVAL;
	result = check_header(doc, h);
	if (result < 0) return result;
	result = check_crc(doc, h, scratch, size);
	if (result < 0) return result;
	if (read_full(doc, doc->orders_offset, doc->orders,
		      (uint16_t)(doc->order_count * ZTRDOC_CHANNELS)) < 0 ||
	    read_full(doc, doc->patterns_offset, doc->patterns,
		      (uint16_t)(doc->pattern_count * 8u)) < 0)
		return -KOS_EIO;
	result = sort_patterns(doc);
	if (result < 0) return result;
	index_channels(doc);
	if (read_string(doc, word(h + 28u), doc->title, sizeof(doc->title)) < 0 ||
	    read_string(doc, word(h + 30u), discard, sizeof(discard)) < 0 ||
	    read_string(doc, word(h + 32u), discard, sizeof(discard)) < 0)
		return -KOS_EINVAL;
	result = load_instruments(doc);
	if (result < 0) return result;
	result = check_streams(doc, scratch, size);
	if (result < 0) return result;
	return check_orders(doc);
}
