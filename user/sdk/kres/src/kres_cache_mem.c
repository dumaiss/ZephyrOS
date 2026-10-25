/* KRES core: the memory catalog cache backend (private).
 *
 * The host edition's cache.  Compaction holds the source and the
 * rebuilt container open together, so the backend keeps two 2 KiB
 * areas, keyed by the ctx each was loaded for (least-recently used
 * replacement beyond two).  The KonaOS edition installs an XMEM
 * backend instead, so this member never links into a target program.
 */

#include "kres_internal.h"

#define MEM_AREAS 2u

static uint8_t storage[MEM_AREAS][KRES_CATALOG_RECORDS * KRES_RECORD_SIZE];
static void *owners[MEM_AREAS];
static unsigned int stamps[MEM_AREAS];
static unsigned int tick;

/* The area holding `ctx`, or MEM_AREAS when none is loaded. */
static unsigned int area_of(void *ctx)
{
	int i;

	for (i = 0; i < (int)MEM_AREAS; ++i) {
		if (owners[i] == ctx) {
			return i;
		}
	}
	return MEM_AREAS;
}

/* A free area if one exists, else the least-recently used one;
 * claimed for `ctx`. */
static unsigned int claim(void *ctx)
{
	unsigned int victim = 0u;
	int i;

	for (i = 0; i < (int)MEM_AREAS; ++i) {
		if (owners[i] == (void *)0) {
			owners[i] = ctx;
			return (unsigned int)i;
		}
	}
	/* MEM_AREAS is 2: the second area wins the tie only when it is
	 * strictly older. */
	if (stamps[1] < stamps[0]) {
		victim = 1u;
	}
	owners[victim] = ctx;
	return victim;
}

static void touch(unsigned int area)
{
	stamps[area] = ++tick;
}

static int mem_load(void *ctx, uint32_t area_record)
{
	unsigned int area = area_of(ctx);
	int result;

	if (area == MEM_AREAS) {
		area = claim(ctx);
	} else {
		touch(area);
	}
	result = kres_io->seek(ctx, kres_record_offset(area_record));
	if (result < 0) {
		return result;
	}
	result = kres_io_blocks(ctx, area_record, storage[area],
				KRES_CATALOG_RECORDS, 0);
	if (result != (int)KRES_CATALOG_RECORDS) {
		return (result < 0) ? result : -KOS_EIO;
	}
	return 0;
}

static int mem_entry(void *ctx, unsigned int index, uint8_t *out)
{
	unsigned int area = area_of(ctx);

	if (area == MEM_AREAS || index >= KRES_RESOURCE_MAX) {
		return -KOS_EIO;
	}
	touch(area);
	memcpy(out, storage[area] + index * KRES_ENTRY_SIZE, KRES_ENTRY_SIZE);
	return 0;
}

static int mem_put(void *ctx, unsigned int index, const uint8_t *entry)
{
	unsigned int area = area_of(ctx);

	if (index >= KRES_RESOURCE_MAX) {
		return -KOS_EIO;
	}
	if (area == MEM_AREAS) {
		/* A put without a load starts an empty area (the sealed
		 * builder accumulates its catalog this way). */
		area = claim(ctx);
		memset(storage[area], 0,
		       KRES_CATALOG_RECORDS * KRES_RECORD_SIZE);
	} else {
		touch(area);
	}
	memcpy(storage[area] + index * KRES_ENTRY_SIZE, entry,
	       KRES_ENTRY_SIZE);
	return 0;
}

static int mem_store(void *ctx, uint32_t area_record)
{
	unsigned int area = area_of(ctx);
	int result;

	if (area == MEM_AREAS) {
		return -KOS_EIO;
	}
	touch(area);
	result = kres_io->seek(ctx, kres_record_offset(area_record));
	if (result < 0) {
		return result;
	}
	result = kres_io_blocks(ctx, area_record, storage[area],
				KRES_CATALOG_RECORDS, 1);
	if (result != (int)KRES_CATALOG_RECORDS) {
		return (result < 0) ? result : -KOS_EIO;
	}
	return 0;
}

static int mem_release(void *ctx)
{
	unsigned int area = area_of(ctx);

	if (area != MEM_AREAS) {
		owners[area] = (void *)0;
	}
	return 0;
}

const struct kres_cache kres_cache_mem = {
	mem_load,
	mem_entry,
	mem_put,
	mem_store,
	mem_release,
};
