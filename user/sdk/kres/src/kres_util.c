/* KRES core: shared state and byte-level helpers (private).
 *
 * Owns the one shared I/O buffer, the installed adapter and catalog
 * cache, the header parser, the entry emit/parse forms and the name
 * rules.  Compiled for both the host and the Z80.
 */

#include "kres_internal.h"

/* The one shared I/O buffer: 512 bytes, never one per resource. */
uint8_t kres_shared[KRES_RECORD_SIZE];

/* The platform adapter and catalog cache, installed once by the
 * platform glue before the first call. */
const struct kres_io *kres_io;
const struct kres_cache *kres_cache;
void *kres_ctx;

/* The mutation-side entry points the read path reaches without linking
 * the mutation member: kres_open's create-on-missing fallback and
 * kres_close's flush.  The mutation member's startup initializer
 * installs both, so a program that links no mutation member keeps the
 * read path self-contained (design §8, "Library size"). */
int (*kres_create_hook)(const char *path);
int (*kres_flush_hook)(kres_container *c);

void kres_install(const struct kres_io *io, const struct kres_cache *cache,
		  void *ctx)
{
	kres_io = io;
	kres_cache = cache;
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

void kres_ent_clear(uint8_t *entry)
{
	uint8_t i;

	for (i = 0u; i < KRES_ENTRY_SIZE; ++i) {
		entry[i] = 0u;
	}
}

/* --- adapter block operations ----------------------------------------- */

/* `count` whole records at `record`, into or out of `buffer`, in the
 * shared buffer (so `count` stays small: callers chunk).  Returns the
 * count of records moved, or a negative error. */
int kres_io_blocks(void *ctx, uint32_t record, uint8_t *buffer,
		   uint32_t count, int writing)
{
	uint32_t i;

	(void)record; /* the caller seeks before the walk */
	for (i = 0u; i < count; ++i) {
		int result;

		if (writing) {
			result = kres_io->write(ctx, buffer,
						KRES_RECORD_SIZE);
			if (result < 0) {
				return result;
			}
			if (result != (int)KRES_RECORD_SIZE) {
				return -KOS_EIO;
			}
		} else {
			result = kres_io->read(ctx, kres_shared,
					       KRES_RECORD_SIZE);
			if (result < 0) {
				return result;
			}
			if (result != (int)KRES_RECORD_SIZE) {
				return (int)i; /* a short read ends it */
			}
			memcpy(buffer, kres_shared, KRES_RECORD_SIZE);
		}
		buffer += KRES_RECORD_SIZE;
	}
	return (int)count;
}

int kres_read_records_at(void *ctx, uint32_t record, uint8_t *dst,
			 uint32_t count)
{
	int result;

	result = kres_io->seek(ctx, kres_record_offset(record));
	if (result < 0) {
		return result;
	}
	return kres_io_blocks(ctx, record, dst, count, 0);
}

/* --- the header -------------------------------------------------------- */

int kres_header_parse(const uint8_t *header, kres_container *out)
{
	uint32_t next_record;
	uint32_t flags;
	uint16_t count;
	uint8_t active;

	/* The fixed fields sit as adjacent pairs: one 32-bit compare per
	 * pair (magic "KRES", major|minor, record|catalog records). */
	if (kres_get32(header + KRES_HDR_MAGIC) != 0x5345524Bu ||
	    kres_get32(header + KRES_HDR_MAJOR) !=
		    (KRES_HDR_MAJOR_V1 |
		     ((uint32_t)KRES_HDR_MINOR_V1 << 16)) ||
	    kres_get32(header + KRES_HDR_RECORD_SIZE) !=
		    (KRES_RECORD_SIZE |
		     ((uint32_t)KRES_CATALOG_RECORDS << 16))) {
		return -KRES_E_BAD;
	}
	flags = kres_get32(header + KRES_HDR_FLAGS);
	active = header[KRES_HDR_ACTIVE_AREA];
	next_record = kres_get32(header + KRES_HDR_NEXT_RECORD);
	count = kres_get16(header + KRES_HDR_RES_COUNT);
	if ((flags & ~(uint32_t)KRES_HDR_FLAG_SEALED) != 0u) {
		return -KRES_E_BAD;
	}
	if ((flags & KRES_HDR_FLAG_SEALED) != 0u) {
		if (active != 0u ||
		    kres_get32(header + KRES_HDR_GENERATION) != 0u) {
			return -KRES_E_BAD;
		}
	} else if (active > 1u) {
		return -KRES_E_BAD;
	}
	if (count > KRES_RESOURCE_MAX || next_record > KRES_RECORD_LIMIT ||
	    next_record < KRES_DATA_BASE_SEALED) {
		return -KRES_E_BAD;
	}
	out->sealed = ((flags & KRES_HDR_FLAG_SEALED) != 0u);
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
	kres_ent_clear(entry);
	memcpy(entry + KRES_ENT_NAME, name, name_length);
	kres_put32(entry + KRES_ENT_START, start);
	kres_put32(entry + KRES_ENT_ALLOCATED, allocated);
	kres_put32(entry + KRES_ENT_USED, used);
	entry[KRES_ENT_NAME_LENGTH] = name_length;
}

void kres_entry_info(const uint8_t *entry, struct kres_info *out)
{
	uint8_t length = entry[KRES_ENT_NAME_LENGTH];

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

unsigned int kres_find(kres_container *c, const char *name,
		       uint8_t name_length)
{
	unsigned int index;

	for (index = 0u; index < c->resource_count; ++index) {
		uint8_t entry[KRES_ENTRY_SIZE];

		if (kres_entry_get(c, index, entry) < 0) {
			break;
		}
		if (entry[KRES_ENT_NAME_LENGTH] == name_length &&
		    memcmp(entry + KRES_ENT_NAME, name, name_length) == 0) {
			return index;
		}
	}
	return 0xFFFFu;
}

int kres_entry_get(kres_container *c, unsigned int index, uint8_t *out)
{
	return kres_cache->entry(c, index, out);
}

int kres_entry_put32(kres_container *c, unsigned int index,
		     uint16_t offset, uint32_t value)
{
	uint8_t entry[KRES_ENTRY_SIZE];

	if (kres_entry_get(c, index, entry) < 0) {
		return -KOS_EIO;
	}
	kres_put32(entry + offset, value);
	return kres_cache->put(c, index, entry);
}
