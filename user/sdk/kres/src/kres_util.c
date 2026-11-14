/* KRES core: shared state and byte-level helpers (private).
 *
 * Owns the one shared I/O buffer, the installed adapter, the header
 * parser, the entry emit/parse forms, the name rules and the catalog
 * lookup: a one-record cache over the sorted catalog of a read-only
 * container, the catalog store for a writable one, and the binary
 * search over either.  Compiled for both the host and the Z80.
 */

#include "kres_internal.h"

/* The one shared I/O buffer: 512 bytes, never one per resource. */
uint8_t kres_shared[KRES_RECORD_SIZE];

/* The platform adapter and catalog store, installed once by the
 * platform glue before the first call. */
const struct kres_io *kres_io;
const struct kres_cache *kres_cache;
void *kres_ctx;

/* The mutation-side entry points the read path reaches without linking
 * the mutation member: kres_open's create-on-missing fallback and
 * kres_close's flush.  kres_install_mut installs both, so a program
 * that links no mutation member keeps the read path self-contained
 * (design §8, "Library size"). */
int (*kres_create_hook)(const char *path);
int (*kres_flush_hook)(kres_container *c);

void kres_install(const struct kres_io *io, void *ctx)
{
	kres_io = io;
	kres_cache = (const struct kres_cache *)0;
	kres_ctx = ctx;
}

/* --- byte helpers (kres_layout.h declarations) ------------------------- */

void kres_put16(uint8_t *at, uint16_t value)
{
	at[0] = (uint8_t)value;
	at[1] = (uint8_t)(value >> 8);
}

void kres_put32(uint8_t *at, uint32_t value)
{
	at[0] = (uint8_t)value;
	at[1] = (uint8_t)(value >> 8);
	at[2] = (uint8_t)(value >> 16);
	at[3] = (uint8_t)(value >> 24);
}

uint16_t kres_get16(const uint8_t *at)
{
	return (uint16_t)at[0] | ((uint16_t)at[1] << 8);
}

uint32_t kres_get32(const uint8_t *at)
{
	return (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
	       ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

uint16_t kres_header_check(const uint8_t *header)
{
	uint16_t crc = 0xFFFFu;
	uint8_t i;

	for (i = 0u; i < KRES_HDR_CHECK; ++i) {
		uint8_t bit;

		crc ^= (uint16_t)header[i] << 8;
		for (bit = 0u; bit < 8u; ++bit) {
			crc = (crc & 0x8000u) != 0u
				      ? (uint16_t)((crc << 1) ^ 0x1021u)
				      : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

/* --- adapter block operations ----------------------------------------- */

int kres_io_blocks(void *ctx, uint8_t *buffer, uint16_t count, int writing)
{
	uint16_t i;

	for (i = 0u; i < count; ++i) {
		int result;

		if (writing) {
			result = kres_io->write(ctx, buffer,
						KRES_RECORD_SIZE);
		} else {
			result = kres_io->read(ctx, buffer,
					       KRES_RECORD_SIZE);
		}
		if (result < 0) {
			return result;
		}
		if (result != (int)KRES_RECORD_SIZE) {
			return writing ? -KOS_EIO : (int)i;
		}
		buffer += KRES_RECORD_SIZE;
	}
	return (int)count;
}

int kres_read_records_at(void *ctx, uint32_t record, uint8_t *dst,
			 uint16_t count)
{
	int result;

	result = kres_io->seek(ctx, kres_record_offset(record));
	if (result < 0) {
		return result;
	}
	return kres_io_blocks(ctx, dst, count, 0);
}

/* --- the header -------------------------------------------------------- */

/* A catalog extent lies inside [1, next_record). */
static int extent_ok(uint32_t start, uint32_t records, uint32_t next)
{
	return start >= 1u && start <= next && records <= next - start;
}

int kres_header_parse(const uint8_t *header, kres_container *out)
{
	uint32_t next_record;
	uint32_t flags;
	uint32_t a_start;
	uint32_t a_records;
	uint32_t b_start;
	uint32_t b_records;
	uint16_t count;
	uint8_t active;
	uint8_t sealed;

	/* The fixed fields sit as adjacent pairs: one 32-bit compare per
	 * pair (magic "KRES", major|minor, record size|reserved). */
	if (kres_get32(header + KRES_HDR_MAGIC) != 0x5345524Bu ||
	    kres_get32(header + KRES_HDR_MAJOR) !=
		    (KRES_HDR_MAJOR_VALUE |
		     ((uint32_t)KRES_HDR_MINOR_VALUE << 16)) ||
	    kres_get32(header + KRES_HDR_RECORD_SIZE) != KRES_RECORD_SIZE ||
	    kres_get16(header + KRES_HDR_CHECK) !=
		    kres_header_check(header)) {
		return -KRES_E_BAD;
	}
	flags = kres_get32(header + KRES_HDR_FLAGS);
	if ((flags & ~(uint32_t)KRES_HDR_FLAG_SEALED) != 0u) {
		return -KRES_E_BAD;
	}
	sealed = (uint8_t)(flags & KRES_HDR_FLAG_SEALED);
	active = header[KRES_HDR_ACTIVE_AREA];
	count = kres_get16(header + KRES_HDR_RES_COUNT);
	next_record = kres_get32(header + KRES_HDR_NEXT_RECORD);
	a_start = kres_get32(header + KRES_HDR_CAT_A_START);
	a_records = kres_get32(header + KRES_HDR_CAT_A_RECS);
	b_start = kres_get32(header + KRES_HDR_CAT_B_START);
	b_records = kres_get32(header + KRES_HDR_CAT_B_RECS);
	if (next_record > KRES_RECORD_LIMIT ||
	    !extent_ok(a_start, a_records, next_record)) {
		return -KRES_E_BAD;
	}
	if (sealed != 0u) {
		/* One catalog, no commit machinery. */
		if (active != 0u || b_start != 0u || b_records != 0u ||
		    kres_get32(header + KRES_HDR_GENERATION) != 0u) {
			return -KRES_E_BAD;
		}
	} else {
		/* Two disjoint catalog areas, each at least one record. */
		if (active > 1u || a_records == 0u || b_records == 0u ||
		    !extent_ok(b_start, b_records, next_record) ||
		    (a_start < b_start + b_records &&
		     b_start < a_start + a_records)) {
			return -KRES_E_BAD;
		}
	}
	out->catalog_start[0] = (uint16_t)a_start;
	out->catalog_records[0] = (uint16_t)a_records;
	out->catalog_start[1] = (uint16_t)b_start;
	out->catalog_records[1] = (uint16_t)b_records;
	if (kres_catalog_fill(count) > out->catalog_records[active]) {
		return -KRES_E_BAD;
	}
	out->sealed = sealed;
	out->generation = kres_get32(header + KRES_HDR_GENERATION);
	out->active_area = active;
	out->resource_count = count;
	out->next_record = next_record;
	return 0;
}

/* --- catalog entry forms ------------------------------------------------ */

void kres_entry_emit(uint8_t *entry, const uint8_t *name,
		     uint8_t name_length, uint32_t start,
		     uint32_t allocated, uint32_t used)
{
	memset(entry, 0, KRES_ENTRY_SIZE);
	memcpy(entry + KRES_ENT_NAME, name, name_length);
	kres_put32(entry + KRES_ENT_START, start);
	kres_put32(entry + KRES_ENT_ALLOCATED, allocated);
	kres_put32(entry + KRES_ENT_USED, used);
	entry[KRES_ENT_NAME_LENGTH] = name_length;
}

void kres_entry_info(const uint8_t *entry, struct kres_info *out)
{
	uint8_t length = entry[KRES_ENT_NAME_LENGTH];

	if (length > KRES_NAME_MAX) {
		length = KRES_NAME_MAX; /* never past the buffer */
	}
	memcpy(out->name, entry + KRES_ENT_NAME, length);
	out->name[length] = 0u;
	out->name_length = length;
	out->flags = entry[KRES_ENT_FLAGS];
	out->reserved = 0u;
	out->used_bytes = kres_get32(entry + KRES_ENT_USED);
	out->allocated_records = kres_get32(entry + KRES_ENT_ALLOCATED);
}

/* --- names -------------------------------------------------------------- */

uint16_t kres_name_check(const char *name)
{
	uint16_t length = 0u;

	if (name == (const char *)0) {
		return 0xFFFFu;
	}
	while (name[length] != '\0') {
		if (length >= KRES_NAME_MAX) {
			return 0xFFFFu;
		}
		++length;
	}
	return (length == 0u) ? 0xFFFFu : length;
}

int kres_name_order(const uint8_t *name, uint8_t length,
		    const uint8_t *entry)
{
	uint8_t entry_length = entry[KRES_ENT_NAME_LENGTH];
	uint8_t common = (length < entry_length) ? length : entry_length;
	uint8_t i;

	for (i = 0u; i < common; ++i) {
		if (name[i] != entry[KRES_ENT_NAME + i]) {
			return (name[i] < entry[KRES_ENT_NAME + i]) ? -1 : 1;
		}
	}
	return (int)length - (int)entry_length;
}

/* --- the catalog -------------------------------------------------------- */

/* The record cache: one catalog record of one read-only container. */
static uint8_t record_cache[KRES_RECORD_SIZE];
static const kres_container *record_owner;
static uint16_t record_number;

void kres_record_cache_drop(const kres_container *c)
{
	if (record_owner == c) {
		record_owner = (const kres_container *)0;
	}
}

const uint8_t *kres_entry_at(kres_container *c, uint16_t index,
			     uint8_t *scratch)
{
	uint16_t record;

	if (c->writable != 0u) {
		if (kres_cache->entry(c, index, scratch) < 0) {
			return (const uint8_t *)0;
		}
		return (const uint8_t *)scratch;
	}
	record = (uint16_t)(c->catalog_start[c->active_area] + (index >> 3));
	if (record_owner != c || record_number != record) {
		record_owner = (const kres_container *)0;
		if (kres_io->seek(c->io_ctx, kres_record_offset(record)) < 0 ||
		    kres_io->read(c->io_ctx, record_cache,
				  KRES_RECORD_SIZE) !=
			    (int)KRES_RECORD_SIZE) {
			return (const uint8_t *)0;
		}
		record_owner = c;
		record_number = record;
	}
	return record_cache + ((uint16_t)(index & 7u) << 6);
}

int kres_search(kres_container *c, const char *name, uint8_t length,
		uint16_t *pos)
{
	uint8_t scratch[KRES_ENTRY_SIZE];
	uint16_t low = 0u;
	uint16_t high = c->resource_count;

	while (low < high) {
		uint16_t mid = (uint16_t)(low + ((uint16_t)(high - low) >> 1));
		const uint8_t *entry = kres_entry_at(c, mid, scratch);
		int order;

		if (entry == (const uint8_t *)0) {
			return -KOS_EIO;
		}
		order = kres_name_order((const uint8_t *)name, length, entry);
		if (order == 0) {
			*pos = mid;
			return 1;
		}
		if (order < 0) {
			high = mid;
		} else {
			low = (uint16_t)(mid + 1u);
		}
	}
	*pos = low;
	return 0;
}

uint16_t kres_find(kres_container *c, const char *name,
		   uint8_t name_length)
{
	uint16_t pos;

	return kres_search(c, name, name_length, &pos) == 1 ? pos
							     : KRES_NO_ENTRY;
}

int kres_entry_get(kres_container *c, uint16_t index, uint8_t *out)
{
	const uint8_t *entry = kres_entry_at(c, index, out);

	if (entry == (const uint8_t *)0) {
		return -KOS_EIO;
	}
	if (entry != out) {
		memcpy(out, entry, KRES_ENTRY_SIZE);
	}
	return 0;
}
