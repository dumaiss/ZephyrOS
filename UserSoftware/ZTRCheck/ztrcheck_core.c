/* ztrcheck_core.c - the ZTR structural checks, each reporting a
 * diagnostic and a file offset.  Reader-based: every section comes in
 * through kos_reader_t reads, windowed through the caller's scratch, so
 * the same source runs on the Z80 and on the host twin. */

#include "ztrcheck_core.h"

#define ZC_HEADER_BYTES   64u
#define ZC_CHANNELS       16u
#define ZC_NOTES          96u
#define ZC_NOTE_BYTES     (ZC_NOTES * 2u)
#define ZC_INSTR_BYTES    16u
#define ZC_PAT_BYTES      8u
#define ZC_MACRO_HEADER   12u
#define ZC_MACRO_SLOTS    5u
#define ZC_MAX_ORDERS     136u
#define ZC_MAX_PATTERNS   400u
#define ZC_MACRO_BYTES    6640u
#define ZC_CLOCK_HZ       3579545ul

/* The event mask bits, modifier bits and effect range. */
#define ZC_EVENT_NOTE       0x80u
#define ZC_EVENT_INSTRUMENT 0x40u
#define ZC_EVENT_VOLUME     0x20u
#define ZC_EVENT_EFFECT     0x10u
#define ZC_EVENT_OFF        0x08u
#define ZC_EVENT_RELEASE    0x04u
#define ZC_EVENT_LEGATO     0x02u
#define ZC_EVENT_MODIFIERS  0x01u
#define ZC_MOD_ARP_SET      0x40u
#define ZC_MOD_ARP_CLEAR    0x20u
#define ZC_MOD_HAIRPIN_SET  0x10u
#define ZC_MOD_HAIRPIN_CLEAR 0x08u
#define ZC_EFFECT_STOP      6u

struct zc_song {
	uint16_t rate;
	uint8_t speed;
	uint16_t rows;
	uint16_t orders;
	uint16_t patterns;
	uint8_t instruments;
	uint16_t strings_end;
	uint16_t instr_off;
	uint16_t macro_off;
	uint16_t patdir_off;
	uint16_t patdata_off;
	uint16_t note_off;
	uint32_t logical;
};

static uint16_t zc_u16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t zc_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int zc_fail(struct ztrcheck_report *r, uint8_t check, uint16_t offset)
{
	r->check = check;
	r->offset = offset;
	return -1;
}

/* The read contract, retried: short reads are legal, an early end or a
 * negative return is not. */
static int zc_read(const kos_reader_t *reader, uint32_t at, void *dst,
		   uint16_t length)
{
	uint8_t *out = (uint8_t *)dst;
	while (length != 0u) {
		int n;
		if (at >= reader->size || length > reader->size - at)
			return -1;
		n = reader->read_at(reader->context, at, out, length);
		if (n <= 0) return -1;
		at += (uint32_t)n;
		out += n;
		length = (uint16_t)(length - (uint16_t)n);
	}
	return 0;
}

/* --- the content CRC (docs/ZTR-Format.md 16h) --------------------------- */

static const uint16_t zc_crc_table[256] = {
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

static uint16_t zc_crc_feed(uint16_t crc, const uint8_t *data,
			    uint16_t length)
{
	while (length-- != 0u)
		crc = (uint16_t)((crc << 8) ^
			zc_crc_table[(uint8_t)(crc >> 8) ^ *data++]);
	return crc;
}

static int zc_crc_sweep(const kos_reader_t *reader, uint16_t *crc,
			uint16_t at, uint16_t end, uint8_t *scratch,
			uint16_t size)
{
	while (at < end) {
		uint16_t chunk = (uint16_t)(end - at);
		if (chunk > size) chunk = size;
		if (zc_read(reader, at, scratch, chunk) < 0) return -1;
		*crc = zc_crc_feed(*crc, scratch, chunk);
		at = (uint16_t)(at + chunk);
	}
	return 0;
}

/* --- the checks, in file order ------------------------------------------- */

/* The fixed and ranged header fields; each failure names its field. */
static int zc_header(const kos_reader_t *reader, uint8_t *h,
		     struct zc_song *s, struct ztrcheck_report *r)
{
	uint8_t i;
	if (reader->size < ZC_HEADER_BYTES)
		return zc_fail(r, ZTRCHECK_HEADER, 0u);
	if (zc_read(reader, 0u, h, ZC_HEADER_BYTES) < 0)
		return zc_fail(r, ZTRCHECK_IO, 0u);
	if (h[0] != 'Z' || h[1] != 'T' || h[2] != 'R' || h[3] != '1')
		return zc_fail(r, ZTRCHECK_HEADER, 0u);
	if (h[4] != 1u) return zc_fail(r, ZTRCHECK_HEADER, 4u);
	if (h[5] != ZC_HEADER_BYTES) return zc_fail(r, ZTRCHECK_HEADER, 5u);
	if (h[6] != 0u) return zc_fail(r, ZTRCHECK_HEADER, 6u);
	if (h[7] != ZC_CHANNELS) return zc_fail(r, ZTRCHECK_HEADER, 7u);
	s->rate = zc_u16(h + 8u);
	if (s->rate == 0u || s->rate > 180u)
		return zc_fail(r, ZTRCHECK_HEADER, 8u);
	if (h[9] != 0u) return zc_fail(r, ZTRCHECK_HEADER, 9u);
	s->speed = h[10];
	if (s->speed == 0u) return zc_fail(r, ZTRCHECK_HEADER, 10u);
	if (h[11] == 0u || h[11] > ZC_CHANNELS)
		return zc_fail(r, ZTRCHECK_HEADER, 11u);
	s->rows = zc_u16(h + 12u);
	if (s->rows == 0u || s->rows > 256u)
		return zc_fail(r, ZTRCHECK_HEADER, 12u);
	s->orders = zc_u16(h + 14u);
	if (s->orders == 0u || s->orders > ZC_MAX_ORDERS)
		return zc_fail(r, ZTRCHECK_HEADER, 14u);
	s->patterns = zc_u16(h + 16u);
	if (s->patterns > ZC_MAX_PATTERNS)
		return zc_fail(r, ZTRCHECK_HEADER, 16u);
	if (zc_u16(h + 18u) > ZC_CHANNELS)
		return zc_fail(r, ZTRCHECK_HEADER, 18u);
	s->instruments = (uint8_t)zc_u16(h + 18u);
	if (zc_u32(h + 24u) != ZC_CLOCK_HZ)
		return zc_fail(r, ZTRCHECK_HEADER, 24u);
	if (h[50] != ZC_CHANNELS || h[51] != ZC_INSTR_BYTES ||
	    h[52] != ZC_PAT_BYTES || h[53] != ZC_NOTES)
		return zc_fail(r, ZTRCHECK_HEADER, 50u);
	for (i = 55u; i < 64u; ++i)
		if (h[i] != 0u) return zc_fail(r, ZTRCHECK_HEADER, i);
	s->logical = zc_u16(h + 46u);
	if (s->logical < ZC_HEADER_BYTES || s->logical > reader->size)
		return zc_fail(r, ZTRCHECK_HEADER, 46u);
	return 0;
}

/* The sections are contiguous and ordered; each failure names the offset
 * field that does not line up. */
static int zc_layout(const kos_reader_t *reader, const uint8_t *h,
		     struct zc_song *s, struct ztrcheck_report *r)
{
	uint32_t expected;
	(void)reader;
	s->strings_end = (uint16_t)(ZC_HEADER_BYTES + zc_u16(h + 48u));
	if ((uint32_t)s->strings_end > s->logical)
		return zc_fail(r, ZTRCHECK_LAYOUT, 48u);
	expected = s->strings_end;
	if (zc_u16(h + 34u) != expected)
		return zc_fail(r, ZTRCHECK_LAYOUT, 34u);
	expected += (uint32_t)s->orders << 4;
	if (zc_u16(h + 36u) != expected)
		return zc_fail(r, ZTRCHECK_LAYOUT, 36u);
	s->instr_off = zc_u16(h + 36u);
	expected += (uint32_t)s->instruments << 4;
	if (zc_u16(h + 38u) != expected)
		return zc_fail(r, ZTRCHECK_LAYOUT, 38u);
	s->macro_off = zc_u16(h + 38u);
	s->patdir_off = zc_u16(h + 40u);
	if (s->patdir_off < s->macro_off ||
	    (uint32_t)(s->patdir_off - s->macro_off) + 1u > ZC_MACRO_BYTES)
		return zc_fail(r, ZTRCHECK_LAYOUT, 40u);
	expected = (uint32_t)s->patdir_off +
		((uint32_t)s->patterns << 3);
	if (zc_u16(h + 42u) != expected)
		return zc_fail(r, ZTRCHECK_LAYOUT, 42u);
	s->patdata_off = zc_u16(h + 42u);
	s->note_off = zc_u16(h + 44u);
	if (s->note_off < s->patdata_off ||
	    (uint32_t)s->note_off + ZC_NOTE_BYTES != s->logical)
		return zc_fail(r, ZTRCHECK_LAYOUT, 44u);
	return 0;
}

/* A file copied through CP/M occupies complete 128-byte records whose
 * unused tail is 1Ah-filled; anything else past the logical size is a
 * defect. */
static int zc_padding(const kos_reader_t *reader, const struct zc_song *s,
		      uint8_t *scratch, uint16_t size,
		      struct ztrcheck_report *r)
{
	uint32_t at = s->logical;
	uint32_t rounded = (s->logical + 127u) & ~(uint32_t)127u;
	if (reader->size == s->logical) return 0;
	if (reader->size != rounded)
		return zc_fail(r, ZTRCHECK_PADDING, (uint16_t)at);
	while (at < reader->size) {
		uint32_t chunk = reader->size - at;
		uint16_t i, n;
		if (chunk > size) chunk = size;
		n = (uint16_t)chunk;
		if (zc_read(reader, at, scratch, n) < 0)
			return zc_fail(r, ZTRCHECK_IO, (uint16_t)at);
		for (i = 0u; i < n; ++i)
			if (scratch[i] != 0x1Au)
				return zc_fail(r, ZTRCHECK_PADDING,
					       (uint16_t)(at + i));
		at += n;
	}
	return 0;
}

/* The stored CRC, verified first when nonzero: [0,16h) +
 * [18h,pattern data) + [note table,+192). */
static int zc_crc(const kos_reader_t *reader, const uint8_t *h,
		  const struct zc_song *s, uint8_t *scratch, uint16_t size,
		  struct ztrcheck_report *r)
{
	uint16_t crc;
	r->crc_stored = zc_u16(h + 0x16u);
	if (r->crc_stored == 0u) return 0;
	crc = zc_crc_feed(0xFFFFu, h, 0x16u);
	crc = zc_crc_feed(crc, h + 0x18u,
			  (uint16_t)(ZC_HEADER_BYTES - 0x18u));
	if (zc_crc_sweep(reader, &crc, ZC_HEADER_BYTES, s->patdata_off,
			 scratch, size) < 0 ||
	    zc_crc_sweep(reader, &crc, s->note_off,
			 (uint16_t)(s->note_off + ZC_NOTE_BYTES),
			 scratch, size) < 0)
		return zc_fail(r, ZTRCHECK_IO, 0u);
	r->crc_computed = crc;
	if (crc != r->crc_stored)
		return zc_fail(r, ZTRCHECK_CRC, 0x16u);
	return 0;
}

/* One pool reference: inside the pool and NUL-terminated before its end.
 * Copies up to capacity-1 bytes when dst is not 0. */
static int zc_text(const kos_reader_t *reader, uint16_t ref, uint16_t end,
		   char *dst, uint8_t capacity)
{
	uint32_t at;
	uint8_t used = 0u;
	if (ref < ZC_HEADER_BYTES || ref >= end) return -1;
	for (at = ref; at < end; ++at) {
		uint8_t c;
		if (zc_read(reader, at, &c, 1u) < 0) return -1;
		if (c == 0u) {
			if (dst != (char *)0) dst[used] = '\0';
			return 0;
		}
		if (dst != (char *)0 && (uint16_t)used + 1u < capacity)
			dst[used++] = (char)c;
	}
	return -1;
}

static int zc_strings(const kos_reader_t *reader, const uint8_t *h,
		      const struct zc_song *s, struct ztrcheck_report *r)
{
	uint8_t c;
	if (zc_read(reader, (uint32_t)s->strings_end - 1u, &c, 1u) < 0)
		return zc_fail(r, ZTRCHECK_IO, (uint16_t)(s->strings_end - 1u));
	if (c != 0u)
		return zc_fail(r, ZTRCHECK_STRINGS,
			       (uint16_t)(s->strings_end - 1u));
	if (zc_text(reader, zc_u16(h + 28u), s->strings_end,
		    r->title, ZTRCHECK_TEXT) < 0)
		return zc_fail(r, ZTRCHECK_STRINGS, 28u);
	if (zc_text(reader, zc_u16(h + 30u), s->strings_end,
		    r->author, ZTRCHECK_TEXT) < 0)
		return zc_fail(r, ZTRCHECK_STRINGS, 30u);
	if (zc_text(reader, zc_u16(h + 32u), s->strings_end,
		    r->source, ZTRCHECK_TEXT) < 0)
		return zc_fail(r, ZTRCHECK_STRINGS, 32u);
	return 0;
}

/* One macro block, as libsnd's resident_macro_ok had it: the 12-byte
 * header, then count x 3 value bytes with their reserved bits clear. */
static int zc_macro(const kos_reader_t *reader, uint16_t at, uint8_t type,
		    const struct zc_song *s, uint8_t *scratch, uint16_t size,
		    struct ztrcheck_report *r)
{
	uint8_t m[ZC_MACRO_HEADER];
	uint16_t count, loop, release, i;
	uint32_t values, values_end;
	uint8_t phase = 0u;	/* only each entry's flags byte is checked */
	if (at < s->macro_off ||
	    (uint32_t)at + ZC_MACRO_HEADER > (uint32_t)s->patdir_off)
		return zc_fail(r, ZTRCHECK_MACRO, at);
	if (zc_read(reader, at, m, ZC_MACRO_HEADER) < 0)
		return zc_fail(r, ZTRCHECK_IO, at);
	count = zc_u16(m + 10u);
	loop = zc_u16(m + 6u);
	release = zc_u16(m + 8u);
	values = (uint32_t)at + ZC_MACRO_HEADER;
	values_end = values + (uint32_t)count + ((uint32_t)count << 1);
	if (m[0] != type || m[1] > 2u || (m[2] & 0xFEu) != 0u ||
	    m[5] != 0u || count == 0u || values_end > s->patdir_off ||
	    (loop != 0xFFFFu && loop >= count) ||
	    (release != 0xFFFFu && release >= count))
		return zc_fail(r, ZTRCHECK_MACRO, at);
	while (values < values_end) {
		uint32_t chunk = values_end - values;
		if (chunk > size) chunk = size;
		if (zc_read(reader, values, scratch, (uint16_t)chunk) < 0)
			return zc_fail(r, ZTRCHECK_IO, (uint16_t)values);
		for (i = 0u; i < (uint16_t)chunk; ++i) {
			if (phase == 0u && (scratch[i] & 0xFEu) != 0u)
				return zc_fail(r, ZTRCHECK_MACRO,
					       (uint16_t)(values + i));
			if (++phase == 3u) phase = 0u;
		}
		values += chunk;
	}
	return 0;
}

static int zc_instruments(const kos_reader_t *reader, const struct zc_song *s,
			  uint8_t *scratch, uint16_t size,
			  struct ztrcheck_report *r)
{
	uint8_t seen[(ZC_CHANNELS + 7u) / 8u];
	uint8_t i, slot;
	for (i = 0u; i < sizeof(seen); ++i) seen[i] = 0u;
	for (i = 0u; i < s->instruments; ++i) {
		uint8_t b[ZC_INSTR_BYTES];
		uint16_t entry = (uint16_t)(s->instr_off +
					    (uint16_t)((uint16_t)i << 4));
		uint8_t number;
		if (zc_read(reader, entry, b, ZC_INSTR_BYTES) < 0)
			return zc_fail(r, ZTRCHECK_IO, entry);
		number = b[0];
		if (number >= ZC_CHANNELS || b[14] != 0u || b[15] != 0u)
			return zc_fail(r, ZTRCHECK_INSTRUMENT, entry);
		if ((seen[number >> 3] & (uint8_t)(1u << (number & 7u))) != 0u)
			return zc_fail(r, ZTRCHECK_INSTRUMENT, entry);
		seen[number >> 3] |= (uint8_t)(1u << (number & 7u));
		if (zc_text(reader, zc_u16(b + 2u), s->strings_end,
			    (char *)0, 0u) < 0)
			return zc_fail(r, ZTRCHECK_STRINGS,
				       (uint16_t)(entry + 2u));
		for (slot = 0u; slot < ZC_MACRO_SLOTS; ++slot) {
			uint16_t at = zc_u16(b + 4u + (uint16_t)slot * 2u);
			if (at != 0u &&
			    zc_macro(reader, at, (uint8_t)(slot + 1u),
				     s, scratch, size, r) < 0)
				return -1;
		}
	}
	return 0;
}

/* The pattern directory: channel, placement inside the pattern data, and
 * file order (streams may leave gaps but never overlap or go back).  The
 * window lives outside scratch so the stream walker's refills cannot
 * clobber it. */
#define ZC_DIR_WINDOW 32u
static uint8_t zc_dir_window[ZC_DIR_WINDOW * ZC_PAT_BYTES];

static int zc_patterns(const kos_reader_t *reader, const struct zc_song *s,
		       struct ztrcheck_report *r)
{
	uint16_t done = 0u;
	uint32_t prior_end = s->patdata_off;
	while (done < s->patterns) {
		uint16_t window = (uint16_t)(s->patterns - done);
		uint16_t j;
		if (window > ZC_DIR_WINDOW) window = ZC_DIR_WINDOW;
		if (zc_read(reader,
			    (uint32_t)s->patdir_off +
				((uint32_t)done << 3),
			    zc_dir_window,
			    (uint16_t)(window << 3)) < 0)
			return zc_fail(r, ZTRCHECK_IO, s->patdir_off);
		for (j = 0u; j < window; ++j) {
			const uint8_t *e = zc_dir_window + (uint16_t)(j << 3);
			uint16_t at = zc_u16(e + 2u);
			uint32_t end = (uint32_t)at + zc_u16(e + 4u);
			if (e[0] >= ZC_CHANNELS || at < s->patdata_off ||
			    end > s->note_off || (uint32_t)at < prior_end)
				return zc_fail(r, ZTRCHECK_PATTERN,
					       (uint16_t)(s->patdir_off +
					(uint16_t)((uint16_t)(done + j) << 3)));
			prior_end = end;
		}
		done = (uint16_t)(done + window);
	}
	return 0;
}

/* A forward window walker over the pattern data: one reader call per
 * scratch window, jumps forward over the gaps between streams. */
struct zc_walk {
	const kos_reader_t *reader;
	uint8_t *buf;
	uint16_t capacity;
	uint32_t pos;		/* file offset of buf[0] */
	uint16_t have;		/* valid bytes in buf */
	uint16_t at;		/* cursor */
	uint32_t end;		/* the pattern-data region end */
};

static int zc_byte(struct zc_walk *w, uint8_t *out)
{
	if (w->at == w->have) {
		uint32_t chunk;
		w->pos += w->at;
		w->at = 0u;
		w->have = 0u;
		if (w->pos >= w->end) return -1;
		chunk = w->end - w->pos;
		if (chunk > w->capacity) chunk = w->capacity;
		if (zc_read(w->reader, w->pos, w->buf, (uint16_t)chunk) < 0)
			return -1;
		w->have = (uint16_t)chunk;
	}
	*out = w->buf[w->at++];
	return 0;
}

/* One stream's events, as libsnd's pattern_streams_ok had them: rows in
 * order inside the pattern, the mask combinations legal, every field in
 * range, and exactly the directory's event count. */
static int zc_stream(struct zc_walk *w, const uint8_t *entry, uint16_t rows,
		     struct ztrcheck_report *r)
{
	uint16_t offset = zc_u16(entry + 2u);
	uint32_t end = (uint32_t)offset + zc_u16(entry + 4u);
	uint16_t expected = zc_u16(entry + 6u);
	uint16_t events = 0u;
	uint16_t last_row = 0u;
	uint8_t first = 1u;
	uint8_t row, mask, modifiers, byte;

	if ((uint32_t)(w->pos + w->at) != (uint32_t)offset) {
		if (offset < w->pos + w->at)
			return zc_fail(r, ZTRCHECK_STREAM, offset);
		if ((uint32_t)offset < w->pos + w->have)
			w->at = (uint16_t)(offset - w->pos);
		else {
			w->pos = offset;
			w->have = 0u;
			w->at = 0u;
		}
	}
	while ((uint32_t)(w->pos + w->at) < end) {
		uint16_t event_at = (uint16_t)(w->pos + w->at);
		if (zc_byte(w, &row) < 0 || zc_byte(w, &mask) < 0)
			return zc_fail(r, ZTRCHECK_STREAM, event_at);
		if ((uint16_t)row >= rows ||
		    (first == 0u && (uint16_t)row <= last_row) || mask == 0u)
			return zc_fail(r, ZTRCHECK_STREAM, event_at);
		first = 0u;
		last_row = row;
		if ((mask & (ZC_EVENT_OFF | ZC_EVENT_RELEASE)) ==
		    (ZC_EVENT_OFF | ZC_EVENT_RELEASE))
			return zc_fail(r, ZTRCHECK_STREAM, event_at);
		if ((mask & ZC_EVENT_NOTE) != 0u) {
			if (zc_byte(w, &byte) < 0 || byte >= ZC_NOTES)
				return zc_fail(r, ZTRCHECK_STREAM,
					       (uint16_t)(w->pos + w->at - 1u));
		}
		if ((mask & ZC_EVENT_INSTRUMENT) != 0u) {
			if (zc_byte(w, &byte) < 0 || byte >= ZC_CHANNELS)
				return zc_fail(r, ZTRCHECK_STREAM,
					       (uint16_t)(w->pos + w->at - 1u));
		}
		if ((mask & ZC_EVENT_VOLUME) != 0u) {
			if (zc_byte(w, &byte) < 0 || byte > 15u)
				return zc_fail(r, ZTRCHECK_STREAM,
					       (uint16_t)(w->pos + w->at - 1u));
		}
		if ((mask & ZC_EVENT_EFFECT) != 0u) {
			if (zc_byte(w, &byte) < 0 ||
			    byte == 0u || byte > ZC_EFFECT_STOP)
				return zc_fail(r, ZTRCHECK_STREAM,
					       (uint16_t)(w->pos + w->at - 1u));
			if (zc_byte(w, &byte) < 0)
				return zc_fail(r, ZTRCHECK_STREAM,
					       (uint16_t)(w->pos + w->at - 1u));
		}
		if ((mask & ZC_EVENT_LEGATO) != 0u &&
		    (mask & ZC_EVENT_NOTE) == 0u)
			return zc_fail(r, ZTRCHECK_STREAM, event_at);
		if ((mask & ZC_EVENT_NOTE) != 0u &&
		    (mask & (ZC_EVENT_OFF | ZC_EVENT_RELEASE)) != 0u)
			return zc_fail(r, ZTRCHECK_STREAM, event_at);
		if ((mask & ZC_EVENT_MODIFIERS) != 0u) {
			if (zc_byte(w, &modifiers) < 0)
				return zc_fail(r, ZTRCHECK_STREAM,
					       (uint16_t)(w->pos + w->at - 1u));
			if ((modifiers & 0x07u) != 0u ||
			    (modifiers & (ZC_MOD_ARP_SET |
					  ZC_MOD_ARP_CLEAR)) ==
				(ZC_MOD_ARP_SET | ZC_MOD_ARP_CLEAR) ||
			    (modifiers & (ZC_MOD_HAIRPIN_SET |
					  ZC_MOD_HAIRPIN_CLEAR)) ==
				(ZC_MOD_HAIRPIN_SET | ZC_MOD_HAIRPIN_CLEAR))
				return zc_fail(r, ZTRCHECK_STREAM,
					       (uint16_t)(w->pos + w->at - 1u));
			if ((modifiers & ZC_MOD_ARP_SET) != 0u &&
			    zc_byte(w, &byte) < 0)
				return zc_fail(r, ZTRCHECK_STREAM,
					       (uint16_t)(w->pos + w->at - 1u));
			if ((modifiers & ZC_MOD_HAIRPIN_SET) != 0u) {
				if (zc_byte(w, &byte) < 0 || byte == 0u ||
				    (byte > 15u && byte < 0xF1u))
					return zc_fail(r, ZTRCHECK_STREAM,
						       (uint16_t)(w->pos + w->at - 1u));
				if (zc_byte(w, &byte) < 0 || byte > 15u)
					return zc_fail(r, ZTRCHECK_STREAM,
						       (uint16_t)(w->pos + w->at - 1u));
			}
		}
		++events;
	}
	if ((uint32_t)(w->pos + w->at) != end || events != expected)
		return zc_fail(r, ZTRCHECK_STREAM, (uint16_t)end);
	return 0;
}

/* Every stream in directory order; the placement pass left the directory
 * in file order, so the walker's reads stay monotonic. */
static int zc_streams(const kos_reader_t *reader, const struct zc_song *s,
		      uint8_t *scratch, uint16_t size,
		      struct ztrcheck_report *r)
{
	struct zc_walk w;
	uint16_t done = 0u;
	w.reader = reader;
	w.buf = scratch;
	w.capacity = size;
	w.pos = s->patdata_off;
	w.have = 0u;
	w.at = 0u;
	w.end = s->note_off;
	while (done < s->patterns) {
		uint16_t window = (uint16_t)(s->patterns - done);
		uint16_t j;
		if (window > ZC_DIR_WINDOW) window = ZC_DIR_WINDOW;
		if (zc_read(reader,
			    (uint32_t)s->patdir_off +
				((uint32_t)done << 3),
			    zc_dir_window,
			    (uint16_t)(window << 3)) < 0)
			return zc_fail(r, ZTRCHECK_IO, s->patdir_off);
		for (j = 0u; j < window; ++j) {
			if (zc_stream(&w, zc_dir_window + (uint16_t)(j << 3),
				      s->rows, r) < 0)
				return -1;
		}
		done = (uint16_t)(done + window);
	}
	return 0;
}

/* The note table: every divisor nonzero and at most 1023. */
static int zc_notes(const kos_reader_t *reader, const struct zc_song *s,
		    struct ztrcheck_report *r)
{
	uint8_t table[ZC_NOTE_BYTES];
	uint16_t i;
	if (zc_read(reader, s->note_off, table, ZC_NOTE_BYTES) < 0)
		return zc_fail(r, ZTRCHECK_IO, s->note_off);
	for (i = 0u; i < ZC_NOTES; ++i) {
		uint16_t divisor = zc_u16(table + (uint16_t)(i << 1));
		if (divisor == 0u || divisor > 1023u)
			return zc_fail(r, ZTRCHECK_NOTES,
				       (uint16_t)(s->note_off + (uint16_t)(i << 1)));
	}
	return 0;
}

int ztrcheck_run(const kos_reader_t *reader, uint8_t *scratch,
		 uint16_t scratch_size, struct ztrcheck_report *report)
{
	uint8_t h[ZC_HEADER_BYTES];
	struct zc_song s;
	report->check = ZTRCHECK_OK;
	report->offset = 0u;
	report->crc_stored = 0u;
	report->crc_computed = 0u;
	if (reader == (const kos_reader_t *)0 ||
	    reader->read_at == (kos_read_at_fn)0 || scratch_size < 8u)
		return zc_fail(report, ZTRCHECK_IO, 0u);
	if (zc_header(reader, h, &s, report) < 0) return -1;
	if (zc_layout(reader, h, &s, report) < 0) return -1;
	if (zc_padding(reader, &s, scratch, scratch_size, report) < 0)
		return -1;
	if (zc_crc(reader, h, &s, scratch, scratch_size, report) < 0)
		return -1;
	if (zc_strings(reader, h, &s, report) < 0) return -1;
	if (zc_instruments(reader, &s, scratch, scratch_size, report) < 0)
		return -1;
	if (zc_patterns(reader, &s, report) < 0)
		return -1;
	if (zc_streams(reader, &s, scratch, scratch_size, report) < 0)
		return -1;
	if (zc_notes(reader, &s, report) < 0) return -1;
	report->rate = s.rate;
	report->speed = s.speed;
	report->rows = s.rows;
	report->orders = s.orders;
	report->patterns = s.patterns;
	report->instruments = s.instruments;
	report->macro_bytes = (uint16_t)(s.patdir_off - s.macro_off);
	report->size = s.logical;
	return 0;
}

const char *ztrcheck_name(uint8_t check)
{
	static const char *const names[] = {
		"ok", "header", "layout", "padding", "strings",
		"instrument", "macro", "pattern", "stream", "notes",
		"crc", "io"
	};
	if (check > ZTRCHECK_IO) return "?";
	return names[check];
}

const char *ztrcheck_detail(uint8_t check)
{
	static const char *const details[] = {
		"the file is a valid ZTR",
		"a header field is outside the format or the player capacity",
		"the sections are not the contiguous ordered layout",
		"bytes past the logical size are not one 1Ah record tail",
		"a string reference does not terminate in the pool",
		"an instrument directory entry is malformed",
		"a macro block is malformed",
		"a pattern directory entry is out of place",
		"an event stream is malformed",
		"a note divisor is zero or above 1023",
		"the content CRC does not match",
		"the reader failed"
	};
	if (check > ZTRCHECK_IO) return "?";
	return details[check];
}
