/* KRES: the KonaOS edition's catalog store in XMEM (libkres member).
 *
 * A writable container's catalog, or the sealed builder's, held in
 * XMEM: entries 0..255 live in chunk 0, 256..511 in chunk 1, and so
 * on.  An XMEM object holds at most 32 KiB, so a chunk is 16 KiB (256
 * entries); chunk 0 starts at 2 KiB (32 entries) and doubles in place
 * up to 16 KiB, so a small catalog costs a small object.  A store
 * holds up to 16 chunks (4,096 entries, 256 KiB of XMEM); beyond that,
 * or when XMEM is exhausted, a growth answers -KOS_ENOSPC and the
 * catalog is unchanged.  Two stores (a compaction's target and the
 * sealed builder, or two writable containers) keyed by ctx.
 *
 * The 512-byte shared buffer stages every move, so this member needs
 * no primary-space tables beyond the chunk handles.
 */

#include "kos.h"

#include "kos/kres.h"
#include "kos/kres_konaos.h"
#include "kos/errno.h"

#include "kres_internal.h"

#define XS_STORES        2u
#define XS_CHUNKS        16u
#define XS_CHUNK_ENTRIES 256u
#define XS_CHUNK_BYTES   16384u
#define XS_FIRST_BYTES   2048u

struct xstore {
	void *ctx;
	uint8_t open;
	uint8_t chunks;          /* live chunk objects */
	uint16_t first_bytes;    /* chunk 0's size */
	int handle[XS_CHUNKS];
};

static struct xstore xstores[XS_STORES];

static struct xstore *xs_of(void *ctx)
{
	uint8_t i;

	for (i = 0u; i < XS_STORES; ++i) {
		if (xstores[i].open != 0u && xstores[i].ctx == ctx) {
			return &xstores[i];
		}
	}
	return (struct xstore *)0;
}

static void xs_free(struct xstore *s)
{
	while (s->chunks != 0u) {
		--s->chunks;
		(void)xfree((unsigned int)s->handle[s->chunks]);
	}
	s->open = 0u;
	s->ctx = (void *)0;
}

/* The entries the store holds now. */
static uint16_t xs_capacity(const struct xstore *s)
{
	if (s->chunks <= 1u) {
		return (uint16_t)(s->first_bytes >> 6);
	}
	return (uint16_t)((uint16_t)s->chunks << 8);
}

/* Room for `entries` (up to 65,536, whole records). */
static int xs_reserve(struct xstore *s, uint32_t entries)
{
	int handle;

	/* Chunk 0 doubles in place while the catalog fits it. */
	while (s->chunks == 1u && s->first_bytes < XS_CHUNK_BYTES &&
	       (uint32_t)(s->first_bytes >> 6) < entries) {
		uint16_t bytes = (uint16_t)(s->first_bytes << 1);
		uint16_t at;

		handle = xalloc(bytes, KOS_XMEM_FLAG_NONE);
		if (handle < 0) {
			return -KOS_ENOSPC;
		}
		for (at = 0u; at < s->first_bytes; at += KRES_RECORD_SIZE) {
			if (xread((unsigned int)s->handle[0], at, kres_shared,
				  KRES_RECORD_SIZE) !=
				    (int)KRES_RECORD_SIZE ||
			    xwrite((unsigned int)handle, at, kres_shared,
				   KRES_RECORD_SIZE) !=
				    (int)KRES_RECORD_SIZE) {
				(void)xfree((unsigned int)handle);
				return -KOS_EIO;
			}
		}
		(void)xfree((unsigned int)s->handle[0]);
		s->handle[0] = handle;
		s->first_bytes = bytes;
	}
	/* Further chunks are whole. */
	while (s->chunks != 0u && (uint32_t)xs_capacity(s) < entries &&
	       s->first_bytes == XS_CHUNK_BYTES) {
		if (s->chunks == XS_CHUNKS) {
			return -KOS_ENOSPC;
		}
		handle = xalloc(XS_CHUNK_BYTES, KOS_XMEM_FLAG_NONE);
		if (handle < 0) {
			return -KOS_ENOSPC;
		}
		s->handle[s->chunks] = handle;
		++s->chunks;
	}
	return (uint32_t)xs_capacity(s) >= entries ? 0 : -KOS_ENOSPC;
}

/* Move `count` entries from entry `index` between the store and
 * `buffer`, split at chunk boundaries. */
static int xs_move(struct xstore *s, uint16_t index, uint8_t *buffer,
		   uint16_t count, uint8_t writing)
{
	while (count != 0u) {
		uint16_t within = (uint16_t)(index & (XS_CHUNK_ENTRIES - 1u));
		uint16_t n = (uint16_t)(XS_CHUNK_ENTRIES - within);
		unsigned int handle =
			(unsigned int)s->handle[(uint8_t)(index >> 8)];
		int result;

		if (n > count) {
			n = count;
		}
		if (writing != 0u) {
			result = xwrite(handle, (unsigned int)within << 6,
					buffer, (unsigned int)n << 6);
		} else {
			result = xread(handle, (unsigned int)within << 6,
				       buffer, (unsigned int)n << 6);
		}
		if (result != (int)((unsigned int)n << 6)) {
			return result < 0 ? result : -KOS_EIO;
		}
		buffer += (uint16_t)(n << 6);
		index = (uint16_t)(index + n);
		count = (uint16_t)(count - n);
	}
	return 0;
}

static int xmem_load(void *ctx, uint16_t start, uint16_t count)
{
	struct xstore *s = xs_of(ctx);
	uint16_t fill = kres_catalog_fill(count);
	uint16_t record;
	int result;

	if (s != (struct xstore *)0) {
		xs_free(s);
	}
	for (record = 0u; record < XS_STORES; ++record) {
		if (xstores[record].open == 0u) {
			s = &xstores[record];
			break;
		}
	}
	if (s == (struct xstore *)0 || s->open != 0u) {
		return -KOS_EBUSY;
	}
	result = xalloc(XS_FIRST_BYTES, KOS_XMEM_FLAG_NONE);
	if (result < 0) {
		return -KOS_ENOSPC;
	}
	s->handle[0] = result;
	s->chunks = 1u;
	s->first_bytes = XS_FIRST_BYTES;
	s->ctx = ctx;
	s->open = 1u;
	result = xs_reserve(s, (uint32_t)fill * KRES_ENTRIES_PER_RECORD);
	for (record = 0u; result == 0 && record < fill; ++record) {
		result = kres_io->seek(ctx, kres_record_offset(
					(uint32_t)start + record));
		if (result == 0 &&
		    kres_io->read(ctx, kres_shared, KRES_RECORD_SIZE) !=
			    (int)KRES_RECORD_SIZE) {
			result = -KOS_EIO;
		}
		if (result == 0) {
			result = xs_move(s, (uint16_t)(record << 3),
					 kres_shared,
					 KRES_ENTRIES_PER_RECORD, 1u);
		}
	}
	if (result < 0) {
		xs_free(s); /* the error path after the allocation frees it */
	}
	return result;
}

static int xmem_entry(void *ctx, uint16_t index, uint8_t *out)
{
	struct xstore *s = xs_of(ctx);

	if (s == (struct xstore *)0 || index >= xs_capacity(s)) {
		return -KOS_EIO;
	}
	return xs_move(s, index, out, 1u, 0u);
}

static int xmem_put(void *ctx, uint16_t index, const uint8_t *entry)
{
	struct xstore *s = xs_of(ctx);

	if (s == (struct xstore *)0 || index >= xs_capacity(s)) {
		return -KOS_EIO;
	}
	return xs_move(s, index, (uint8_t *)entry, 1u, 1u);
}

/* Shift entries [index, count) up by one, top first, eight at a
 * time through the shared buffer. */
static int xmem_insert(void *ctx, uint16_t index, uint16_t count)
{
	struct xstore *s = xs_of(ctx);
	uint16_t high = count;
	int result;

	if (s == (struct xstore *)0 || index > count) {
		return -KOS_EIO;
	}
	result = xs_reserve(s, (uint32_t)count + 1u);
	if (result < 0) {
		return result;
	}
	while (high > index) {
		uint16_t n = (uint16_t)(high - index);
		uint16_t low;

		if (n > KRES_ENTRIES_PER_RECORD) {
			n = KRES_ENTRIES_PER_RECORD;
		}
		low = (uint16_t)(high - n);
		result = xs_move(s, low, kres_shared, n, 0u);
		if (result == 0) {
			result = xs_move(s, (uint16_t)(low + 1u), kres_shared,
					 n, 1u);
		}
		if (result < 0) {
			return result;
		}
		high = low;
	}
	return 0;
}

/* Shift entries (index, count) down by one, bottom first. */
static int xmem_remove(void *ctx, uint16_t index, uint16_t count)
{
	struct xstore *s = xs_of(ctx);
	uint16_t low = (uint16_t)(index + 1u);
	int result;

	if (s == (struct xstore *)0 || index >= count) {
		return -KOS_EIO;
	}
	while (low < count) {
		uint16_t n = (uint16_t)(count - low);

		if (n > KRES_ENTRIES_PER_RECORD) {
			n = KRES_ENTRIES_PER_RECORD;
		}
		result = xs_move(s, low, kres_shared, n, 0u);
		if (result == 0) {
			result = xs_move(s, (uint16_t)(low - 1u), kres_shared,
					 n, 1u);
		}
		if (result < 0) {
			return result;
		}
		low = (uint16_t)(low + n);
	}
	return 0;
}

static int xmem_store(void *ctx, uint16_t start, uint16_t count)
{
	struct xstore *s = xs_of(ctx);
	uint16_t done = 0u;
	uint16_t record = start;
	int result;

	if (s == (struct xstore *)0) {
		return -KOS_EIO;
	}
	while (done < count) {
		uint16_t n = (uint16_t)(count - done);

		if (n > KRES_ENTRIES_PER_RECORD) {
			n = KRES_ENTRIES_PER_RECORD;
		}
		result = xs_move(s, done, kres_shared, n, 0u);
		if (result < 0) {
			return result;
		}
		if (n < KRES_ENTRIES_PER_RECORD) {
			memset(kres_shared + ((uint16_t)n << 6), 0,
			       KRES_RECORD_SIZE - ((uint16_t)n << 6));
		}
		result = kres_io->seek(ctx, kres_record_offset(record));
		if (result < 0) {
			return result;
		}
		result = kres_io->write(ctx, kres_shared, KRES_RECORD_SIZE);
		if (result != (int)KRES_RECORD_SIZE) {
			return result < 0 ? result : -KOS_EIO;
		}
		done = (uint16_t)(done + n);
		++record;
	}
	return 0;
}

static int xmem_release(void *ctx)
{
	struct xstore *s = xs_of(ctx);

	if (s != (struct xstore *)0) {
		xs_free(s);
	}
	return 0;
}

const struct kres_cache kres_cache_xmem = {
	xmem_load,
	xmem_entry,
	xmem_put,
	xmem_insert,
	xmem_remove,
	xmem_store,
	xmem_release,
};
