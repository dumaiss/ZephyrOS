/* KRES core: the read path (private).
 *
 * kres_open, kres_close, kres_get, kres_rclose, kres_read, kres_seek,
 * kres_stat, the iterator and the record reads.  Nothing here mutates
 * a container; the mutation and commit machinery lives in kres_mut.c.
 */

#include "kres_internal.h"

/* Open the backing file and validate the header.  A writable open
 * loads the active catalog into the catalog store; a read-only open
 * reads the catalog one record at a time on demand.
 * KRES_OPEN_CREATE makes a missing file an empty mutable container
 * (through the mutation member, so read-only programs do not link it). */
int kres_open(kres_container *c, const char *path, unsigned int flags)
{
	uint32_t size;
	uint16_t start;
	int result;

	if (c == (kres_container *)0 || kres_io == (const struct kres_io *)0 ||
	    (flags & KRES_OPEN_READ) == 0u ||
	    ((flags & KRES_OPEN_WRITE) != 0u &&
	     kres_cache == (const struct kres_cache *)0)) {
		return -KOS_EINVAL;
	}
	kres_record_cache_drop(c);
	memset(c, 0, sizeof(*c));
	c->io_ctx = c; /* the adapter keys its per-open state by ctx, so a
			* compactor can hold two containers open at once */
	/* KRES_OPEN_CREATE does not reach the adapter: an adapter that
	 * creates on demand (KFS_O_CREATE, "w+b") would make the
	 * missing file itself and return a zero-length file the header
	 * check rejects.  Probe without it; only a reported miss goes
	 * through the mutation member's create-on-missing. */
	result = kres_io->open(c, path, flags & ~KRES_OPEN_CREATE);
	if (result == -KOS_ENOENT && (flags & KRES_OPEN_CREATE) != 0u &&
	    kres_create_hook != (int (*)(const char *))0) {
		result = kres_create_hook(path);
		if (result < 0) {
			return result;
		}
		result = kres_io->open(c, path, flags & ~KRES_OPEN_CREATE);
	}
	if (result < 0) {
		return result;
	}
	c->opened = 1u;
	result = kres_io->size(c->io_ctx, &size);
	if (result < 0) {
		goto fail;
	}
	if (size < KRES_RECORD_SIZE) {
		result = -KRES_E_BAD;
		goto fail;
	}
	if (kres_read_records_at(c->io_ctx, 0u, kres_shared, 1u) != 1) {
		result = -KOS_EIO;
		goto fail;
	}
	result = kres_header_parse(kres_shared, c);
	if (result < 0) {
		goto fail;
	}
	c->file_size = size;
	/* The active catalog's entries must be present; the data records
	 * may end in a partial final record. */
	start = c->catalog_start[c->active_area];
	if (size < kres_record_offset((uint32_t)start +
				      kres_catalog_fill(c->resource_count))) {
		result = -KRES_E_BAD;
		goto fail;
	}
	if (c->sealed == 0u && (flags & KRES_OPEN_WRITE) != 0u) {
		result = kres_cache->load(c, start, c->resource_count);
		if (result < 0) {
			goto fail;
		}
		c->writable = 1u;
	}
	return 0;
fail:
	(void)kres_io->close(c->io_ctx);
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
	if (c->writable != 0u &&
	    kres_flush_hook != (int (*)(kres_container *))0) {
		result = kres_flush_hook(c);
	}
	if (result == 0) {
		result = kres_io->close(c->io_ctx);
	} else {
		(void)kres_io->close(c->io_ctx);
	}
	if (c->writable != 0u) {
		(void)kres_cache->release(c);
	}
	kres_record_cache_drop(c);
	c->opened = 0u;
	return result;
}

/* --- resources --------------------------------------------------------- */

int resource_fill(kres_container *c, uint16_t index, kres_resource *r)
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
	r->entry_index = index;
	r->epoch = c->epoch;
	r->start_record = kres_get32(entry + KRES_ENT_START);
	r->allocated_records = kres_get32(entry + KRES_ENT_ALLOCATED);
	r->used_bytes = kres_get32(entry + KRES_ENT_USED);
	return 0;
}

int kres_get(kres_container *c, const char *name, kres_resource *r,
	     unsigned int flags)
{
	uint16_t name_length;
	uint16_t index;

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
	if (index == KRES_NO_ENTRY) {
		return -KOS_ENOENT;
	}
	return resource_fill(c, index, r);
}

/* --- the catalog -------------------------------------------------------- *
 *
 * kres_stat is one binary search; the iterator walks the entries in
 * name order, eight to a catalog record, so a read-only walk reads
 * each record once. */

int kres_stat(kres_container *c, const char *name, struct kres_info *out)
{
	uint16_t name_length;
	uint16_t index;

	if (c == (kres_container *)0 || c->opened == 0u ||
	    out == (struct kres_info *)0) {
		return -KOS_EINVAL;
	}
	name_length = kres_name_check(name);
	if (name_length == 0xFFFFu) {
		return -KOS_EINVAL;
	}
	index = kres_find(c, name, (uint8_t)name_length);
	if (index == KRES_NO_ENTRY) {
		return -KOS_ENOENT;
	}
	{
		uint8_t scratch[KRES_ENTRY_SIZE];
		const uint8_t *entry = kres_entry_at(c, index, scratch);

		if (entry == (const uint8_t *)0) {
			return -KOS_EIO;
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
		uint8_t scratch[KRES_ENTRY_SIZE];
		const uint8_t *entry = kres_entry_at(it->container, it->index,
						     scratch);

		if (entry == (const uint8_t *)0) {
			return -KOS_EIO;
		}
		kres_entry_info(entry, out);
	}
	++it->index;
	return 1;
}
