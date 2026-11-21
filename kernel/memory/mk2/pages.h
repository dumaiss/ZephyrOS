/* KonaOS page ownership on the Zephyr-80 Mk2 board (kernel-private).
 *
 * Architecture 2.7.6: the nucleus owns the 8 KiB SRAM pages one by one.
 * Every page has one owner - free, the nucleus, or a process - and a
 * process's memory is a set of pages: its primary pages (one DAT image,
 * _kos_dat_images[pid]) and the pages of its extended-memory objects.
 * ROM pages are never allocated; the nucleus's pages (the common page, the
 * page-0 images, its code and data) are reserved at init from a list the
 * build provides.
 *
 * Bytes move between pages through the system copy windows
 * (kernel/memory/mk2/page_copy.asm): a "map" is a page list indexed by
 * address >> 13 - a DAT image for a logical address, an object's pages for
 * an object offset - so one copy can cross page boundaries on both sides.
 */

#ifndef KONAOS_KERNEL_MEMORY_MK2_PAGES_H
#define KONAOS_KERNEL_MEMORY_MK2_PAGES_H

#include "kos/types.h"
#include "hardware.h"
#include "memory.h"

/* --- the allocator (pages.c) --------------------------------------------- */

/* One owner byte per SRAM page: KOS_PAGE_OWNER_FREE, KOS_PAGE_OWNER_NUCLEUS
 * or pid + 1. */
#define KOS_PAGE_OWNER_FREE    0x00u
#define KOS_PAGE_OWNER_NUCLEUS 0x80u

/* The pid kos_pages_alloc takes for a nucleus page (the driver domain). */
#define KOS_PAGE_PID_NUCLEUS   (KOS_PAGE_OWNER_NUCLEUS - 1u)

extern uint8_t kos_page_owner[KOS_DAT_SRAM_PAGES];

/* Every SRAM page free, then the nucleus's pages reserved: `nucleus` is
 * the build's list of SRAM pages, ended by 00h (a ROM page). */
void kos_pages_init(const uint8_t *nucleus);

/* `count` pages (1 or more) for `pid` (KOS_PAGE_PID_NUCLEUS: the
 * nucleus), lowest free first, written to `pages`; all or none.  0,
 * -KOS_EINVAL or -KOS_ENOSPC. */
int kos_pages_alloc(uint8_t pid, uint8_t count, uint8_t *pages);

/* Release the listed pages `pid` owns (others are left alone). */
void kos_pages_free(uint8_t pid, const uint8_t *pages, uint8_t count);

/* Release every page `pid` owns; returns how many. */
uint8_t kos_pages_release(uint8_t pid);

uint8_t kos_pages_free_count(void);

/* --- moving bytes (pages.c over page_copy.asm) --------------------------- */

#define KOS_PAGE_ZEROES 0x80u /* page_copy.asm: zero-fill the chunk     */
#define KOS_PAGE_DIRECT 0xFFu /* page_copy.asm: a nucleus address       */

struct kos_page_copy_req {
	uint8_t  src_page;
	uint8_t  dst_page;
	uint16_t src;
	uint16_t dst;
	uint16_t length;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct kos_page_copy_req) == 8,
	       "the page copy request must be 8 packed bytes");
#endif

void kos_page_copy(const struct kos_page_copy_req *req);

/* Copy `length` bytes from `src` through `src_map` to `dst` through
 * `dst_map`; a null map means a nucleus address (outside the copy
 * windows).  The caller has checked every range. */
void kos_pages_copy(const uint8_t *src_map, uint16_t src,
		    const uint8_t *dst_map, uint16_t dst, uint16_t length);

/* Zero `length` bytes at `dst` through `dst_map`. */
void kos_pages_zero(const uint8_t *dst_map, uint16_t dst, uint16_t length);

/* --- system entries (dat_map.asm, cold_call.asm; fixed) ----------------- */

/* Remap `count` system entries from `first` on to `pages`, mirror first
 * (kos_dat_map's span argument: KOS_DAT_SPAN(first, count)). */
#define KOS_DAT_SPAN(first, count) \
	((uint16_t)((uint16_t)(first) | ((uint16_t)(count) << 8)))
void kos_dat_map(const uint8_t *pages, uint16_t span);

/* Empty the cold-set veneer stack (the syscall dispatcher's entry, where
 * no cross-set call is live). */
void kos_cold_reset(void);

/* --- DAT images and the far window (banking.asm, far.asm; common) -------- */

/* One image per process, indexed by pid (PROCESS_TABLE_SIZE rows). */
extern uint8_t kos_dat_images[][KOS_DAT_ENTRIES];

/* The running process's image; 0 while no program runs. */
extern uint8_t *kos_dat_running;

/* The running program's far-window handle (0: its own pages). */
extern uint16_t kos_far_current;

/* One row per extended-memory object: the generation of its mapping
 * handle (0: none) and the pages the far window shows for it. */
struct kos_far_row {
	uint8_t gen;
	uint8_t pages[2];
};

extern struct kos_far_row kos_far_table[];

/* During a syscall body: the caller's current working mapping, far window
 * included (the image the gate keeps for the crossing).  A caller address
 * is resolved through it (architecture 2.7.6, buffer identity). */
extern const uint8_t *kos_gate_saved_image;
#define kos_caller_map() (kos_gate_saved_image)

/* The working bank's cached-image identity (memory.h KOS_DAT_CACHED):
 * code that edits an image the bank may hold drops the identity, so the
 * next entry loads the image again. */
#define kos_dat_cache_drop() \
	(*(volatile uint8_t *)(KOS_DAT_CACHED + 1u) = 0u)

/* The common page: system entry 7's mirror. */
#define kos_dat_common_page() \
	(((volatile const uint8_t *)KOS_DAT_SYSTEM)[KOS_DAT_ENTRIES - 1u])

#endif /* KONAOS_KERNEL_MEMORY_MK2_PAGES_H */
