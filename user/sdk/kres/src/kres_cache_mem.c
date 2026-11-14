/* KRES core: the memory catalog store (private; host edition).
 *
 * A writable container's catalog, or the sealed builder's, in host
 * heap memory: one growable array of 64-byte entries per store,
 * doubling as entries are inserted.  Compaction holds a writable
 * target while the builder may hold another, so the backend keeps two
 * stores, keyed by the ctx each was loaded for.  The KonaOS edition
 * installs its XMEM backend instead, so this member never links into a
 * target program.
 */

#include <stdlib.h>

#include "kres_internal.h"

#define MEM_STORES 2u

struct mem_store {
	void *ctx;
	uint8_t *data;
	uint32_t capacity; /* entries, a multiple of eight */
};

static struct mem_store stores[MEM_STORES];

static struct mem_store *store_of(void *ctx)
{
	unsigned int i;

	for (i = 0u; i < MEM_STORES; ++i) {
		if (stores[i].data != (uint8_t *)0 && stores[i].ctx == ctx) {
			return &stores[i];
		}
	}
	return (struct mem_store *)0;
}

/* Room for `entries`, rounded up to whole records. */
static int reserve(struct mem_store *s, uint32_t entries)
{
	uint32_t capacity = s->capacity != 0u ? s->capacity : 8u;
	uint8_t *grown;

	while (capacity < entries) {
		capacity *= 2u;
	}
	if (capacity == s->capacity) {
		return 0;
	}
	grown = (uint8_t *)realloc(s->data, (size_t)capacity *
					    KRES_ENTRY_SIZE);
	if (grown == (uint8_t *)0) {
		return -KOS_ENOSPC;
	}
	memset(grown + (size_t)s->capacity * KRES_ENTRY_SIZE, 0,
	       (size_t)(capacity - s->capacity) * KRES_ENTRY_SIZE);
	s->data = grown;
	s->capacity = capacity;
	return 0;
}

static int mem_load(void *ctx, uint16_t start, uint16_t count)
{
	struct mem_store *s = store_of(ctx);
	uint16_t fill = kres_catalog_fill(count);
	uint16_t i;
	int result;

	if (s == (struct mem_store *)0) {
		for (i = 0u; i < MEM_STORES; ++i) {
			if (stores[i].data == (uint8_t *)0) {
				s = &stores[i];
				break;
			}
		}
		if (s == (struct mem_store *)0) {
			return -KOS_EBUSY;
		}
		s->capacity = 0u;
	}
	s->ctx = ctx;
	result = reserve(s, (uint32_t)fill * KRES_ENTRIES_PER_RECORD + 1u);
	if (result < 0) {
		return result;
	}
	if (fill == 0u) {
		return 0;
	}
	result = kres_io->seek(ctx, kres_record_offset(start));
	if (result < 0) {
		return result;
	}
	result = kres_io_blocks(ctx, s->data, fill, 0);
	if (result != (int)fill) {
		return (result < 0) ? result : -KOS_EIO;
	}
	return 0;
}

static int mem_entry(void *ctx, uint16_t index, uint8_t *out)
{
	struct mem_store *s = store_of(ctx);

	if (s == (struct mem_store *)0 || index >= s->capacity) {
		return -KOS_EIO;
	}
	memcpy(out, s->data + (size_t)index * KRES_ENTRY_SIZE,
	       KRES_ENTRY_SIZE);
	return 0;
}

static int mem_put(void *ctx, uint16_t index, const uint8_t *entry)
{
	struct mem_store *s = store_of(ctx);

	if (s == (struct mem_store *)0 || index >= s->capacity) {
		return -KOS_EIO;
	}
	memcpy(s->data + (size_t)index * KRES_ENTRY_SIZE, entry,
	       KRES_ENTRY_SIZE);
	return 0;
}

static int mem_insert(void *ctx, uint16_t index, uint16_t count)
{
	struct mem_store *s = store_of(ctx);
	int result;

	if (s == (struct mem_store *)0 || index > count) {
		return -KOS_EIO;
	}
	result = reserve(s, (uint32_t)count + 1u);
	if (result < 0) {
		return result;
	}
	memmove(s->data + ((size_t)index + 1u) * KRES_ENTRY_SIZE,
		s->data + (size_t)index * KRES_ENTRY_SIZE,
		(size_t)(count - index) * KRES_ENTRY_SIZE);
	return 0;
}

static int mem_remove(void *ctx, uint16_t index, uint16_t count)
{
	struct mem_store *s = store_of(ctx);

	if (s == (struct mem_store *)0 || index >= count ||
	    count > s->capacity) {
		return -KOS_EIO;
	}
	memmove(s->data + (size_t)index * KRES_ENTRY_SIZE,
		s->data + ((size_t)index + 1u) * KRES_ENTRY_SIZE,
		(size_t)(count - index - 1u) * KRES_ENTRY_SIZE);
	return 0;
}

static int mem_store_out(void *ctx, uint16_t start, uint16_t count)
{
	struct mem_store *s = store_of(ctx);
	uint16_t fill = kres_catalog_fill(count);
	int result;

	if (s == (struct mem_store *)0 ||
	    (uint32_t)fill * KRES_ENTRIES_PER_RECORD > s->capacity) {
		return -KOS_EIO;
	}
	/* The final record's unused entries are zero. */
	memset(s->data + (size_t)count * KRES_ENTRY_SIZE, 0,
	       ((size_t)fill * KRES_ENTRIES_PER_RECORD - count) *
		       KRES_ENTRY_SIZE);
	result = kres_io->seek(ctx, kres_record_offset(start));
	if (result < 0) {
		return result;
	}
	result = kres_io_blocks(ctx, s->data, fill, 1);
	return result < 0 ? result : 0;
}

static int mem_release(void *ctx)
{
	struct mem_store *s = store_of(ctx);

	if (s != (struct mem_store *)0) {
		free(s->data);
		s->data = (uint8_t *)0;
		s->ctx = (void *)0;
		s->capacity = 0u;
	}
	return 0;
}

const struct kres_cache kres_cache_mem = {
	mem_load,
	mem_entry,
	mem_put,
	mem_insert,
	mem_remove,
	mem_store_out,
	mem_release,
};
