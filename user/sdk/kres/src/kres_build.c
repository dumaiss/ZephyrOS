/* KRES core: the sealed builder (private).
 *
 * One build at a time, on the installed adapter, with no resource
 * count known in advance: kres_build_begin creates the file with a
 * blank header record, add/write stream resources one after the other
 * from record 1, and end appends the sorted catalog after the last
 * resource and writes the header last - so an interrupted build is
 * simply an invalid file (§8).
 *
 * The accumulating catalog lives in the catalog store (host memory, or
 * XMEM on the KonaOS edition), keyed by the installed context (a
 * container pointer never aliases it), sorted as the names arrive;
 * this member's own state is the shared buffer plus a few bytes.
 */

#include "kres_internal.h"

static uint8_t build_open;
static uint8_t build_active;
static uint16_t build_count;
static uint16_t build_index;
static uint32_t build_next_record;
static uint32_t build_start;
static uint32_t build_used;

/* Close the current resource (build_active != 0): an empty one still
 * owns one zero record, so every entry has a distinct extent. */
static int build_finish_entry(void)
{
	uint8_t entry[KRES_ENTRY_SIZE];
	uint32_t allocated;
	int result;

	build_active = 0u;
	if (build_used == 0u) {
		if (build_next_record >= KRES_RECORD_LIMIT) {
			return -KRES_E_RECLIM;
		}
		result = kres_io->seek(kres_ctx,
				       kres_record_offset(build_next_record));
		if (result < 0) {
			return result;
		}
		result = kres_io->write(kres_ctx, kres_zero_page,
					KRES_RECORD_SIZE);
		if (result != (int)KRES_RECORD_SIZE) {
			return result < 0 ? result : -KOS_EIO;
		}
		build_next_record = (uint32_t)(build_next_record + 1u);
	}
	allocated = build_next_record - build_start;
	result = kres_cache->entry(kres_ctx, build_index, entry);
	if (result < 0) {
		return result;
	}
	kres_put32(entry + KRES_ENT_ALLOCATED, allocated);
	kres_put32(entry + KRES_ENT_USED, build_used);
	return kres_cache->put(kres_ctx, build_index, entry);
}

int kres_build_begin(const char *path)
{
	int result;

	if (kres_io == (const struct kres_io *)0 ||
	    kres_cache == (const struct kres_cache *)0 ||
	    path == (const char *)0) {
		return -KOS_EINVAL;
	}
	if (build_open != 0u) {
		return -KOS_EBUSY;
	}
	result = kres_io->open(kres_ctx, path,
			       KRES_OPEN_WRITE | KRES_OPEN_CREATE);
	if (result < 0) {
		return result;
	}
	result = kres_io->truncate(kres_ctx, 0u);
	if (result < 0) {
		goto fail;
	}
	/* The header slot, zeroed up front so the file never begins
	 * with a plausible magic; the resources start at record 1. */
	result = kres_io->seek(kres_ctx, 0u);
	if (result < 0) {
		goto fail;
	}
	result = kres_io->write(kres_ctx, kres_zero_page, KRES_RECORD_SIZE);
	if (result != (int)KRES_RECORD_SIZE) {
		result = result < 0 ? result : -KOS_EIO;
		goto fail;
	}
	result = kres_cache->load(kres_ctx, 0u, 0u);
	if (result < 0) {
		goto fail;
	}
	build_open = 1u;
	build_count = 0u;
	build_active = 0u;
	build_next_record = 1u;
	return 0;
fail:
	(void)kres_io->close(kres_ctx);
	return result;
}

int kres_build_add(const char *name)
{
	uint8_t entry[KRES_ENTRY_SIZE];
	uint16_t name_length;
	uint16_t low = 0u;
	uint16_t high;
	int result;

	if (build_open == 0u) {
		return -KOS_EINVAL;
	}
	if (build_active != 0u) {
		/* Starting the next resource closes the previous one. */
		result = build_finish_entry();
		if (result < 0) {
			return result;
		}
	}
	if (build_count >= KRES_RESOURCE_MAX) {
		return -KRES_E_FULL;
	}
	name_length = kres_name_check(name);
	if (name_length == 0xFFFFu) {
		return -KOS_EINVAL;
	}
	/* The sorted position, by binary search (names added in order
	 * land at the end and shift nothing). */
	high = build_count;
	if (high != 0u) {
		result = kres_cache->entry(kres_ctx, (uint16_t)(high - 1u),
					   entry);
		if (result < 0) {
			return result;
		}
		if (kres_name_order((const uint8_t *)name,
				    (uint8_t)name_length, entry) > 0) {
			low = high;
		}
	}
	while (low < high) {
		uint16_t mid = (uint16_t)(low + ((uint16_t)(high - low) >> 1));
		int order;

		result = kres_cache->entry(kres_ctx, mid, entry);
		if (result < 0) {
			return result;
		}
		order = kres_name_order((const uint8_t *)name,
					(uint8_t)name_length, entry);
		if (order == 0) {
			return -KOS_EEXIST;
		}
		if (order < 0) {
			high = mid;
		} else {
			low = (uint16_t)(mid + 1u);
		}
	}
	result = kres_cache->insert(kres_ctx, low, build_count);
	if (result < 0) {
		return result;
	}
	kres_entry_emit(entry, (const uint8_t *)name, (uint8_t)name_length,
			build_next_record, 0u, 0u);
	result = kres_cache->put(kres_ctx, low, entry);
	if (result < 0) {
		return result;
	}
	build_count = (uint16_t)(build_count + 1u);
	build_index = low;
	build_start = build_next_record;
	build_used = 0u;
	build_active = 1u;
	return 0;
}

int kres_build_write(const void *src, uint16_t len)
{
	uint16_t done = 0u;

	if (build_open == 0u || build_active == 0u ||
	    src == (const void *)0 || len == 0u) {
		return -KOS_EINVAL;
	}
	if ((build_used & (KRES_RECORD_SIZE - 1u)) != 0u) {
		return -KOS_EINVAL; /* only the final write may be partial */
	}
	while (done < len) {
		uint16_t chunk = (uint16_t)(len - done);
		int result;

		if (chunk > KRES_RECORD_SIZE) {
			chunk = KRES_RECORD_SIZE;
		}
		if (build_next_record >= KRES_RECORD_LIMIT) {
			return -KRES_E_RECLIM;
		}
		memcpy(kres_shared, (const uint8_t *)src + done, chunk);
		if (chunk < KRES_RECORD_SIZE) {
			/* The final partial record is padded with zeroes
			 * (the sealed format's padding, at most 511 bytes
			 * per resource). */
			memset(kres_shared + chunk, 0,
			       KRES_RECORD_SIZE - chunk);
		}
		result = kres_io->seek(kres_ctx,
				       kres_record_offset(build_next_record));
		if (result < 0) {
			return result;
		}
		result = kres_io->write(kres_ctx, kres_shared,
					KRES_RECORD_SIZE);
		if (result < 0) {
			return result;
		}
		if (result != (int)KRES_RECORD_SIZE) {
			return -KOS_EIO;
		}
		done = (uint16_t)(done + chunk);
		build_used = (uint32_t)(build_used + chunk);
		build_next_record = (uint32_t)(build_next_record + 1u);
	}
	return len;
}

int kres_build_end(void)
{
	kres_container state;
	uint16_t fill;
	int result;

	if (build_open == 0u) {
		return -KOS_EINVAL;
	}
	if (build_active != 0u) {
		result = build_finish_entry();
		if (result < 0) {
			goto fail;
		}
	}
	/* The sorted catalog after the last resource, then the header
	 * last. */
	fill = kres_catalog_fill(build_count);
	if (fill > KRES_RECORD_LIMIT - build_next_record) {
		result = -KRES_E_RECLIM;
		goto fail;
	}
	if (fill != 0u) {
		result = kres_cache->store(kres_ctx,
					   (uint16_t)build_next_record,
					   build_count);
		if (result < 0) {
			goto fail;
		}
	}
	result = kres_io->sync(kres_ctx);
	if (result < 0) {
		goto fail;
	}
	memset(&state, 0, sizeof(state));
	state.sealed = 1u;
	state.resource_count = build_count;
	state.catalog_start[0] = (uint16_t)build_next_record;
	state.catalog_records[0] = fill;
	state.next_record = build_next_record + fill;
	kres_header_emit(&state, kres_shared);
	result = kres_io->seek(kres_ctx, 0u);
	if (result < 0) {
		goto fail;
	}
	result = kres_io->write(kres_ctx, kres_shared, KRES_RECORD_SIZE);
	if (result != (int)KRES_RECORD_SIZE) {
		result = result < 0 ? result : -KOS_EIO;
		goto fail;
	}
	result = kres_io->sync(kres_ctx);
	if (result < 0) {
		goto fail;
	}
	(void)kres_cache->release(kres_ctx);
	build_open = 0u;
	return kres_io->close(kres_ctx);
fail:
	(void)kres_build_abort();
	return result;
}

int kres_build_abort(void)
{
	int result = 0;

	if (build_open != 0u) {
		result = kres_io->close(kres_ctx);
		(void)kres_cache->release(kres_ctx);
	}
	build_open = 0u;
	build_active = 0u;
	build_count = 0u;
	return result;
}
