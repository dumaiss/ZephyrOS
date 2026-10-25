/* KRES core: the read path (private).
 *
 * kres_open, kres_close, kres_get, kres_rclose, kres_read, kres_seek,
 * kres_stat, the iterator and the record reads.  Nothing here mutates
 * a container; the mutation and commit machinery lives in kres_mut.c.
 */

#include "kres_internal.h"

/* Open the backing file, validate the header, load the active catalog.
 * KRES_OPEN_CREATE makes a missing file an empty mutable container
 * (through the mutation member, so read-only programs do not link it). */
int kres_open(kres_container *c, const char *path, unsigned int flags)
{
	uint8_t header[KRES_RECORD_SIZE];
	uint32_t size;
	int result;

	if (c == (kres_container *)0 || kres_io == (const struct kres_io *)0 ||
	    kres_cache == (const struct kres_cache *)0 ||
	    (flags & KRES_OPEN_READ) == 0u) {
		return -KOS_EINVAL;
	}
	memset(c, 0, sizeof(*c));
	c->io_ctx = c; /* the adapter keys its per-open state by ctx, so a
			* compactor can hold two containers open at once */
	result = kres_io->open(c, path, flags);
	if (result == -KOS_ENOENT && (flags & KRES_OPEN_CREATE) != 0u) {
		result = kres_create_container(path);
		if (result < 0) {
			return result;
		}
		result = kres_io->open(c, path, flags);
	}
	if (result < 0) {
		return result;
	}
	c->opened = 1u;
	result = kres_io->size(c->io_ctx, &size);
	if (result < 0) {
		goto fail;
	}
	if (size < (uint32_t)KRES_DATA_BASE_SEALED * KRES_RECORD_SIZE) {
		result = -KRES_E_BAD;
		goto fail;
	}
	if (kres_read_records_at(c->io_ctx, 0u, header, 1u) != 1) {
		result = -KOS_EIO;
		goto fail;
	}
	result = kres_header_parse(header, c);
	if (result < 0) {
		goto fail;
	}
	if (c->sealed != 0u) {
		c->writable = 0u;
		c->dirty = 0u;
	} else {
		c->writable = ((flags & KRES_OPEN_WRITE) != 0u);
	}
	c->file_size = size;
	/* The header and the catalog areas must be present; the data
	 * records may end in a partial final record. */
	if (size < (uint32_t)(c->sealed != 0u
				      ? KRES_DATA_BASE_SEALED
				      : KRES_DATA_BASE_MUT) *
			   KRES_RECORD_SIZE) {
		result = -KRES_E_BAD;
		goto fail;
	}
	result = kres_cache->load(c, c->sealed != 0u
				      ? KRES_AREA_A_BASE
				      : c->active_area != 0u
					    ? KRES_AREA_B_BASE
					    : KRES_AREA_A_BASE);
	if (result < 0) {
		goto fail;
	}
	return 0;
fail:
	(void)kres_io->close(c->io_ctx);
	(void)kres_cache->release(c);
	c->opened = 0u;
	return result;
}

int kres_close(kres_container *c)
{
	int result;

	if (c == (kres_container *)0 || c->opened == 0u) {
		return -KOS_EINVAL;
	}
	result = 0;
	if (c->writable != 0u) {
		result = kres_flush(c);
	}
	if (result == 0) {
		result = kres_io->close(c->io_ctx);
	}
	(void)kres_cache->release(c);
	c->opened = 0u;
	return result;
}

/* --- resources --------------------------------------------------------- */

int resource_fill(kres_container *c, unsigned int index,
		 kres_resource *r)
{
	uint8_t entry[KRES_ENTRY_SIZE];
	int result = kres_entry_get(c, index, entry);

	if (result < 0) {
		return result;
	}
	memset(r, 0, sizeof(*r));
	r->container = c;
	r->open = 1u;
	r->writable = c->writable;
	r->entry_index = (uint8_t)index;
	r->start_record = kres_get32(entry + KRES_ENT_START);
	r->allocated_records = kres_get32(entry + KRES_ENT_ALLOCATED);
	r->used_bytes = kres_get32(entry + KRES_ENT_USED);
	return 0;
}

int kres_get(kres_container *c, const char *name, kres_resource *r,
	     unsigned int flags)
{
	uint16_t name_length;
	unsigned int index;

	if (c == (kres_container *)0 || c->opened == 0u ||
	    r == (kres_resource *)0) {
		return -KOS_EINVAL;
	}
	if (c->writable == 0u && (flags & KRES_OPEN_WRITE) != 0u) {
		return -KOS_EROFS;
	}
	name_length = kres_name_check(name);
	if (name_length == 0xFFFFu) {
		return -KOS_EINVAL;
	}
	index = kres_find(c, name, (uint8_t)name_length);
	if (index == 0xFFFFu) {
		return -KOS_ENOENT;
	}
	return resource_fill(c, index, r);
}

int kres_rclose(kres_resource *r)
{
	if (r == (kres_resource *)0 || r->open == 0u) {
		return -KOS_EINVAL;
	}
	r->open = 0u;
	return 0;
}

int kres_seek(kres_resource *r, uint32_t offset)
{
	if (r == (kres_resource *)0 || r->open == 0u) {
		return -KOS_EINVAL;
	}
	r->cursor = offset;
	return 0;
}

int kres_read(kres_resource *r, void *dst, uint16_t len)
{
	uint32_t at;
	uint16_t got = 0u;

	if (r == (kres_resource *)0 || r->open == 0u || dst == (void *)0 ||
	    len == 0u) {
		return -KOS_EINVAL;
	}
	if (r->cursor >= r->used_bytes) {
		return 0; /* end of file */
	}
	if ((uint32_t)len > r->used_bytes - r->cursor) {
		len = (uint16_t)(r->used_bytes - r->cursor);
	}
	at = r->start_record * KRES_RECORD_SIZE + r->cursor;
	while (len != 0u) {
		uint16_t chunk = (uint16_t)(KRES_RECORD_SIZE -
			 (at & (uint32_t)(KRES_RECORD_SIZE - 1u)));
		int result;

		if (chunk > len) {
			chunk = len;
		}
		result = kres_io->seek(r->container->io_ctx, at);
		if (result < 0) {
			return result;
		}
		result = kres_io->read(r->container->io_ctx, kres_shared,
				       chunk);
		if (result < 0) {
			return result;
		}
		if (result == 0) {
			break;
		}
		memcpy(dst, kres_shared, (uint16_t)result);
		dst = (uint8_t *)dst + result;
		got = (uint16_t)(got + (uint16_t)result);
		len = (uint16_t)(len - (uint16_t)result);
		at = (uint32_t)(at + (uint16_t)result);
		r->cursor = (uint32_t)(r->cursor + (uint16_t)result);
		if (result != (int)chunk) {
			break; /* a short read ends the walk */
		}
	}
	return got;
}

int kres_read_records(kres_resource *r, uint16_t first_record,
		      uint16_t record_count, void *dst)
{
	uint16_t count = 0u;
	int total = 0;

	if (r == (kres_resource *)0 || r->open == 0u || dst == (void *)0) {
		return -KOS_EINVAL;
	}
	while (count < record_count &&
	       (uint32_t)first_record + count < r->allocated_records) {
		uint32_t record = (uint32_t)first_record + count;
		uint32_t at = (uint32_t)r->start_record + record;
		uint32_t record_begin = record * KRES_RECORD_SIZE;
		uint16_t want = KRES_RECORD_SIZE;
		int result;

		if (record_begin >= r->used_bytes) {
			break;
		}
		if (record_begin + KRES_RECORD_SIZE > r->used_bytes) {
			want = (uint16_t)(r->used_bytes - record_begin);
		}
		result = kres_io->seek(r->container->io_ctx,
				       at * KRES_RECORD_SIZE);
		if (result < 0) {
			return result;
		}
		result = kres_io->read(r->container->io_ctx, kres_shared,
				       want);
		if (result < 0) {
			return result;
		}
		memcpy(dst, kres_shared, (uint16_t)result);
		dst = (uint8_t *)dst + result;
		total += result;
		++count;
		if (result != (int)want) {
			break; /* a short read ends the walk */
		}
	}
	return total;
}

/* --- the catalog -------------------------------------------------------- */

int kres_stat(kres_container *c, const char *name, struct kres_info *out)
{
	uint16_t name_length;
	unsigned int index;

	if (c == (kres_container *)0 || c->opened == 0u ||
	    out == (struct kres_info *)0) {
		return -KOS_EINVAL;
	}
	name_length = kres_name_check(name);
	if (name_length == 0xFFFFu) {
		return -KOS_EINVAL;
	}
	index = kres_find(c, name, (uint8_t)name_length);
	if (index == 0xFFFFu) {
		return -KOS_ENOENT;
	}
	{
		uint8_t entry[KRES_ENTRY_SIZE];
		int result = kres_entry_get(c, index, entry);

		if (result < 0) {
			return result;
		}
		kres_entry_info(entry, out);
	}
	return 0;
}

int kres_iter_begin(kres_container *c, struct kres_iter *it)
{
	if (c == (kres_container *)0 || c->opened == 0u ||
	    it == (struct kres_iter *)0) {
		return -KOS_EINVAL;
	}
	it->container = c;
	it->index = 0u;
	return 0;
}

int kres_iter_next(struct kres_iter *it, struct kres_info *out)
{
	if (it == (struct kres_iter *)0 || out == (struct kres_info *)0 ||
	    it->container == (kres_container *)0 ||
	    it->container->opened == 0u) {
		return -KOS_EINVAL;
	}
	if (it->index >= it->container->resource_count) {
		return 0; /* enumeration ended */
	}
	{
		uint8_t entry[KRES_ENTRY_SIZE];
		int result = kres_entry_get(it->container, it->index, entry);

		if (result < 0) {
			return result;
		}
		kres_entry_info(entry, out);
	}
	++it->index;
	return 1;
}
