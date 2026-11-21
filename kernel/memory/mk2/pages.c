/* KonaOS page ownership on the Zephyr-80 Mk2 board (architecture 2.7.6).
 *
 * The allocator and the chunked copy over the page-copy primitive; see
 * kernel/memory/mk2/pages.h.  Single-tasking: the nucleus calls these from
 * syscall bodies and the process lifecycle, never from an interrupt.
 */

#include "kos/errno.h"
#include "kernel/memory/mk2/pages.h"

uint8_t kos_page_owner[KOS_DAT_SRAM_PAGES];
static uint8_t page_free;

void kos_pages_init(const uint8_t *nucleus)
{
	uint8_t i;

	for (i = 0u; i < KOS_DAT_SRAM_PAGES; ++i) {
		kos_page_owner[i] = KOS_PAGE_OWNER_FREE;
	}
	page_free = KOS_DAT_SRAM_PAGES;
	for (; *nucleus != 0u; ++nucleus) {
		i = (uint8_t)(*nucleus - KOS_DAT_SRAM_FIRST);
		if (i < KOS_DAT_SRAM_PAGES &&
		    kos_page_owner[i] == KOS_PAGE_OWNER_FREE) {
			kos_page_owner[i] = KOS_PAGE_OWNER_NUCLEUS;
			--page_free;
		}
	}
}

int kos_pages_alloc(uint8_t pid, uint8_t count, uint8_t *pages)
{
	uint8_t owner = (uint8_t)(pid + 1u);
	uint8_t i;

	if (count == 0u || owner >= KOS_PAGE_OWNER_NUCLEUS) {
		return -KOS_EINVAL;
	}
	if (count > page_free) {
		return -KOS_ENOSPC;
	}
	page_free = (uint8_t)(page_free - count);
	for (i = 0u; count != 0u; ++i) {
		if (kos_page_owner[i] == KOS_PAGE_OWNER_FREE) {
			kos_page_owner[i] = owner;
			*pages++ = (uint8_t)(KOS_DAT_SRAM_FIRST + i);
			--count;
		}
	}
	return 0;
}

void kos_pages_free(uint8_t pid, const uint8_t *pages, uint8_t count)
{
	uint8_t owner = (uint8_t)(pid + 1u);
	uint8_t i;

	for (; count != 0u; --count) {
		i = (uint8_t)(*pages++ - KOS_DAT_SRAM_FIRST);
		if (i < KOS_DAT_SRAM_PAGES && kos_page_owner[i] == owner) {
			kos_page_owner[i] = KOS_PAGE_OWNER_FREE;
			++page_free;
		}
	}
}

uint8_t kos_pages_release(uint8_t pid)
{
	uint8_t owner = (uint8_t)(pid + 1u);
	uint8_t released = 0u;
	uint8_t i;

	for (i = 0u; i < KOS_DAT_SRAM_PAGES; ++i) {
		if (kos_page_owner[i] == owner) {
			kos_page_owner[i] = KOS_PAGE_OWNER_FREE;
			++released;
		}
	}
	page_free = (uint8_t)(page_free + released);
	return released;
}

uint8_t kos_pages_free_count(void)
{
	return page_free;
}

/* One side of a chunk: a page and the offset in it, or a nucleus address
 * (no page boundary).  Returns the room left in that page. */
static uint16_t page_side(const uint8_t *map, uint16_t addr, uint8_t *page,
			  uint16_t *at)
{
	if (map == (const uint8_t *)0) {
		*page = KOS_PAGE_DIRECT;
		*at = addr;
		return 0xFFFFu;
	}
	*page = map[addr >> KOS_DAT_PAGE_SHIFT];
	*at = (uint16_t)(addr & (KOS_DAT_PAGE_SIZE - 1u));
	return (uint16_t)(KOS_DAT_PAGE_SIZE - *at);
}

/* The copy, or with `zero` set a zero fill (no source). */
static void pages_move(uint8_t zero, const uint8_t *src_map, uint16_t src,
		       const uint8_t *dst_map, uint16_t dst, uint16_t length)
{
	struct kos_page_copy_req req;
	uint16_t room;

	while (length != 0u) {
		req.length = length;
		if (zero != 0u) {
			req.src_page = KOS_PAGE_ZEROES;
		} else {
			room = page_side(src_map, src, &req.src_page, &req.src);
			if (req.length > room) {
				req.length = room;
			}
		}
		room = page_side(dst_map, dst, &req.dst_page, &req.dst);
		if (req.length > room) {
			req.length = room;
		}
		kos_page_copy(&req);
		src = (uint16_t)(src + req.length);
		dst = (uint16_t)(dst + req.length);
		length = (uint16_t)(length - req.length);
	}
}

void kos_pages_copy(const uint8_t *src_map, uint16_t src,
		    const uint8_t *dst_map, uint16_t dst, uint16_t length)
{
	pages_move(0u, src_map, src, dst_map, dst, length);
}

void kos_pages_zero(const uint8_t *dst_map, uint16_t dst, uint16_t length)
{
	pages_move(1u, (const uint8_t *)0, 0u, dst_map, dst, length);
}
