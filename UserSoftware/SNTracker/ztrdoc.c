/* SN-Track's read-only ZTR document (docs/sntrack-ui.md section 11).
 *
 * The song is libsnd's: its open checked the layout, the capacities and
 * the content CRC, and its resident orders and pattern directory are
 * the document's.  Open adds the header's display values and the
 * instruments, and validates every sparse stream through one in-memory
 * decoder while typing the instruments.  Streams are read again per
 * order for display.
 */

#include "kos.h"
#include "kos/errno.h"
#include "ztrdoc.h"

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

/* Directory entry `i` (libsnd's 6-byte resident form). */
static void pattern_at(const struct ztrdoc *doc, uint16_t i,
		       struct ztrdoc_pattern *out)
{
	const uint8_t *e = doc->patterns + i * SND_ZTR_DIR_BYTES;
	out->channel = e[0]; out->number = e[1];
	out->offset = word(e + 2u); out->length = word(e + 4u);
}

static void put_word(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

int ztrdoc_load_order(const struct ztrdoc *doc, uint16_t order,
		      uint8_t *buffer, uint16_t size)
{
	const uint8_t *numbers;
	struct ztrdoc_pattern pattern;
	uint16_t used = ZTRDOC_ORDER_TABLE, i;
	if (order >= doc->order_count || size < ZTRDOC_ORDER_TABLE)
		return -KOS_EINVAL;
	numbers = ZTRDOC_ORDER(doc, order);
	memset(buffer, 0, ZTRDOC_ORDER_TABLE);
	/* One directory pass: each entry the order selects, at most once
	 * per channel (the first match, as the engine takes it). */
	for (i = 0u; i < doc->pattern_count; ++i) {
		uint8_t *slot;
		pattern_at(doc, i, &pattern);
		slot = buffer + pattern.channel * 4u;
		if (pattern.channel >= ZTRDOC_CHANNELS ||
		    numbers[pattern.channel] != pattern.number ||
		    word(slot + 2u) != 0u)
			continue;
		if (pattern.length > size - used) return -KOS_ENOSPC;
		if (read_full(doc, pattern.offset, buffer + used,
			      pattern.length) < 0) return -KOS_EIO;
		put_word(slot, used);
		used = (uint16_t)(used + pattern.length);
		put_word(slot + 2u, used);
	}
	return (int)used;
}

/* An event starts with its row byte, so a cursor peeks the next row
 * without decoding. */
int ztrdoc_seek_rows(const uint8_t *buffer, uint16_t first_row,
		     uint16_t at[ZTRDOC_CHANNELS])
{
	struct ztrdoc_event event;
	uint8_t channel, row;
	for (channel = 0u; channel < ZTRDOC_CHANNELS; ++channel) {
		uint16_t end = word(buffer + channel * 4u + 2u);
		at[channel] = word(buffer + channel * 4u);
		while (at[channel] < end && buffer[at[channel]] < first_row)
			if (next_event(buffer, &at[channel], end, &row, &event) < 0)
				return -KOS_EINVAL;
	}
	return 0;
}

int ztrdoc_next_row(const uint8_t *buffer, uint16_t row,
		    uint16_t at[ZTRDOC_CHANNELS],
		    struct ztrdoc_event out[ZTRDOC_CHANNELS])
{
	uint8_t channel, event_row;
	memset(out, 0, sizeof(*out) * ZTRDOC_CHANNELS);
	for (channel = 0u; channel < ZTRDOC_CHANNELS; ++channel) {
		uint16_t end = word(buffer + channel * 4u + 2u);
		if (at[channel] < end && buffer[at[channel]] == row &&
		    next_event(buffer, &at[channel], end, &event_row,
			       &out[channel]) < 0)
			return -KOS_EINVAL;
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

/* The header's display values and section offsets; libsnd checked the
 * fixed layout and the capacities. */
static int header_values(struct ztrdoc *doc, const uint8_t *h)
{
	doc->rate = word(h + 8u); doc->speed = h[10]; doc->rows = word(h + 12u);
	doc->instrument_count = (uint8_t)word(h + 18u);
	doc->clock_hz = dword(h + 24u); doc->strings_offset = 64u;
	doc->strings_size = word(h + 48u); doc->orders_offset = word(h + 34u);
	doc->instruments_offset = word(h + 36u); doc->macros_offset = word(h + 38u);
	doc->patterns_offset = word(h + 40u); doc->pattern_data_offset = word(h + 42u);
	doc->note_table_offset = word(h + 44u);
	if (doc->rate == 0u || doc->speed == 0u || doc->rows == 0u ||
	    doc->rows > 256u || doc->instrument_count > ZTRDOC_MAX_INSTRUMENTS ||
	    doc->strings_size == 0u)
		return -KOS_EINVAL;
	return 0;
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
	uint16_t i, at, window_at = 0u, window_end = 0u;
	uint8_t row, previous, noise, first;
	const uint8_t *stream;
	struct ztrdoc_event event;
	struct ztrdoc_instrument *ins;
	for (i = 0u; i < doc->pattern_count; ++i) {
		struct ztrdoc_pattern entry;
		const struct ztrdoc_pattern *pattern = &entry;
		pattern_at(doc, i, &entry);
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
		at = 0u; first = 1u; previous = 0u;
		while (at < pattern->length) {
			if (next_event(stream, &at, pattern->length, &row, &event) < 0 ||
			    row >= doc->rows || (first == 0u && row <= previous))
				return -KOS_EINVAL;
			previous = row;
			first = 0u;
			if ((event.mask & 0x40u) == 0u) continue;
			ins = (struct ztrdoc_instrument *)ztrdoc_instrument(
				doc, event.instrument);
			if (ins == 0) return -KOS_EINVAL;
			if (ins->type == ZTRDOC_INSTR_UNUSED) ins->type = noise;
			else if (ins->type != noise) return -KOS_ENOTSUP;
		}
	}
	return 0;
}

int ztrdoc_open(struct ztrdoc *doc, const kos_reader_t *reader,
		const struct snd_ztr_tables *tables,
		uint8_t *scratch, uint16_t size)
{
	uint8_t h[64];
	uint8_t i;
	int result;
	memset(doc, 0, sizeof(*doc));
	doc->reader = reader;
	doc->orders = tables->orders;
	doc->patterns = tables->patterns;
	doc->order_count = tables->order_count;
	doc->pattern_count = tables->pattern_count;
	for (i = 0u; i < ZTRDOC_MAX_INSTRUMENTS; ++i) doc->instrument_index[i] = -1;
	if (read_full(doc, 0u, h, 64u) < 0) return -KOS_EINVAL;
	result = header_values(doc, h);
	if (result < 0) return result;
	if (read_string(doc, word(h + 28u), doc->title, sizeof(doc->title)) < 0)
		return -KOS_EINVAL;
	result = load_instruments(doc);
	if (result < 0) return result;
	return check_streams(doc, scratch, size);
}
