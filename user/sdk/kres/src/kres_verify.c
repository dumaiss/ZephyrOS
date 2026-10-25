/* KRES core: the §10 validation pass (private).
 *
 * kres_verify opens the file through the installed adapter and checks
 * the header, every catalog entry's name and geometry, the name
 * uniqueness and the extents against `next_record` and the real file
 * size.  It fails closed on a malformed file and never follows an
 * unchecked offset.
 *
 * The walk streams the catalog records one at a time through the
 * shared buffer (no private tables), so this member's own state is a
 * single 64-byte entry.
 */

#include "kres_internal.h"

/* Verify one entry's fields, returning 0 or a negated error. */
static int entry_check(const uint8_t *entry, kres_container *c,
		       uint32_t file_size)
{
	uint32_t start = kres_get32(entry + KRES_ENT_START);
	uint32_t allocated = kres_get32(entry + KRES_ENT_ALLOCATED);
	uint32_t used = kres_get32(entry + KRES_ENT_USED);
	uint32_t base = c->sealed != 0u ? KRES_DATA_BASE_SEALED
					: KRES_DATA_BASE_MUT;
	uint8_t name_length = entry[KRES_ENT_NAME_LENGTH];

	if (name_length == 0u || name_length > KRES_NAME_MAX) {
		return -KRES_E_BAD;
	}
	if (entry[KRES_ENT_FLAGS] != 0u ||
	    entry[KRES_ENT_RESERVED] != 0u ||
	    entry[KRES_ENT_RESERVED + 1u] != 0u) {
		return -KRES_E_BAD;
	}
	if (start < base || allocated == 0u ||
	    used > allocated * KRES_RECORD_SIZE ||
	    start > c->next_record ||
	    allocated > c->next_record - start) {
		return -KRES_E_BAD;
	}
	/* The used bytes must exist in the file; an empty resource has
	 * none and may sit past the current file length. */
	if (used > 0u && start * KRES_RECORD_SIZE + used > file_size) {
		return -KRES_E_BAD;
	}
	return 0;
}

/* Read one record of the active area into the shared buffer. */
static int read_area_record(void *ctx, uint32_t area, unsigned int rec)
{
	int result = kres_read_records_at(ctx, area + rec, kres_shared, 1u);

	return result == 1 ? 0 : (result < 0 ? result : -KRES_E_BAD);
}

/* The pairwise pass: compare entry `a` (a copy in `entry_a`) against
 * the entries of one shared-buffer record, from `start_within`, for
 * duplicate names and overlapping extents. */
static int pair_check(unsigned int count, const uint8_t *entry_a,
		      unsigned int first_index, unsigned int start_within)
{
	unsigned int within;

	for (within = start_within; within < 8u; ++within) {
		unsigned int index = first_index + within;
		const uint8_t *eb;

		if (index >= count) {
			continue;
		}
		eb = kres_shared + within * KRES_ENTRY_SIZE;
		if (entry_a[KRES_ENT_NAME_LENGTH] ==
		    eb[KRES_ENT_NAME_LENGTH] &&
		    memcmp(entry_a + KRES_ENT_NAME, eb + KRES_ENT_NAME,
			   entry_a[KRES_ENT_NAME_LENGTH]) == 0) {
			return -KRES_E_BAD;
		}
		{
			uint32_t a_start = kres_get32(entry_a +
						      KRES_ENT_START);
			uint32_t a_end = a_start + kres_get32(entry_a +
						KRES_ENT_ALLOCATED);
			uint32_t b_start = kres_get32(eb + KRES_ENT_START);
			uint32_t b_end = b_start + kres_get32(eb +
						KRES_ENT_ALLOCATED);

			if (a_start < b_end && b_start < a_end) {
				return -KRES_E_BAD;
			}
		}
	}
	return 0;
}

int kres_verify(const char *path)
{
	kres_container c;
	uint8_t entry_a[KRES_ENTRY_SIZE];
	uint32_t size;
	uint32_t area;
	unsigned int record;
	unsigned int index_a;
	int result;

	if (kres_io == (const struct kres_io *)0 || path == (const char *)0) {
		return -KOS_EINVAL;
	}
	result = kres_io->open(kres_ctx, path, KRES_OPEN_READ);
	if (result < 0) {
		return result;
	}
	result = kres_io->size(kres_ctx, &size);
	if (result < 0) {
		goto out;
	}
	result = kres_io->seek(kres_ctx, 0u);
	if (result < 0) {
		goto out;
	}
	result = kres_io->read(kres_ctx, kres_shared, KRES_RECORD_SIZE);
	if (result < 0) {
		goto out;
	}
	if (result != (int)KRES_RECORD_SIZE) {
		result = -KRES_E_BAD;
		goto out;
	}
	memset(&c, 0, sizeof(c));
	result = kres_header_parse(kres_shared, &c);
	if (result < 0) {
		goto out;
	}
	if (size < (uint32_t)(c.sealed != 0u ? KRES_DATA_BASE_SEALED
					     : KRES_DATA_BASE_MUT) *
			   KRES_RECORD_SIZE) {
		result = -KRES_E_BAD;
		goto out;
	}
	/* One pass per record: every live entry's fields. */
	area = c.sealed != 0u ? KRES_AREA_A_BASE
			      : c.active_area != 0u ? KRES_AREA_B_BASE
						    : KRES_AREA_A_BASE;
	for (record = 0u; record < c.resource_count; ++record) {
		unsigned int within = record % 8u;
		int at_record = (int)(record / 8u);

		if (within == 0u) {
			result = read_area_record(kres_ctx, area, at_record);
			if (result < 0) {
				goto out;
			}
		}
		result = entry_check(kres_shared + within * KRES_ENTRY_SIZE,
				     &c, size);
		if (result < 0) {
			goto out;
		}
	}
	/* The pairwise pass: name uniqueness and non-overlap, reading
	 * each record at most four times (no stored tables). */
	for (index_a = 0u; index_a < c.resource_count; ++index_a) {
		unsigned int rec_a = index_a / 8u;
		unsigned int rec_b;

		result = read_area_record(kres_ctx, area, rec_a);
		if (result < 0) {
			goto out;
		}
		memcpy(entry_a, kres_shared + (index_a % 8u) * KRES_ENTRY_SIZE,
		       KRES_ENTRY_SIZE);
		/* The later entries of record a. */
		result = pair_check(c.resource_count, entry_a,
				    rec_a * 8u, index_a % 8u + 1u);
		if (result < 0) {
			goto out;
		}
		/* The later records' entries. */
		for (rec_b = rec_a + 1u; rec_b < KRES_CATALOG_RECORDS;
		     ++rec_b) {
			unsigned int first_b = rec_b * 8u;

			if (first_b >= c.resource_count) {
				continue;
			}
			result = read_area_record(kres_ctx, area, rec_b);
			if (result < 0) {
				goto out;
			}
			result = pair_check(c.resource_count, entry_a,
					    first_b, 0u);
			if (result < 0) {
				goto out;
			}
		}
	}
	result = 0;
out:
	(void)kres_io->close(kres_ctx);
	return result;
}
