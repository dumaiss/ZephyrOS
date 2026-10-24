#if defined(KE_HOST_TEST)
#include <string.h>
#else
#include "kos.h"
#endif

#include "ke_platform.h"
#include "ke_store.h"

#define KE_NO_PAGE 0xffu

static uint16_t page_handle(const struct ke_store *store, uint8_t physical)
{
	return store->handles[physical / KE_PAGES_PER_OBJECT];
}

static uint16_t page_offset(uint8_t physical)
{
	return (uint16_t)((uint16_t)(physical % KE_PAGES_PER_OBJECT) *
			  KE_PAGE_SIZE);
}

static int exact_read(uint16_t handle, uint16_t offset, void *buffer,
		      uint16_t length)
{
	int result;

	if (length == 0u) return KE_STORE_OK;
	result = ke_xmem_read(handle, offset, buffer, length);
	return result == (int)length ? KE_STORE_OK : KE_STORE_EIO;
}

static int exact_write(uint16_t handle, uint16_t offset, const void *buffer,
		       uint16_t length)
{
	int result;

	if (length == 0u) return KE_STORE_OK;
	result = ke_xmem_write(handle, offset, buffer, length);
	return result == (int)length ? KE_STORE_OK : KE_STORE_EIO;
}

int ke_store_flush(struct ke_store *store)
{
	uint8_t physical;
	uint16_t length;
	int result;

	if (store == (struct ke_store *)0) return KE_STORE_EINVAL;
	if (store->cache_page == KE_NO_PAGE || store->cache_dirty == 0u)
		return KE_STORE_OK;
	physical = store->order[store->cache_page];
	length = store->page_length[store->cache_page];
	result = exact_write(page_handle(store, physical), page_offset(physical),
			     store->cache, length);
	if (result == KE_STORE_OK) store->cache_dirty = 0u;
	return result;
}

static int load_page(struct ke_store *store, uint8_t page)
{
	uint8_t physical;
	uint16_t length;
	int result;

	if (page >= store->page_count) return KE_STORE_EINVAL;
	if (store->cache_page == page) return KE_STORE_OK;
	result = ke_store_flush(store);
	if (result != KE_STORE_OK) return result;
	physical = store->order[page];
	length = store->page_length[page];
	result = exact_read(page_handle(store, physical), page_offset(physical),
			    store->cache, length);
	if (result != KE_STORE_OK) return result;
	store->cache_page = page;
	store->cache_dirty = 0u;
	return KE_STORE_OK;
}

static int locate(struct ke_store *store, ke_pos_t at,
		  uint8_t *page, uint16_t *offset)
{
	ke_pos_t base = 0ul;
	uint8_t i = 0u;

	if (at > store->length || page == (uint8_t *)0 ||
	    offset == (uint16_t *)0) return KE_STORE_EINVAL;
	for (; i < store->page_count; ++i) {
		ke_pos_t end = base + (ke_pos_t)store->page_length[i];

		if (at < end || (at == end && i + 1u == store->page_count)) {
			*page = i;
			*offset = (uint16_t)(at - base);
			return KE_STORE_OK;
		}
		base = end;
	}
	return KE_STORE_EINVAL;
}

static uint8_t physical_in_use(const struct ke_store *store, uint8_t physical)
{
	uint8_t i;

	for (i = 0u; i < store->page_count; ++i) {
		if (store->order[i] == physical) return 1u;
	}
	return 0u;
}

static int split_page(struct ke_store *store, uint8_t page)
{
	uint8_t physical = KE_NO_PAGE;
	uint8_t i;
	int result;

	if (store->page_count >= KE_PAGE_COUNT) return KE_STORE_ENOSPC;
	result = load_page(store, page);
	if (result != KE_STORE_OK) return result;
	for (i = 0u; i < KE_PAGE_COUNT; ++i) {
		if (physical_in_use(store, i) == 0u) {
			physical = i;
			break;
		}
	}
	if (physical == KE_NO_PAGE) return KE_STORE_ENOSPC;
	result = exact_write(page_handle(store, physical), page_offset(physical),
			     store->cache + KE_PAGE_SIZE / 2u,
			     KE_PAGE_SIZE / 2u);
	if (result != KE_STORE_OK) return result;
	store->page_length[page] = KE_PAGE_SIZE / 2u;
	store->cache_dirty = 1u;
	result = ke_store_flush(store);
	if (result != KE_STORE_OK) return result;
	for (i = store->page_count; i > (uint8_t)(page + 1u); --i) {
		store->order[i] = store->order[i - 1u];
		store->page_length[i] = store->page_length[i - 1u];
	}
	store->order[page + 1u] = physical;
	store->page_length[page + 1u] = KE_PAGE_SIZE / 2u;
	++store->page_count;
	store->cache_page = KE_NO_PAGE;
	return KE_STORE_OK;
}

static int append_empty_page(struct ke_store *store, uint8_t page)
{
	uint8_t physical = KE_NO_PAGE;
	uint8_t i;
	int result;

	if (store->page_count >= KE_PAGE_COUNT) return KE_STORE_ENOSPC;
	result = ke_store_flush(store);
	if (result != KE_STORE_OK) return result;
	for (i = 0u; i < KE_PAGE_COUNT; ++i) {
		if (physical_in_use(store, i) == 0u) {
			physical = i;
			break;
		}
	}
	if (physical == KE_NO_PAGE) return KE_STORE_ENOSPC;
	for (i = store->page_count; i > (uint8_t)(page + 1u); --i) {
		store->order[i] = store->order[i - 1u];
		store->page_length[i] = store->page_length[i - 1u];
	}
	store->order[page + 1u] = physical;
	store->page_length[page + 1u] = 0u;
	++store->page_count;
	return KE_STORE_OK;
}

/* With all 64 logical pages in use, make room in `page` by propagating one
 * boundary byte toward the nearest page that still has capacity. The byte
 * sequence does not change; locate() is repeated afterwards because page
 * boundaries do. This is a fragmented-store slow path, not ordinary local
 * editing. */
static int make_page_room(struct ke_store *store, uint8_t page)
{
	uint8_t q;
	uint8_t k;
	uint8_t moved;
	uint16_t length;
	int result;

	for (q = (uint8_t)(page + 1u); q < store->page_count; ++q) {
		if (store->page_length[q] < KE_PAGE_SIZE) {
			for (k = q; k > page; --k) {
				result = load_page(store, (uint8_t)(k - 1u));
				if (result != KE_STORE_OK) return result;
				length = store->page_length[k - 1u];
				moved = store->cache[length - 1u];
				store->page_length[k - 1u] = (uint16_t)(length - 1u);
				store->cache_dirty = 1u;
				result = ke_store_flush(store);
				if (result != KE_STORE_OK) return result;
				result = load_page(store, k);
				if (result != KE_STORE_OK) return result;
				length = store->page_length[k];
				memmove(store->cache + 1u, store->cache, length);
				store->cache[0] = moved;
				store->page_length[k] = (uint16_t)(length + 1u);
				store->cache_dirty = 1u;
				result = ke_store_flush(store);
				if (result != KE_STORE_OK) return result;
			}
			return KE_STORE_OK;
		}
	}
	q = page;
	while (q != 0u) {
		--q;
		if (store->page_length[q] < KE_PAGE_SIZE) {
			for (k = q; k < page; ++k) {
				result = load_page(store, (uint8_t)(k + 1u));
				if (result != KE_STORE_OK) return result;
				length = store->page_length[k + 1u];
				moved = store->cache[0];
				memmove(store->cache, store->cache + 1u,
					(uint16_t)(length - 1u));
				store->page_length[k + 1u] =
					(uint16_t)(length - 1u);
				store->cache_dirty = 1u;
				result = ke_store_flush(store);
				if (result != KE_STORE_OK) return result;
				result = load_page(store, k);
				if (result != KE_STORE_OK) return result;
				length = store->page_length[k];
				store->cache[length] = moved;
				store->page_length[k] = (uint16_t)(length + 1u);
				store->cache_dirty = 1u;
				result = ke_store_flush(store);
				if (result != KE_STORE_OK) return result;
			}
			return KE_STORE_OK;
		}
	}
	return KE_STORE_ENOSPC;
}

int ke_store_init(struct ke_store *store)
{
	uint8_t i;
	int handle;

	if (store == (struct ke_store *)0) return KE_STORE_EINVAL;
	memset(store, 0, sizeof(*store));
	store->cache_page = KE_NO_PAGE;
	for (i = 0u; i < KE_OBJECT_COUNT; ++i) {
		handle = ke_xmem_alloc(KE_OBJECT_SIZE, 0u);
		if (handle < 0) {
			while (i != 0u) {
				--i;
				(void)ke_xmem_free(store->handles[i]);
				store->handles[i] = 0u;
			}
			return KE_STORE_ENOSPC;
		}
		store->handles[i] = (uint16_t)handle;
	}
	store->page_count = 1u;
	store->order[0] = 0u;
	return KE_STORE_OK;
}

void ke_store_dispose(struct ke_store *store)
{
	uint8_t i;

	if (store == (struct ke_store *)0) return;
	(void)ke_store_flush(store);
	for (i = 0u; i < KE_OBJECT_COUNT; ++i) {
		if (store->handles[i] != 0u) {
			(void)ke_xmem_free(store->handles[i]);
			store->handles[i] = 0u;
		}
	}
	store->page_count = 0u;
	store->length = 0ul;
	store->cache_page = KE_NO_PAGE;
}

ke_pos_t ke_store_length(const struct ke_store *store)
{
	return store == (const struct ke_store *)0 ? 0ul : store->length;
}

int ke_store_byte(struct ke_store *store, ke_pos_t at, uint8_t *value)
{
	uint8_t page;
	uint16_t offset;
	int result;

	if (value == (uint8_t *)0 || at >= store->length)
		return KE_STORE_EINVAL;
	result = locate(store, at, &page, &offset);
	if (result != KE_STORE_OK) return result;
	result = load_page(store, page);
	if (result != KE_STORE_OK) return result;
	*value = store->cache[offset];
	return KE_STORE_OK;
}

int ke_store_read(struct ke_store *store, ke_pos_t at, void *buffer,
		  uint16_t length)
{
	uint8_t *target = (uint8_t *)buffer;
	uint16_t done = 0u;

	if (store == (struct ke_store *)0 || buffer == (void *)0 ||
	    at + (ke_pos_t)length > store->length) return KE_STORE_EINVAL;
	while (done < length) {
		uint8_t page;
		uint16_t offset;
		uint16_t chunk;
		int result = locate(store, at + done, &page, &offset);

		if (result != KE_STORE_OK) return result;
		result = load_page(store, page);
		if (result != KE_STORE_OK) return result;
		chunk = (uint16_t)(store->page_length[page] - offset);
		if (chunk > (uint16_t)(length - done))
			chunk = (uint16_t)(length - done);
		memcpy(target + done, store->cache + offset, chunk);
		done = (uint16_t)(done + chunk);
	}
	return KE_STORE_OK;
}

int ke_store_span(struct ke_store *store, ke_pos_t at, const uint8_t **page,
		  uint16_t *offset, uint16_t *page_length)
{
	uint8_t index;
	int result;

	if (store == (struct ke_store *)0 || at >= store->length)
		return KE_STORE_EINVAL;
	result = locate(store, at, &index, offset);
	if (result != KE_STORE_OK) return result;
	result = load_page(store, index);
	if (result != KE_STORE_OK) return result;
	*page = store->cache;
	*page_length = store->page_length[index];
	return KE_STORE_OK;
}

/* The bytes equal to `value` in [from, to), a page span at a time. */
uint32_t ke_store_count(struct ke_store *store, ke_pos_t from, ke_pos_t to,
			uint8_t value)
{
	uint32_t count = 0ul;

	while (from < to) {
		const uint8_t *page;
		uint16_t offset;
		uint16_t length;
		uint16_t n;
		uint16_t i;

		if (ke_store_span(store, from, &page, &offset, &length) !=
		    KE_STORE_OK) break;
		n = (uint16_t)(length - offset);
		if (to - from < (ke_pos_t)n) n = (uint16_t)(to - from);
		page += offset;
		for (i = 0u; i < n; ++i)
			if (page[i] == value) ++count;
		from += n;
	}
	return count;
}

static int insert_byte(struct ke_store *store, ke_pos_t at, uint8_t value)
{
	uint8_t page;
	uint16_t offset;
	uint16_t length;
	int result;

	if (store->length >= KE_DOCUMENT_CAPACITY) return KE_STORE_ENOSPC;
	result = locate(store, at, &page, &offset);
	if (result != KE_STORE_OK) return result;
	if (store->page_length[page] == KE_PAGE_SIZE) {
		if (store->page_count >= KE_PAGE_COUNT) {
			result = make_page_room(store, page);
			if (result != KE_STORE_OK) return result;
			result = locate(store, at, &page, &offset);
			if (result != KE_STORE_OK) return result;
		} else if (offset == KE_PAGE_SIZE &&
			   page + 1u == store->page_count) {
			result = append_empty_page(store, page);
			if (result != KE_STORE_OK) return result;
			++page;
			offset = 0u;
		} else {
			result = split_page(store, page);
			if (result != KE_STORE_OK) return result;
			if (offset >= KE_PAGE_SIZE / 2u) {
				++page;
				offset = (uint16_t)(offset - KE_PAGE_SIZE / 2u);
			}
		}
	}
	result = load_page(store, page);
	if (result != KE_STORE_OK) return result;
	length = store->page_length[page];
	memmove(store->cache + offset + 1u, store->cache + offset,
		(uint16_t)(length - offset));
	store->cache[offset] = value;
	store->page_length[page] = (uint16_t)(length + 1u);
	store->cache_dirty = 1u;
	++store->length;
	return KE_STORE_OK;
}

int ke_store_insert(struct ke_store *store, ke_pos_t at,
		    const void *buffer, uint16_t length)
{
	const uint8_t *source = (const uint8_t *)buffer;
	uint16_t i;
	int result;

	if (store == (struct ke_store *)0 ||
	    (buffer == (const void *)0 && length != 0u) || at > store->length)
		return KE_STORE_EINVAL;
	if (store->length + (ke_pos_t)length > KE_DOCUMENT_CAPACITY)
		return KE_STORE_ENOSPC;
	for (i = 0u; i < length; ++i) {
		result = insert_byte(store, at + i, source[i]);
		if (result != KE_STORE_OK) return result;
	}
	return KE_STORE_OK;
}

static void remove_page(struct ke_store *store, uint8_t page)
{
	uint8_t i;

	if (store->page_count <= 1u) return;
	if (store->cache_page == page) store->cache_page = KE_NO_PAGE;
	for (i = page; i + 1u < store->page_count; ++i) {
		store->order[i] = store->order[i + 1u];
		store->page_length[i] = store->page_length[i + 1u];
	}
	--store->page_count;
}

int ke_store_delete(struct ke_store *store, ke_pos_t at, ke_pos_t length)
{
	if (store == (struct ke_store *)0 || at > store->length ||
	    length > store->length - at) return KE_STORE_EINVAL;
	while (length != 0ul) {
		uint8_t page;
		uint16_t offset;
		uint16_t chunk;
		uint16_t page_length;
		int result = locate(store, at, &page, &offset);

		if (result != KE_STORE_OK) return result;
		result = load_page(store, page);
		if (result != KE_STORE_OK) return result;
		page_length = store->page_length[page];
		chunk = (uint16_t)(page_length - offset);
		if ((ke_pos_t)chunk > length) chunk = (uint16_t)length;
		memmove(store->cache + offset, store->cache + offset + chunk,
			(uint16_t)(page_length - offset - chunk));
		store->page_length[page] = (uint16_t)(page_length - chunk);
		store->cache_dirty = 1u;
		store->length -= chunk;
		length -= chunk;
		if (store->page_length[page] == 0u && store->page_count > 1u) {
			result = ke_store_flush(store);
			if (result != KE_STORE_OK) return result;
			remove_page(store, page);
		}
	}
	return KE_STORE_OK;
}

int ke_store_replace_byte(struct ke_store *store, ke_pos_t at, uint8_t value)
{
	uint8_t page;
	uint16_t offset;
	int result;

	if (store == (struct ke_store *)0 || at >= store->length)
		return KE_STORE_EINVAL;
	result = locate(store, at, &page, &offset);
	if (result != KE_STORE_OK) return result;
	result = load_page(store, page);
	if (result != KE_STORE_OK) return result;
	store->cache[offset] = value;
	store->cache_dirty = 1u;
	return KE_STORE_OK;
}
