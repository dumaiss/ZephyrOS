/* KRES core: the §10 validation pass (private).
 *
 * kres_verify opens the file through the installed adapter and checks
 * the header, the catalog extents, every catalog entry's name, order
 * and geometry, and that no two live extents (the header, the catalog
 * areas, the resources) share a record.  It fails closed on a
 * malformed file and never follows an unchecked offset.
 *
 * Memory is bounded and independent of the resource count: the
 * catalog streams through the one-record cache, and the overlap pass
 * marks records in a 4,096-record bitmap window held in the shared
 * buffer, one catalog walk per window (at most 16 windows for the
 * 65,535-record limit).
 */

#include "kres_internal.h"

/* Records one bitmap window covers: the shared buffer's bits. */
#define WINDOW_RECORDS ((uint32_t)KRES_RECORD_SIZE * 8u)

static uint32_t window_base;

/* Mark the records of [start, start + count) inside the window;
 * -KRES_E_BAD when one is marked already (an overlap). */
static int window_mark(uint32_t start, uint32_t count)
{
	uint32_t low = start;
	uint32_t high = start + count;
	uint32_t record;

	if (low < window_base) {
		low = window_base;
	}
	if (high > window_base + WINDOW_RECORDS) {
		high = window_base + WINDOW_RECORDS;
	}
	for (record = low; record < high; ++record) {
		uint16_t bit = (uint16_t)(record - window_base);
		uint8_t mask = (uint8_t)(1u << (bit & 7u));

		if ((kres_shared[bit >> 3] & mask) != 0u) {
			return -KRES_E_BAD;
		}
		kres_shared[bit >> 3] |= mask;
	}
	return 0;
}

/* Verify one entry's fields, returning 0 or a negated error. */
static int entry_check(const uint8_t *entry, const kres_container *c,
		       uint32_t file_size)
{
	uint32_t start = kres_get32(entry + KRES_ENT_START);
	uint32_t allocated = kres_get32(entry + KRES_ENT_ALLOCATED);
	uint32_t used = kres_get32(entry + KRES_ENT_USED);
	uint8_t name_length = entry[KRES_ENT_NAME_LENGTH];
	uint8_t i;

	if (name_length == 0u || name_length > KRES_NAME_MAX) {
		return -KRES_E_BAD;
	}
	for (i = 0u; i < KRES_NAME_MAX; ++i) {
		/* No NUL inside the name, zero padding after it. */
		if ((entry[KRES_ENT_NAME + i] == 0u) != (i >= name_length)) {
			return -KRES_E_BAD;
		}
	}
	if (entry[KRES_ENT_FLAGS] != 0u ||
	    entry[KRES_ENT_RESERVED] != 0u ||
	    entry[KRES_ENT_RESERVED + 1u] != 0u) {
		return -KRES_E_BAD;
	}
	if (start < 1u || allocated == 0u || start > c->next_record ||
	    allocated > c->next_record - start ||
	    used > allocated * KRES_RECORD_SIZE) {
		return -KRES_E_BAD;
	}
	/* The used bytes must exist in the file; an empty resource has
	 * none and may sit past the current file length. */
	if (used > 0u && kres_record_offset(start) + used > file_size) {
		return -KRES_E_BAD;
	}
	return 0;
}

/* The header bytes the parser does not read must be zero. */
static int header_reserved_zero(const uint8_t *header)
{
	uint16_t i;

	if (header[KRES_HDR_RESERVED1] != 0u) {
		return 0;
	}
	for (i = KRES_HDR_USED; i < KRES_RECORD_SIZE; ++i) {
		if (header[i] != 0u) {
			return 0;
		}
	}
	return 1;
}

static int verify_open(kres_container *c, uint32_t size,
		       struct kres_usage *usage)
{
	uint8_t previous[KRES_ENTRY_SIZE];
	uint8_t scratch[KRES_ENTRY_SIZE];
	uint32_t live = 0u;
	uint32_t catalog;
	uint16_t fill = kres_catalog_fill(c->resource_count);
	uint16_t index;
	int result;

	/* The sealed layout: the catalog fills exactly its records and
	 * ends the container. */
	if (c->sealed != 0u &&
	    (c->catalog_records[0] != fill ||
	     (uint32_t)c->catalog_start[0] + fill != c->next_record)) {
		return -KRES_E_BAD;
	}
	if (size < kres_record_offset(
			   (uint32_t)c->catalog_start[c->active_area] + fill)) {
		return -KRES_E_BAD; /* a truncated catalog */
	}
	/* The entry pass: fields, and names strictly increasing (sorted
	 * and unique at once). */
	for (index = 0u; index < c->resource_count; ++index) {
		const uint8_t *entry = kres_entry_at(c, index, scratch);

		if (entry == (const uint8_t *)0) {
			return -KOS_EIO;
		}
		result = entry_check(entry, c, size);
		if (result < 0) {
			return result;
		}
		if (index != 0u &&
		    kres_name_order(previous + KRES_ENT_NAME,
				    previous[KRES_ENT_NAME_LENGTH],
				    entry) >= 0) {
			return -KRES_E_BAD;
		}
		memcpy(previous, entry, KRES_ENTRY_SIZE);
		live += kres_get32(entry + KRES_ENT_ALLOCATED);
	}
	/* The overlap pass, one bitmap window at a time. */
	catalog = (uint32_t)c->catalog_records[0] + c->catalog_records[1];
	for (window_base = 0u; window_base < c->next_record;
	     window_base += WINDOW_RECORDS) {
		memset(kres_shared, 0, KRES_RECORD_SIZE);
		if (window_mark(0u, 1u) < 0 ||
		    window_mark(c->catalog_start[0],
				c->catalog_records[0]) < 0 ||
		    window_mark(c->catalog_start[1],
				c->catalog_records[1]) < 0) {
			return -KRES_E_BAD;
		}
		for (index = 0u; index < c->resource_count; ++index) {
			const uint8_t *entry = kres_entry_at(c, index,
							     scratch);

			if (entry == (const uint8_t *)0) {
				return -KOS_EIO;
			}
			if (window_mark(kres_get32(entry + KRES_ENT_START),
					kres_get32(entry +
						   KRES_ENT_ALLOCATED)) < 0) {
				return -KRES_E_BAD;
			}
		}
	}
	if (usage != (struct kres_usage *)0) {
		usage->resources = c->resource_count;
		usage->catalog_capacity =
			(uint32_t)c->catalog_records[c->active_area] *
			KRES_ENTRIES_PER_RECORD;
		usage->next_record = c->next_record;
		usage->live_records = live;
		usage->catalog_records = catalog;
		usage->abandoned_records = c->next_record - 1u - catalog -
					   live;
	}
	return 0;
}

int kres_verify(const char *path, struct kres_usage *usage)
{
	kres_container c;
	uint32_t size;
	int result;

	if (kres_io == (const struct kres_io *)0 || path == (const char *)0) {
		return -KOS_EINVAL;
	}
	result = kres_io->open(kres_ctx, path, KRES_OPEN_READ);
	if (result < 0) {
		return result;
	}
	memset(&c, 0, sizeof(c));
	c.io_ctx = kres_ctx;
	c.opened = 1u;
	result = kres_io->size(kres_ctx, &size);
	if (result < 0) {
		goto out;
	}
	if (size < KRES_RECORD_SIZE) {
		result = -KRES_E_BAD;
		goto out;
	}
	if (kres_read_records_at(kres_ctx, 0u, kres_shared, 1u) != 1) {
		result = -KRES_E_BAD;
		goto out;
	}
	result = kres_header_parse(kres_shared, &c);
	if (result < 0) {
		goto out;
	}
	if (header_reserved_zero(kres_shared) == 0) {
		result = -KRES_E_BAD;
		goto out;
	}
	result = verify_open(&c, size, usage);
out:
	kres_record_cache_drop(&c);
	(void)kres_io->close(kres_ctx);
	return result;
}
