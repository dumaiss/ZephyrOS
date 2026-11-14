/* KRES core: mutation and the commit machinery (private).
 *
 * kres_create_container, kres_create, kres_write, kres_write_records,
 * kres_resize, kres_reserve, kres_delete, kres_rename and kres_flush
 * (the §5.6 commit order).  The writable catalog lives in the catalog
 * store, sorted by name; payload bytes are written through; a flush
 * writes the whole catalog into the inactive catalog area (relocating
 * that area to the allocation mark when it is too small) and then the
 * header, the container's single commit point.
 */

#include "kres_internal.h"

/* A whole zero record, in const storage (ROM on the target): the
 * file-extension writes use it, so extending never disturbs whatever
 * the caller staged in the shared buffer.  Defined here, in the
 * mutation member, so a read-only program does not link it. */
const uint8_t kres_zero_page[KRES_RECORD_SIZE];

/* The growth policy's floor (design §5.3: the `minimum` term). */
#define KRES_GROW_MIN 4u

/* The mutation-capable install: kres_install plus the catalog store
 * and the read path's mutation-side hooks.  A program that never
 * mutates calls plain kres_install and links none of this member's
 * machinery. */
int kres_install_mut(const struct kres_io *io,
		     const struct kres_cache *cache, void *ctx)
{
	kres_install(io, ctx);
	kres_cache = cache;
	kres_create_hook = kres_create_container;
	kres_flush_hook = kres_flush;
	return 0;
}

void kres_header_emit(const kres_container *c, uint8_t *header)
{
	memset(header, 0, KRES_RECORD_SIZE);
	memcpy(header + KRES_HDR_MAGIC, "KRES", 4);
	kres_put16(header + KRES_HDR_MAJOR, KRES_HDR_MAJOR_VALUE);
	kres_put16(header + KRES_HDR_MINOR, KRES_HDR_MINOR_VALUE);
	kres_put16(header + KRES_HDR_RECORD_SIZE, KRES_RECORD_SIZE);
	kres_put32(header + KRES_HDR_GENERATION, c->generation);
	header[KRES_HDR_ACTIVE_AREA] = c->active_area;
	kres_put16(header + KRES_HDR_RES_COUNT, c->resource_count);
	kres_put32(header + KRES_HDR_NEXT_RECORD, c->next_record);
	kres_put32(header + KRES_HDR_FLAGS, c->sealed != 0u
		       ? KRES_HDR_FLAG_SEALED : 0u);
	kres_put32(header + KRES_HDR_CAT_A_START, c->catalog_start[0]);
	kres_put32(header + KRES_HDR_CAT_A_RECS, c->catalog_records[0]);
	kres_put32(header + KRES_HDR_CAT_B_START, c->catalog_start[1]);
	kres_put32(header + KRES_HDR_CAT_B_RECS, c->catalog_records[1]);
	kres_put16(header + KRES_HDR_CHECK, kres_header_check(header));
}

/* Write the header record from `c`'s state and sync: the commit. */
static int header_commit(void *ctx, const kres_container *c)
{
	int result;

	kres_header_emit(c, kres_shared);
	result = kres_io->seek(ctx, 0u);
	if (result < 0) {
		return result;
	}
	result = kres_io->write(ctx, kres_shared, KRES_RECORD_SIZE);
	if (result < 0) {
		return result;
	}
	if (result != (int)KRES_RECORD_SIZE) {
		return -KOS_EIO;
	}
	return kres_io->sync(ctx);
}

/* Write `count` zero records at `record` (creation): one record at a
 * time, from the const zero record. */
static int zero_records(void *ctx, uint32_t record, uint32_t count)
{
	uint32_t i;
	int result;

	for (i = 0u; i < count; ++i) {
		result = kres_io->seek(ctx, kres_record_offset(record + i));
		if (result < 0) {
			return result;
		}
		result = kres_io->write(ctx, kres_zero_page, KRES_RECORD_SIZE);
		if (result < 0) {
			return result;
		}
		if (result != (int)KRES_RECORD_SIZE) {
			return -KOS_EIO;
		}
	}
	return 0;
}

/* The container creation: zero catalog areas A and B of
 * `catalog_records` records each, then the header, written last. */
static int create_empty(void *ctx, uint16_t catalog_records)
{
	kres_container state;
	int result;

	if (catalog_records == 0u ||
	    catalog_records > (KRES_RECORD_LIMIT - 1u) / 2u) {
		return -KOS_EINVAL;
	}
	memset(&state, 0, sizeof(state));
	state.catalog_start[0] = 1u;
	state.catalog_records[0] = catalog_records;
	state.catalog_start[1] = (uint16_t)(1u + catalog_records);
	state.catalog_records[1] = catalog_records;
	state.next_record = 1u + 2u * (uint32_t)catalog_records;
	result = kres_io->truncate(ctx, 0u);
	if (result < 0) {
		return result;
	}
	result = zero_records(ctx, 1u, 2u * (uint32_t)catalog_records);
	if (result < 0) {
		return result;
	}
	return header_commit(ctx, &state);
}

int kres_create_sized(const char *path, uint16_t catalog_records)
{
	int result;

	if (kres_io == (const struct kres_io *)0 || path == (const char *)0) {
		return -KOS_EINVAL;
	}
	result = kres_io->open(kres_ctx, path,
			       KRES_OPEN_WRITE | KRES_OPEN_CREATE);
	if (result < 0) {
		return result;
	}
	result = create_empty(kres_ctx, catalog_records);
	if (result == 0) {
		result = kres_io->close(kres_ctx);
	} else {
		(void)kres_io->close(kres_ctx);
	}
	return result;
}

int kres_create_container(const char *path)
{
	return kres_create_sized(path, 1u);
}

/* --- catalog edits ------------------------------------------------------ */

/* The entry index of `r` now: its position at `r->epoch`, or, after
 * entries moved (create, delete, rename), found again by its start
 * record - live extents never share one. */
static int resource_index(kres_resource *r, uint16_t *out)
{
	kres_container *c = r->container;

	if (r->epoch != c->epoch) {
		uint8_t entry[KRES_ENTRY_SIZE];
		uint16_t i;

		for (i = 0u; i < c->resource_count; ++i) {
			if (kres_cache->entry(c, i, entry) < 0) {
				return -KOS_EIO;
			}
			if (kres_get32(entry + KRES_ENT_START) ==
			    r->start_record) {
				break;
			}
		}
		if (i == c->resource_count) {
			return -KOS_ENOENT; /* deleted under the descriptor */
		}
		r->entry_index = i;
		r->epoch = c->epoch;
	}
	*out = r->entry_index;
	return 0;
}

/* Rewrite `r`'s catalog entry from its descriptor state. */
static int resource_store(kres_resource *r)
{
	uint8_t entry[KRES_ENTRY_SIZE];
	uint16_t index;
	int result = resource_index(r, &index);

	if (result < 0) {
		return result;
	}
	result = kres_cache->entry(r->container, index, entry);
	if (result < 0) {
		return result;
	}
	kres_put32(entry + KRES_ENT_START, r->start_record);
	kres_put32(entry + KRES_ENT_ALLOCATED, r->allocated_records);
	kres_put32(entry + KRES_ENT_USED, r->used_bytes);
	r->container->dirty = 1u;
	return kres_cache->put(r->container, index, entry);
}

/* --- allocation --------------------------------------------------------- */

/* The backing file only grows here: `end` bytes are made available by
 * writing zero records at the physical end (design §5.1 — `truncate`
 * never grows a file, the KonaOS provider would extend it with
 * undefined contents).  The callers overwrite the bytes they are about
 * to write. */
static int extend_file(kres_container *c, uint32_t end)
{
	uint32_t at = c->file_size;
	int result;

	if (end <= at) {
		return 0;
	}
	while (at < end) {
		uint32_t chunk = end - at;

		if (chunk > KRES_RECORD_SIZE) {
			chunk = KRES_RECORD_SIZE;
		}
		result = kres_io->seek(c->io_ctx, at);
		if (result < 0) {
			return result;
		}
		result = kres_io->write(c->io_ctx, kres_zero_page, chunk);
		if (result < 0) {
			return result;
		}
		if (result != (int)chunk) {
			return -KOS_EIO;
		}
		at = (uint32_t)(at + chunk);
	}
	c->file_size = end;
	return 0;
}

/* The policy capacity: max(required, old + ceil(old / 2), minimum). */
static uint32_t grown_capacity(uint32_t old_capacity, uint32_t required)
{
	uint32_t grown = old_capacity + (old_capacity + 1u) / 2u;
	uint32_t capacity = (grown > required) ? grown : required;

	if (capacity < KRES_GROW_MIN) {
		capacity = KRES_GROW_MIN;
	}
	return capacity;
}

/* Allocate `count` records at `next_record`, advancing the mark.
 * Refuses the 65,535-record limit. */
static int allocate_records(kres_container *c, uint32_t count,
			    uint32_t *out)
{
	if (count > KRES_RECORD_LIMIT - c->next_record) {
		return -KRES_E_RECLIM;
	}
	*out = c->next_record;
	c->next_record = (uint32_t)(c->next_record + count);
	c->dirty = 1u;
	return 0;
}

/* Move a resource's live bytes from `source` into the new extent at
 * `new_start`, zero-filling the newly exposed tail up to `new_used`. */
static int copy_extent(kres_container *c, uint32_t source,
		       uint32_t old_used, uint32_t new_start,
		       uint32_t new_used)
{
	uint32_t at = 0u;
	int result;

	result = extend_file(c, new_start * KRES_RECORD_SIZE + new_used);
	if (result < 0) {
		return result;
	}
	while (at < old_used) {
		uint16_t chunk = (uint16_t)(old_used - at);

		if (chunk > KRES_RECORD_SIZE) {
			chunk = KRES_RECORD_SIZE;
		}
		if (kres_io->seek(c->io_ctx, source * KRES_RECORD_SIZE +
				  at) < 0 ||
		    kres_io->read(c->io_ctx, kres_shared, chunk) !=
			    (int)chunk) {
			return -KOS_EIO;
		}
		if (kres_io->seek(c->io_ctx, new_start * KRES_RECORD_SIZE +
				  at) < 0 ||
		    kres_io->write(c->io_ctx, kres_shared, chunk) !=
			    (int)chunk) {
			return -KOS_EIO;
		}
		at = (uint32_t)(at + chunk);
	}
	/* The zero-filled tail. */
	while (at < new_used) {
		uint16_t chunk = (uint16_t)(new_used - at);

		if (chunk > KRES_RECORD_SIZE) {
			chunk = KRES_RECORD_SIZE;
		}
		memset(kres_shared, 0, KRES_RECORD_SIZE);
		if (kres_io->seek(c->io_ctx, new_start * KRES_RECORD_SIZE +
				  at) < 0 ||
		    kres_io->write(c->io_ctx, kres_shared, chunk) !=
			    (int)chunk) {
			return -KOS_EIO;
		}
		at = (uint32_t)(at + chunk);
	}
	return 0;
}

/* Relocate a resource into a fresh extent of `new_capacity` records
 * (at least ceil(new_used / 512)); the old extent is abandoned and the
 * in-memory state and the cached entry move. */
static int relocate(kres_container *c, kres_resource *r,
		    uint32_t new_capacity, uint32_t new_used)
{
	uint32_t new_start;
	uint16_t index;
	int result;

	/* Pin the entry first: it is found by the old start record. */
	result = resource_index(r, &index);
	if (result < 0) {
		return result;
	}
	result = allocate_records(c, new_capacity, &new_start);
	if (result < 0) {
		return result;
	}
	result = copy_extent(c, r->start_record, r->used_bytes, new_start,
			     new_used);
	if (result < 0) {
		return result;
	}
	r->start_record = new_start;
	r->allocated_records = new_capacity;
	r->used_bytes = new_used;
	return resource_store(r);
}

/* --- resources ---------------------------------------------------------- */

int kres_create(kres_container *c, const char *name,
		uint16_t capacity_records, kres_resource *r)
{
	uint8_t entry[KRES_ENTRY_SIZE];
	uint16_t name_length;
	uint16_t pos;
	uint32_t start;
	int result;

	if (c == (kres_container *)0 || c->opened == 0u) {
		return -KOS_EINVAL;
	}
	if (c->sealed != 0u || c->writable == 0u) {
		return c->sealed != 0u ? -KOS_EROFS : -KOS_EINVAL;
	}
	name_length = kres_name_check(name);
	if (name_length == 0xFFFFu || capacity_records == 0u) {
		return -KOS_EINVAL;
	}
	if (c->resource_count >= KRES_RESOURCE_MAX) {
		return -KRES_E_FULL;
	}
	result = kres_search(c, name, (uint8_t)name_length, &pos);
	if (result != 0) {
		return result > 0 ? -KOS_EEXIST : result;
	}
	result = allocate_records(c, capacity_records, &start);
	if (result < 0) {
		return result;
	}
	/* The sorted insertion: the later entries shift up one. */
	result = kres_cache->insert(c, pos, c->resource_count);
	if (result == 0) {
		kres_entry_emit(entry, (const uint8_t *)name,
				(uint8_t)name_length, start, capacity_records,
				0u);
		result = kres_cache->put(c, pos, entry);
		if (result < 0) {
			(void)kres_cache->remove(c, pos,
				(uint16_t)(c->resource_count + 1u));
		}
	}
	if (result < 0) {
		c->next_record = start; /* the allocation is undone */
		return result;
	}
	c->resource_count = (uint16_t)(c->resource_count + 1u);
	c->epoch = (uint16_t)(c->epoch + 1u);
	c->dirty = 1u;
	if (r != (kres_resource *)0) {
		return resource_fill(c, pos, r);
	}
	return 0;
}

/* One write's bounds and capacity: a write at the cursor, which must
 * not sit past the logical end; growth relocates when the bytes leave
 * the allocation. */
static int write_ensure(kres_resource *r, uint32_t end)
{
	kres_container *c = r->container;
	uint32_t capacity_bytes = r->allocated_records * KRES_RECORD_SIZE;

	if (end > capacity_bytes) {
		uint32_t required = (end + KRES_RECORD_SIZE - 1u) /
				    KRES_RECORD_SIZE;
		uint32_t capacity = grown_capacity(r->allocated_records,
						   required);

		return relocate(c, r, capacity, end);
	}
	return 0;
}

int kres_write(kres_resource *r, const void *src, uint16_t len)
{
	uint32_t at;
	uint32_t end;
	uint16_t done = 0u;
	int result;

	if (r == (kres_resource *)0 || r->open == 0u ||
	    r->writable == 0u || src == (const void *)0 || len == 0u) {
		return -KOS_EINVAL;
	}
	if (r->cursor > r->used_bytes) {
		return -KOS_EINVAL; /* a write past the logical EOF */
	}
	end = (uint32_t)(r->cursor + len);
	if (end > r->used_bytes) {
		result = write_ensure(r, end);
		if (result < 0) {
			return result;
		}
		result = extend_file(r->container,
				     r->start_record * KRES_RECORD_SIZE +
					     end);
		if (result < 0) {
			return result;
		}
	}
	at = r->start_record * KRES_RECORD_SIZE + r->cursor;
	while (done < len) {
		uint16_t chunk = (uint16_t)(KRES_RECORD_SIZE -
			 (at & (uint32_t)(KRES_RECORD_SIZE - 1u)));

		if (chunk > len - done) {
			chunk = (uint16_t)(len - done);
		}
		result = kres_io->seek(r->container->io_ctx, at);
		if (result < 0) {
			return result;
		}
		memmove(kres_shared, (const uint8_t *)src + done, chunk);
		result = kres_io->write(r->container->io_ctx, kres_shared,
					chunk);
		if (result < 0) {
			return result;
		}
		if (result != (int)chunk) {
			return -KOS_EIO;
		}
		done = (uint16_t)(done + chunk);
		at = (uint32_t)(at + chunk);
	}
	r->cursor = (uint32_t)(r->cursor + len);
	if (r->cursor > r->used_bytes) {
		r->used_bytes = r->cursor;
		result = resource_store(r);
		if (result < 0) {
			return result;
		}
	}
	return len;
}

int kres_write_records(kres_resource *r, uint16_t first_record,
		       uint16_t record_count, const void *src)
{
	uint16_t count = 0u;

	if (r == (kres_resource *)0 || r->open == 0u ||
	    r->writable == 0u || src == (const void *)0) {
		return -KOS_EINVAL;
	}
	while (count < record_count) {
		uint32_t record = (uint32_t)first_record + count;
		uint32_t record_begin = record * KRES_RECORD_SIZE;
		uint32_t end = record_begin + KRES_RECORD_SIZE;
		int result;

		if (record > KRES_RECORD_LIMIT) {
			return -KRES_E_RECLIM;
		}
		result = write_ensure(r, end);
		if (result < 0) {
			return result;
		}
		if (end > r->used_bytes) {
			r->used_bytes = end;
		}
		result = extend_file(r->container,
				     r->start_record * KRES_RECORD_SIZE +
					     end);
		if (result < 0) {
			return result;
		}
		result = kres_io->seek(r->container->io_ctx,
				       (uint32_t)(r->start_record + record) *
				       KRES_RECORD_SIZE);
		if (result < 0) {
			return result;
		}
		result = kres_io->write(r->container->io_ctx,
					(const uint8_t *)src +
					(uint32_t)count * KRES_RECORD_SIZE,
					KRES_RECORD_SIZE);
		if (result < 0) {
			return result;
		}
		if (result != (int)KRES_RECORD_SIZE) {
			return -KOS_EIO;
		}
		++count;
	}
	{
		int result = resource_store(r);

		if (result < 0) {
			return result;
		}
	}
	return (int)record_count * KRES_RECORD_SIZE;
}

int kres_resize(kres_resource *r, uint32_t used_bytes)
{
	uint32_t capacity_bytes;
	int result;

	if (r == (kres_resource *)0 || r->open == 0u ||
	    r->writable == 0u) {
		return -KOS_EINVAL;
	}
	capacity_bytes = r->allocated_records * KRES_RECORD_SIZE;
	if (used_bytes > capacity_bytes) {
		uint32_t required = (used_bytes + KRES_RECORD_SIZE - 1u) /
				    KRES_RECORD_SIZE;

		result = relocate(r->container, r, required, used_bytes);
		if (result < 0) {
			return result;
		}
	} else if (used_bytes > r->used_bytes) {
		/* Zero-fill the newly exposed bytes in place. */
		uint32_t at = r->used_bytes;
		int result;

		result = extend_file(r->container,
				     r->start_record * KRES_RECORD_SIZE +
					     used_bytes);
		if (result < 0) {
			return result;
		}
		while (at < used_bytes) {
			uint16_t chunk = (uint16_t)(used_bytes - at);

			if (chunk > KRES_RECORD_SIZE) {
				chunk = KRES_RECORD_SIZE;
			}
			memset(kres_shared, 0, KRES_RECORD_SIZE);
			result = kres_io->seek(r->container->io_ctx,
				r->start_record * KRES_RECORD_SIZE + at);
			if (result < 0) {
				return result;
			}
			result = kres_io->write(r->container->io_ctx,
						kres_shared, chunk);
			if (result != (int)chunk) {
				return -KOS_EIO;
			}
			at = (uint32_t)(at + chunk);
		}
	}
	r->used_bytes = used_bytes;
	if (r->cursor > used_bytes) {
		r->cursor = used_bytes;
	}
	return resource_store(r);
}

int kres_reserve(kres_resource *r, uint16_t capacity_records)
{
	if (r == (kres_resource *)0 || r->open == 0u ||
	    r->writable == 0u) {
		return -KOS_EINVAL;
	}
	if (capacity_records <= r->allocated_records) {
		return 0; /* never shrinks */
	}
	return relocate(r->container, r, capacity_records, r->used_bytes);
}

int kres_delete(kres_container *c, const char *name)
{
	uint16_t name_length;
	uint16_t index;
	int result;

	if (c == (kres_container *)0 || c->opened == 0u) {
		return -KOS_EINVAL;
	}
	if (c->sealed != 0u || c->writable == 0u) {
		return c->sealed != 0u ? -KOS_EROFS : -KOS_EINVAL;
	}
	name_length = kres_name_check(name);
	if (name_length == 0xFFFFu) {
		return -KOS_EINVAL;
	}
	result = kres_search(c, name, (uint8_t)name_length, &index);
	if (result != 1) {
		return result == 0 ? -KOS_ENOENT : result;
	}
	result = kres_cache->remove(c, index, c->resource_count);
	if (result < 0) {
		return result;
	}
	c->resource_count = (uint16_t)(c->resource_count - 1u);
	c->epoch = (uint16_t)(c->epoch + 1u);
	c->dirty = 1u;
	return 0;
}

/* A rename moves the entry to the new name's sorted position. */
int kres_rename(kres_container *c, const char *old_name,
		const char *new_name)
{
	uint8_t entry[KRES_ENTRY_SIZE];
	uint16_t old_length;
	uint16_t new_length;
	uint16_t index;
	uint16_t pos;
	int result;

	if (c == (kres_container *)0 || c->opened == 0u) {
		return -KOS_EINVAL;
	}
	if (c->sealed != 0u || c->writable == 0u) {
		return c->sealed != 0u ? -KOS_EROFS : -KOS_EINVAL;
	}
	old_length = kres_name_check(old_name);
	new_length = kres_name_check(new_name);
	if (old_length == 0xFFFFu || new_length == 0xFFFFu) {
		return -KOS_EINVAL;
	}
	result = kres_search(c, old_name, (uint8_t)old_length, &index);
	if (result != 1) {
		return result == 0 ? -KOS_ENOENT : result;
	}
	result = kres_search(c, new_name, (uint8_t)new_length, &pos);
	if (result != 0) {
		return result > 0 ? -KOS_EEXIST : result;
	}
	result = kres_cache->entry(c, index, entry);
	if (result < 0) {
		return result;
	}
	memset(entry + KRES_ENT_NAME, 0, KRES_NAME_MAX);
	memcpy(entry + KRES_ENT_NAME, new_name, new_length);
	entry[KRES_ENT_NAME_LENGTH] = (uint8_t)new_length;
	/* `pos` is the insertion point among all entries, the renamed
	 * one included: index or index + 1 keeps the slot. */
	if (pos != index && pos != (uint16_t)(index + 1u)) {
		result = kres_cache->remove(c, index, c->resource_count);
		if (result < 0) {
			return result;
		}
		if (pos > index) {
			--pos;
		}
		result = kres_cache->insert(c, pos,
					    (uint16_t)(c->resource_count - 1u));
		if (result < 0) {
			return result;
		}
		index = pos;
	}
	result = kres_cache->put(c, index, entry);
	if (result < 0) {
		return result;
	}
	c->epoch = (uint16_t)(c->epoch + 1u);
	c->dirty = 1u;
	return 0;
}

/* --- the commit --------------------------------------------------------- */

/* The capacity of a relocated catalog area: the larger area's
 * capacity doubled until `fill` records fit - or exactly `fill` when
 * the doubled extent would pass the record limit. */
static uint16_t catalog_grown(const kres_container *c, uint16_t fill)
{
	uint16_t capacity = c->catalog_records[0];

	if (c->catalog_records[1] > capacity) {
		capacity = c->catalog_records[1];
	}
	if (capacity == 0u) {
		capacity = 1u;
	}
	while (capacity < fill) {
		if (capacity > 0x7FFFu) {
			capacity = fill;
			break;
		}
		capacity = (uint16_t)(capacity << 1);
	}
	if (capacity > KRES_RECORD_LIMIT - c->next_record) {
		capacity = fill;
	}
	return capacity;
}

int kres_flush(kres_container *c)
{
	kres_container next;
	uint32_t catalog_end;
	uint16_t fill;
	uint8_t inactive;
	int result;

	if (c == (kres_container *)0 || c->opened == 0u ||
	    c->writable == 0u) {
		return -KOS_EINVAL;
	}
	if (c->sealed != 0u) {
		return -KOS_EROFS;
	}
	if (c->dirty == 0u) {
		return kres_io->sync(c->io_ctx);
	}
	/* Step 1: the payload bytes are written through already. */
	result = kres_io->sync(c->io_ctx);
	if (result < 0) {
		return result;
	}
	/* Step 2: the whole sorted catalog into the inactive area -
	 * relocated to the allocation mark when it is too small.  The
	 * committed state changes only in `next`, so a failed flush
	 * leaves `c` as it was and a retry repeats the same steps. */
	next = *c;
	inactive = (uint8_t)(1u - c->active_area);
	fill = kres_catalog_fill(c->resource_count);
	if (fill > c->catalog_records[inactive]) {
		uint16_t capacity = catalog_grown(c, fill);

		if (fill > KRES_RECORD_LIMIT - c->next_record) {
			return -KRES_E_RECLIM;
		}
		result = extend_file(c, kres_record_offset(c->next_record));
		if (result < 0) {
			return result;
		}
		next.catalog_start[inactive] = (uint16_t)c->next_record;
		next.catalog_records[inactive] = capacity;
		next.next_record = c->next_record + capacity;
	}
	if (fill != 0u) {
		result = kres_cache->store(c, next.catalog_start[inactive],
					   c->resource_count);
		if (result < 0) {
			return result;
		}
		catalog_end = kres_record_offset(
			(uint32_t)next.catalog_start[inactive] + fill);
		if (catalog_end > c->file_size) {
			c->file_size = catalog_end;
		}
	}
	result = kres_io->sync(c->io_ctx);
	if (result < 0) {
		return result;
	}
	/* Step 3: the header, with the area switched and the generation
	 * advanced; the single commit point. */
	next.active_area = inactive;
	next.generation = (uint32_t)(c->generation + 1u);
	next.file_size = c->file_size;
	result = header_commit(c->io_ctx, &next);
	if (result < 0) {
		return result;
	}
	next.dirty = 0u;
	*c = next;
	return 0;
}
