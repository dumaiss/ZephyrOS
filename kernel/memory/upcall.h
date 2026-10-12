/* KonaOS kernel-private upcall shim (architecture 36.5).
 *
 * Cold-half nucleus code that has to call a resident driver does so
 * through this fixed primitive: it saves the exact banking latch, selects
 * the window page, makes one call into the window, and restores the saved
 * latch.  The caller is not executing while the window shows another bank
 * (the decoder maps bank 7 - P over 6000h-DFFFh), so the caller itself may
 * live in the cold half; the shim cannot, because its own code and its
 * immediate continuation must stay visible across the page change.
 *
 * Usage contract (kernel-internal, not a stable ABI):
 *   - the request block must lie in the fixed core (below 6000h): no
 *     request block is read or written while another bank is mapped;
 *   - `entry` must lie in the window 6000h-DFFFh, which is the only range
 *     the page field selects;
 *   - the caller's stack must stay visible whatever the window shows: the
 *     common region or the fixed core (the syscall gate stack and the ISR
 *     stack both satisfy this);
 *   - `args` is passed to the target in HL, the target's HL return value
 *     is published in `result`, and its A register at return is published
 *     in `kos_upcall_ret_a` (the sdcccall(1) 8-bit return, which is how a
 *     driver's init reports its status);
 *   - single-depth: the state below is not reentrant (single-tasking, like
 *     the common transition gates).
 *
 * Maskable interrupts may arrive during an upcall: the IRQ gate saves the
 * interrupted latch (page field included) in its own slot, dispatches with
 * P = 0 and restores the latch exactly, so the upcall resumes with its
 * selected page still active.
 */

#ifndef KONAOS_KERNEL_MEMORY_UPCALL_H
#define KONAOS_KERNEL_MEMORY_UPCALL_H

#include "kos/errno.h"
#include "kos/types.h"

/* Window pages the operating system uses (architecture 36.2): the decoder
 * answers 6000h-DFFFh from bank 7 - P, so page 0 is the nucleus's own cold
 * half and page 1 is the resident driver domain in bank 6. */
#define KOS_UPCALL_PAGE_NUCLEUS 0u
#define KOS_UPCALL_PAGE_DRIVER 1u
#define KOS_UPCALL_PAGE_MAX KOS_UPCALL_PAGE_DRIVER

struct kos_upcall_req {
	uint8_t page;   /* KOS_UPCALL_PAGE_*                        */
	uint16_t entry; /* target address in the window 6000h-DFFFh */
	void *args;     /* target argument, passed in HL            */
	uint16_t result;/* target result, read from HL              */
};

#if defined(__SDCC)
/* The shim decodes this block at fixed offsets; keep them pinned. */
_Static_assert(sizeof(struct kos_upcall_req) == 7,
	       "upcall request must be 7 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_upcall_req, page) == 0,
	       "upcall page offset");
_Static_assert(__builtin_offsetof(struct kos_upcall_req, entry) == 1,
	       "upcall entry offset");
_Static_assert(__builtin_offsetof(struct kos_upcall_req, args) == 3,
	       "upcall args offset");
_Static_assert(__builtin_offsetof(struct kos_upcall_req, result) == 5,
	       "upcall result offset");
#endif

/* Returns 0, or KOS_EINVAL when the page is not one the operating system
 * uses or the entry does not lie in the window. */
uint8_t kos_upcall(const struct kos_upcall_req *req);

/* The target's A register at return (upcall.asm): a `uint8_t`-returning
 * entry publishes its sdcccall(1) result here.  The driver loader reads
 * it back as the init status. */
extern uint8_t kos_upcall_ret_a;

#endif /* KONAOS_KERNEL_MEMORY_UPCALL_H */
