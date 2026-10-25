/* KRES core: the sealed builder (private).
 *
 * One build at a time, on the installed adapter: kres_build_begin
 * creates the file with the catalog reserved at the front, add/write
 * stream resources one after the other from record 5, and end writes
 * the catalog area and the header last — its magic the final bytes,
 * so an interrupted build is simply an invalid file (§8).
 *
 * The accumulating catalog lives in the catalog cache (design §5.5),
 * not in a private array: on the KonaOS edition it is XMEM, and this
 * member's own state is the shared buffer plus a handful of bytes.
 */

#include "kres_internal.h"

/* The accumulating catalog is keyed by the installed context, the
 * same ctx the build file's adapter calls carry (a container pointer
 * never aliases it). */

static uint8_t build_open;
static uint8_t build_entry_count;
static uint8_t build_count;
static uint8_t build_active;
static uint32_t build_next_record;
static uint32_t build_start;
static uint32_t build_used;
static uint8_t build_name[KRES_NAME_MAX];
static uint8_t build_name_length;

/* Close the current resource's entry (build_active != 0): compute its
 * allocated records and append it to the catalog in the cache. */
static void build_finish_entry(void)
{
	uint8_t entry[KRES_ENTRY_SIZE];
	uint32_t allocated = (build_used + KRES_RECORD_SIZE - 1u) /
			     KRES_RECORD_SIZE;

	if (allocated == 0u) {
		allocated = 1u;
	}
	kres_entry_emit(entry, build_name, build_name_length, build_start,
			allocated, build_used);
	(void)kres_cache->put(kres_ctx, build_count, entry);
	build_count = (uint8_t)(build_count + 1u);
	build_active = 0u;
}

int kres_build_begin(const char *path, uint8_t entry_count)
{
	int result;

	if (kres_io == (const struct kres_io *)0 || path == (const char *)0 ||
	    entry_count == 0u || entry_count > KRES_RESOURCE_MAX) {
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
	 * with a plausible magic; the resources start at record 5. */
	memset(kres_shared, 0, KRES_RECORD_SIZE);
	result = kres_io->seek(kres_ctx, 0u);
	if (result < 0) {
		goto fail;
	}
	result = kres_io->write(kres_ctx, kres_shared, KRES_RECORD_SIZE);
	if (result < 0) {
		goto fail;
	}
	if (result != (int)KRES_RECORD_SIZE) {
		result = -KOS_EIO;
		goto fail;
	}
	build_open = 1u;
	build_entry_count = entry_count;
	build_count = 0u;
	build_active = 0u;
	build_next_record = KRES_DATA_BASE_SEALED;
	return 0;
fail:
	(void)kres_io->close(kres_ctx);
	return result;
}

int kres_build_add(const char *name)
{
	uint16_t name_length;
	unsigned int i;

	if (build_open == 0u) {
		return -KOS_EINVAL;
	}
	if (build_active != 0u) {
		/* Starting the next resource closes the previous one. */
		build_finish_entry();
	}
	if (build_count >= build_entry_count) {
		return -KRES_E_FULL;
	}
	name_length = kres_name_check(name);
	if (name_length == 0xFFFFu) {
		return -KOS_EINVAL;
	}
	for (i = 0u; i < build_count; ++i) {
		uint8_t prior[KRES_ENTRY_SIZE];

		if (kres_cache->entry(kres_ctx, i, prior) < 0) {
			return -KOS_EIO;
		}
		if (prior[KRES_ENT_NAME_LENGTH] == (uint8_t)name_length &&
		    memcmp(prior + KRES_ENT_NAME, name, name_length) == 0) {
			return -KOS_EEXIST;
		}
	}
	memcpy(build_name, name, name_length);
	build_name_length = (uint8_t)name_length;
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
	while (done < len) {
		uint16_t chunk = (uint16_t)(len - done);
		uint32_t record = build_next_record;
		int result;

		if (chunk > KRES_RECORD_SIZE) {
			chunk = KRES_RECORD_SIZE;
		}
		if (record > KRES_RECORD_LIMIT) {
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
		result = kres_io->seek(kres_ctx, kres_record_offset(record));
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
	uint8_t header[KRES_RECORD_SIZE];
	uint8_t zero[KRES_ENTRY_SIZE];
	unsigned int i;
	int result;

	if (build_open == 0u) {
		return -KOS_EINVAL;
	}
	if (build_active == 0u) {
		result = -KOS_EINVAL; /* no resource was started */
		goto fail;
	}
	/* Close the current resource's entry and zero the rest of the
	 * catalog, then write the area (records 1..4) and the header
	 * last. */
	build_finish_entry();
	memset(zero, 0, sizeof(zero));
	for (i = build_count; i < KRES_RESOURCE_MAX; ++i) {
		(void)kres_cache->put(kres_ctx, i, zero);
	}
	result = kres_cache->store(kres_ctx, KRES_AREA_A_BASE);
	if (result < 0) {
		goto fail;
	}
	memset(header, 0, KRES_RECORD_SIZE);
	memcpy(header + KRES_HDR_MAGIC, "KRES", 4);
	kres_put16(header + KRES_HDR_MAJOR, KRES_HDR_MAJOR_V1);
	kres_put16(header + KRES_HDR_MINOR, KRES_HDR_MINOR_V1);
	kres_put16(header + KRES_HDR_RECORD_SIZE, KRES_RECORD_SIZE);
	kres_put16(header + KRES_HDR_CATALOG_RECS, KRES_CATALOG_RECORDS);
	kres_put16(header + KRES_HDR_RES_COUNT, build_count);
	kres_put32(header + KRES_HDR_NEXT_RECORD, build_next_record);
	kres_put32(header + KRES_HDR_FLAGS, KRES_HDR_FLAG_SEALED);
	result = kres_io->seek(kres_ctx, 0u);
	if (result < 0) {
		goto fail;
	}
	result = kres_io->write(kres_ctx, header, KRES_RECORD_SIZE);
	if (result != (int)KRES_RECORD_SIZE) {
		result = -KOS_EIO;
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
	(void)kres_cache->release(kres_ctx);
	build_open = 0u;
	(void)kres_io->close(kres_ctx);
	return result;
}

int kres_build_abort(void)
{
	int result = 0;

	if (build_open != 0u) {
		result = kres_io->close(kres_ctx);
	}
	(void)kres_cache->release(kres_ctx);
	build_open = 0u;
	build_active = 0u;
	build_count = 0u;
	return result;
}
