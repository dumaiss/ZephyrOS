/* KRES core: mutation and the commit machinery (private).
 *
 * kres_create_container, kres_create, kres_write, kres_write_records,
 * kres_resize, kres_reserve, kres_delete, kres_rename and kres_flush
 * (the §5.6 commit order).  Payload bytes are written through; a flush
 * commits the catalog area and the header, which is the container's
 * single commit point.
 */

#include "kres_internal.h"

/* The growth policy's floor (design §5.3: the `minimum` term). */
#define KRES_GROW_MIN 4u

/* An empty mutable container's physical size: header, both catalog
 * areas, no data records. */
/* Build the header bytes for `c`'s current state. */
static void header_emit(kres_container *c, uint8_t *header)
{
	memset(header, 0, KRES_RECORD_SIZE);
	memcpy(header + KRES_HDR_MAGIC, "KRES", 4);
	kres_put16(header + KRES_HDR_MAJOR, KRES_HDR_MAJOR_V1);
	kres_put16(header + KRES_HDR_MINOR, KRES_HDR_MINOR_V1);
	kres_put16(header + KRES_HDR_RECORD_SIZE, KRES_RECORD_SIZE);
	kres_put16(header + KRES_HDR_CATALOG_RECS, KRES_CATALOG_RECORDS);
	kres_put32(header + KRES_HDR_GENERATION, c->generation);
	header[KRES_HDR_ACTIVE_AREA] = c->active_area;
	kres_put16(header + KRES_HDR_RES_COUNT, c->resource_count);
	kres_put32(header + KRES_HDR_NEXT_RECORD, c->next_record);
	kres_put32(header + KRES_HDR_FLAGS, c->sealed != 0u
		       ? KRES_HDR_FLAG_SEALED : 0u);
}

/* Write `count` zero records at `record` (creation and zero-fill):
 * one record at a time, from the single-record shared buffer. */
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

/* The container creation: header, zero catalog areas A and B, header
 * written last. */
static int create_empty(void *ctx)
{
	uint8_t header[KRES_RECORD_SIZE];
	kres_container state;
	int result;

	memset(&state, 0, sizeof(state));
	state.generation = 0u;
	state.next_record = KRES_DATA_BASE_MUT;
	state.resource_count = 0u;
	state.active_area = 0u;
	result = kres_io->truncate(ctx, 0u);
	if (result < 0) {
		return result;
	}
	result = zero_records(ctx, KRES_AREA_A_BASE,
			      2u * KRES_CATALOG_RECORDS);
	if (result < 0) {
		return result;
	}
	header_emit(&state, header);
	result = kres_io->seek(ctx, 0u);
	if (result < 0) {
		return result;
	}
	result = kres_io->write(ctx, header, KRES_RECORD_SIZE);
	if (result < 0) {
		return result;
	}
	if (result != (int)KRES_RECORD_SIZE) {
		return -KOS_EIO;
	}
	return kres_io->sync(ctx);
}

int kres_create_container(const char *path)
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
	result = create_empty(kres_ctx);
	if (result == 0) {
		result = kres_io->close(kres_ctx);
	} else {
		(void)kres_io->close(kres_ctx);
	}
	return result;
}

/* --- catalog edits ------------------------------------------------------ */

/* Append one entry (the capacity checks are the caller's). */
static void entry_append(kres_container *c, const uint8_t *name,
			 uint8_t name_length, uint32_t start,
			 uint32_t allocated, uint32_t used)
{
	uint8_t entry[KRES_ENTRY_SIZE];

	kres_entry_emit(entry, name, name_length, start, allocated, used);
	(void)kres_cache->put(c, c->resource_count, entry);
	c->resource_count = (uint16_t)(c->resource_count + 1u);
	c->dirty = 1u;
}

/* Drop entry `index`, shifting the denser suffix down. */
static void entry_remove(kres_container *c, unsigned int index)
{
	unsigned int i;

	for (i = index; i + 1u < c->resource_count; ++i) {
		uint8_t entry[KRES_ENTRY_SIZE];

		if (kres_entry_get(c, i + 1u, entry) < 0) {
			break;
		}
		(void)kres_cache->put(c, i, entry);
	}
	c->resource_count = (uint16_t)(c->resource_count - 1u);
	c->dirty = 1u;
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
	uint8_t entry[KRES_ENTRY_SIZE];
	int result;

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
	{
		uint8_t current[KRES_ENTRY_SIZE];

		if (kres_entry_get(c, r->entry_index, current) < 0) {
			return -KOS_EIO;
		}
		kres_entry_emit(entry, current + KRES_ENT_NAME,
				current[KRES_ENT_NAME_LENGTH], new_start,
				new_capacity, new_used);
	}
	(void)kres_cache->put(c, r->entry_index, entry);
	return 0;
}

/* --- resources ---------------------------------------------------------- */

int kres_create(kres_container *c, const char *name,
		uint16_t capacity_records, kres_resource *r)
{
	uint16_t name_length;
	uint32_t start;
	int result;

	if (c == (kres_container *)0 || c->opened == 0u ||
	    r == (kres_resource *)0) {
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
	if (kres_find(c, name, (uint8_t)name_length) != 0xFFFFu) {
		return -KOS_EEXIST;
	}
	result = allocate_records(c, capacity_records, &start);
	if (result < 0) {
		return result;
	}
	entry_append(c, (const uint8_t *)name, (uint8_t)name_length,
		     start, capacity_records, 0u);
	if (r != (kres_resource *)0) {
		resource_fill(c, c->resource_count - 1u, r);
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
		(void)kres_entry_put32(r->container, r->entry_index,
				      KRES_ENT_USED, r->used_bytes);
		r->container->dirty = 1u;
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
	(void)kres_entry_put32(r->container, r->entry_index,
			      KRES_ENT_USED, r->used_bytes);
	r->container->dirty = 1u;
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
	(void)kres_entry_put32(r->container, r->entry_index, KRES_ENT_USED,
			      used_bytes);
	r->container->dirty = 1u;
	return 0;
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
	unsigned int index;

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
	index = kres_find(c, name, (uint8_t)name_length);
	if (index == 0xFFFFu) {
		return -KOS_ENOENT;
	}
	entry_remove(c, index);
	return 0;
}

int kres_rename(kres_container *c, const char *old_name,
		const char *new_name)
{
	uint16_t old_length;
	uint16_t new_length;
	unsigned int index;
	uint8_t entry[KRES_ENTRY_SIZE];

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
	index = kres_find(c, old_name, (uint8_t)old_length);
	if (index == 0xFFFFu) {
		return -KOS_ENOENT;
	}
	if (kres_find(c, new_name, (uint8_t)new_length) != 0xFFFFu) {
		return -KOS_EEXIST;
	}
	if (kres_entry_get(c, index, entry) < 0) {
		return -KOS_EIO;
	}
	memset(entry + KRES_ENT_NAME, 0, KRES_NAME_MAX);
	memcpy(entry + KRES_ENT_NAME, new_name, new_length);
	entry[KRES_ENT_NAME_LENGTH] = (uint8_t)new_length;
	(void)kres_cache->put(c, index, entry);
	c->dirty = 1u;
	return 0;
}

/* --- the commit --------------------------------------------------------- */

int kres_flush(kres_container *c)
{
	uint32_t area;
	uint8_t header[KRES_RECORD_SIZE];
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
	/* Step 2: the cached catalog into the inactive area. */
	area = (c->active_area == 0u) ? KRES_AREA_B_BASE
				      : KRES_AREA_A_BASE;
	result = kres_cache->store(c, area);
	if (result < 0) {
		return result;
	}
	result = kres_io->sync(c->io_ctx);
	if (result < 0) {
		return result;
	}
	/* Step 3: the header, with the area switched and the generation
	 * advanced; the single commit point. */
	c->active_area = (uint8_t)(1u - c->active_area);
	c->generation = (uint32_t)(c->generation + 1u);
	header_emit(c, header);
	result = kres_io->seek(c->io_ctx, 0u);
	if (result < 0) {
		return result;
	}
	result = kres_io->write(c->io_ctx, header, KRES_RECORD_SIZE);
	if (result < 0) {
		return result;
	}
	if (result != (int)KRES_RECORD_SIZE) {
		return -KOS_EIO;
	}
	result = kres_io->sync(c->io_ctx);
	if (result == 0) {
		c->dirty = 0u;
	}
	return result;
}
