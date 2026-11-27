/* KonaOS page ownership on the Zephyr-80 Mk2 board (architecture 2.7.6).
 *
 * The allocator; see kernel/memory/mk2/pages.h (the chunked copies are
 * kernel/memory/mk2/page_copy.asm).  Single-tasking: the nucleus calls
 * these from syscall bodies and the process lifecycle, never from an
 * interrupt.
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

	if (count == 0u || owner > KOS_PAGE_OWNER_NUCLEUS) {
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
