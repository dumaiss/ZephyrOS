/* KonaOS extended-memory and segment syscalls (architecture 4.8 and
 * 6.7).
 *
 * Implements SYS_XMEM_ALLOC / _FREE / _READ / _WRITE / _INFO / _MAP,
 * SYS_SEG_INFO and SYS_SEG_MAP.  A runtime object is process-owned,
 * read/write and non-executable; a loader-created EXE segment is the same
 * object with type KOS_XMEM_TYPE_SEGMENT and a container id - a `DATA`
 * segment is reached by handle, a `FAR_CODE` segment is executed in place
 * through the aperture and its map token comes from SYS_SEG_MAP.  The
 * size is rounded up to whole 8-KiB auxiliary slots and the object is
 * contiguous inside one bank (kernel/process/process.c owns the slot map).
 * **Contents are undefined**: nothing here clears a slot (4.8) - a
 * previous object's bytes are what a new one starts with.
 *
 * Handles are opaque and owner-tagged: `(index << 4) | generation`, 9
 * bits for table indices 0-31 and generations 1-15, so a freed-and-reused
 * table entry hands out a different handle and a stale or foreign handle
 * is refused before any state changes.  `SYS_XMEM_MAP`
 * returns the aperture token (the latch value mode 10 | Q = caller bank |
 * P = 7 - bank) and the object's aperture address, `SYS_SEG_MAP` the
 * token of a FAR_CODE segment at its fixed address 8000h; tokens
 * therefore never name bank 6 or bank 7 and never another process's
 * object or segment.
 *
 * Copies: every range is checked before a byte moves, then the transfer
 * goes chunk by chunk between the object and the common staging block.
 * Objects in slots 3-6 are reachable through the mode-11 window
 * (`kos_window_copy`), slots 0-2 through the mode-10 gate
 * (`kos_bank_copy`); the caller side goes through the syscall copy
 * helpers, which resolve the buffer-identity rule (a buffer behind the
 * aperture is read from and written to the far bank).
 *
 * The text above is the Mk1 board's.  On Mk2 (KOS_BOARD_MK2, architecture
 * 2.7.6) an object is a list of 8 KiB pages from the page allocator
 * (kernel/memory/mk2/pages.c), owned by the process; there are no slots or
 * classes.  A loader segment is the same object with its container id and
 * kind, its pages holding the segment from offset 0.  READ and WRITE move
 * the bytes directly between the object's pages and the caller's, the
 * caller's resolved through its current mapping, far window included.
 * MAP (a mappable object) and SEG_MAP (a far-code segment) fill the
 * object's row of the far table and return a one-byte mapping handle,
 * (index << 3) | generation, which the program's far window realizes
 * (kernel/memory/mk2/far.asm).
 */

#if defined(KOS_BOARD_MK2)

#include "kos/syscall.h"
#include "kos/xmem.h"
#include "kos/seg.h"
#include "kos/errno.h"
#include "process.h"
#include "exe.h"
#include "kernel/memory/mk2/pages.h"

#include "diag.h"
#include "xmem.h"

#define XMEM_MAX_OBJECTS 32u /* far.asm KOS_FAR_ROWS */
/* Generations 1..7: the object handle is (index << 4) | generation and
 * its mapping handle (index << 3) | generation, one byte (far.asm). */
#define XMEM_GENERATIONS 8u
#define XMEM_HANDLE_MAX  ((uint16_t)(XMEM_MAX_OBJECTS * 16u - 1u))
#define XMEM_MAX_PAGES   ((uint8_t)(KOS_XMEM_MAX_OBJECT >> KOS_DAT_PAGE_SHIFT))

/* A loader segment's `seg`: its EXE kind and container id. */
#define XMEM_SEG(kind, id) ((uint8_t)(((kind) << 4) | (id)))
#define XMEM_SEG_KIND(seg) ((uint8_t)((seg) >> 4))
#define XMEM_SEG_ID(seg)   ((uint8_t)((seg) & 0x0Fu))

struct xmem_object {
	uint8_t  used;
	uint8_t  pid;      /* owner */
	uint8_t  gen;      /* handle generation, 1..7 */
	uint8_t  count;    /* pages, 1..XMEM_MAX_PAGES */
	uint8_t  flags;    /* KOS_XMEM_FLAG_* */
	uint8_t  type;     /* KOS_XMEM_TYPE_* */
	uint16_t requested;/* bytes asked for (a segment: image + BSS) */
	uint8_t  pages[XMEM_MAX_PAGES]; /* object offset >> 13 -> page */
	uint8_t  seg;      /* SEGMENT: XMEM_SEG(kind, id); 0 otherwise */
};

static struct xmem_object xmem_table[XMEM_MAX_OBJECTS];

#if defined(__SDCC)
_Static_assert(sizeof(struct xmem_object) == 16,
	       "xmem object entry must be 16 packed bytes");
_Static_assert(XMEM_MAX_PAGES == 7u,
	       "an object is at most seven pages");
_Static_assert(KOS_XMEM_MAX_MAPPABLE == 2u * KOS_DAT_PAGE_SIZE,
	       "a mappable object fits the far window's two entries");
_Static_assert(sizeof(struct kos_far_row) == 3,
	       "far.asm KOS_FAR_ROW_SIZE");
_Static_assert(EXE_MAX_SEGMENTS <= 15u && EXE_SEG_KIND_DATA <= 15u,
	       "a segment's kind and id fit its seg byte");
_Static_assert(EXE_SEG_MAX_SIZE <= KOS_XMEM_MAX_OBJECT &&
	       EXE_SEG_MAX_FAR_SIZE == KOS_XMEM_MAX_MAPPABLE,
	       "every segment fits an object, a far one the far window");
_Static_assert(EXE_SEG_KIND_FAR_CODE == KOS_SEG_KIND_FAR_CODE &&
	       EXE_SEG_KIND_DATA == KOS_SEG_KIND_DATA,
	       "segment kinds must match the ABI");
#endif

static struct xmem_object *xmem_lookup(uint16_t handle, uint8_t pid)
{
	struct xmem_object *obj;

	if (handle == 0u || handle > XMEM_HANDLE_MAX) {
		return (struct xmem_object *)0;
	}
	obj = &xmem_table[(uint8_t)(handle >> 4)];
	if (obj->used == 0u || obj->pid != pid ||
	    (uint8_t)(handle & 0x0Fu) != obj->gen) {
		return (struct xmem_object *)0;
	}
	return obj;
}

/* The first free entry, or XMEM_MAX_OBJECTS. */
static uint8_t xmem_free_index(void)
{
	uint8_t index;

	for (index = 0u; index < XMEM_MAX_OBJECTS; ++index) {
		if (xmem_table[index].used == 0u) {
			break;
		}
	}
	return index;
}

/* Take `size` bytes of pages for `pid` into entry `index` (all or none),
 * the next generation, and no mapping yet.  0 or -KOS_ENOSPC. */
static int xmem_take(uint8_t index, uint8_t pid, uint16_t size)
{
	struct xmem_object *obj = &xmem_table[index];
	uint8_t count = (uint8_t)((size >> KOS_DAT_PAGE_SHIFT) +
				  ((size & (KOS_DAT_PAGE_SIZE - 1u)) != 0u));

	if (kos_pages_alloc(pid, count, obj->pages) != 0) {
		return -KOS_ENOSPC;
	}
	obj->gen = (uint8_t)(obj->gen + 1u);
	if (obj->gen >= XMEM_GENERATIONS) {
		obj->gen = 1u;
	}
	obj->used = 1u;
	obj->pid = pid;
	obj->count = count;
	obj->requested = size;
	obj->seg = 0u;
	kos_far_table[index].gen = 0u;
	return 0;
}

/* The far window's view of entry `index` - its first two pages, or its
 * one page and the owner's own A000h page - goes into its far-table row;
 * returns the mapping handle naming that row. */
static uint16_t xmem_far_row(uint8_t index)
{
	struct xmem_object *obj = &xmem_table[index];
	struct kos_far_row *row = &kos_far_table[index];

	row->pages[0] = obj->pages[0];
	row->pages[1] = (obj->count > 1u)
		? obj->pages[1]
		: kos_dat_images[obj->pid][KOS_DAT_FAR_ENTRY -
					   KOS_DAT_ENTRIES + 1u];
	row->gen = obj->gen;
	return (uint16_t)((uint16_t)(index << 3) | obj->gen);
}

/* Release one object: its pages, its far-table row, its entry. */
static void xmem_drop(uint8_t index)
{
	struct xmem_object *obj = &xmem_table[index];

	kos_pages_free(obj->pid, obj->pages, obj->count);
	kos_far_table[index].gen = 0u;
	obj->used = 0u;
}

void kos_xmem_alloc(uint16_t block)
{
	struct kos_xmem_alloc_args *args = (struct kos_xmem_alloc_args *)block;
	struct xmem_object *obj;
	uint8_t pid = process_current_pid();
	uint8_t index;
	uint16_t size;
	uint16_t flags;

	if (!kos_syscall_block_ok(block, sizeof(*args)) ||
	    pid >= PROCESS_TABLE_SIZE) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	size = args->size;
	flags = args->flags;
	if (size == 0u ||
	    (flags & (uint16_t)~(uint16_t)KOS_XMEM_FLAG_MAPPABLE) != 0u ||
	    ((flags & KOS_XMEM_FLAG_MAPPABLE) != 0u &&
	     size > KOS_XMEM_MAX_MAPPABLE)) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	index = xmem_free_index();
	if (size > KOS_XMEM_MAX_OBJECT || index == XMEM_MAX_OBJECTS ||
	    xmem_take(index, pid, size) != 0) {
		kos_syscall_error(KOS_ENOSPC);
		return;
	}
	obj = &xmem_table[index];
	obj->flags = (uint8_t)flags;
	obj->type = KOS_XMEM_TYPE_RUNTIME;
	kos_syscall_success((uint16_t)(((uint16_t)index << 4) | obj->gen));
}

/* One loader segment (kernel/process/exec.c), placed before the program
 * runs: an object of type SEGMENT holding the segment from offset 0 -
 * where a far-code segment and a mappable data segment link 8000h, the
 * far window's base.  A mappable DATA row's object is MAPPABLE (XMEM_MAP
 * opens it); a FAR_CODE row's is opened by SEG_MAP.  The pages are copied
 * to `pages` (the loader's buffer: common or stack memory, seen from this
 * cold set).  0, -KOS_EINVAL or -KOS_ENOSPC; nothing is held on failure. */
int kos_xmem_segment_alloc(uint8_t pid, uint8_t id, uint8_t kind,
			   uint16_t flags, uint16_t size, uint8_t *pages)
{
	struct xmem_object *obj;
	uint8_t index;
	uint8_t i;

	if (pid >= PROCESS_TABLE_SIZE || id == 0u || id > EXE_MAX_SEGMENTS ||
	    size == 0u || (flags & ~(uint16_t)EXE_SEG_FLAG_MAPPABLE) != 0u ||
	    (kind != EXE_SEG_KIND_DATA &&
	     (kind != EXE_SEG_KIND_FAR_CODE || flags != 0u)) ||
	    size > ((kind == EXE_SEG_KIND_FAR_CODE || flags != 0u)
			    ? EXE_SEG_MAX_FAR_SIZE : EXE_SEG_MAX_SIZE)) {
		return -KOS_EINVAL;
	}
	index = xmem_free_index();
	if (index == XMEM_MAX_OBJECTS || xmem_take(index, pid, size) != 0) {
		return -KOS_ENOSPC;
	}
	obj = &xmem_table[index];
	obj->flags = (flags != 0u) ? KOS_XMEM_FLAG_MAPPABLE
				   : KOS_XMEM_FLAG_NONE;
	obj->type = KOS_XMEM_TYPE_SEGMENT;
	obj->seg = XMEM_SEG(kind, id);
	for (i = 0u; i < obj->count; ++i) {
		pages[i] = obj->pages[i];
	}
	return 0;
}

/* The live segment `id` of `pid`, its entry index in `*index`, or null. */
static struct xmem_object *xmem_segment_find(uint8_t pid, uint8_t id,
					     uint8_t *index)
{
	uint8_t i;

	for (i = 0u; i < XMEM_MAX_OBJECTS; ++i) {
		struct xmem_object *obj = &xmem_table[i];

		if (obj->used != 0u && obj->pid == pid && obj->seg != 0u &&
		    XMEM_SEG_ID(obj->seg) == id) {
			*index = i;
			return obj;
		}
	}
	return (struct xmem_object *)0;
}

void kos_xmem_free(uint16_t block)
{
	struct kos_xmem_free_args *args = (struct kos_xmem_free_args *)block;
	struct xmem_object *obj;

	if (!kos_syscall_block_ok(block, sizeof(*args))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	/* A loader segment is released at exit only. */
	obj = xmem_lookup(args->handle, process_current_pid());
	if (obj == (struct xmem_object *)0 ||
	    obj->type != KOS_XMEM_TYPE_RUNTIME) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	xmem_drop((uint8_t)(args->handle >> 4));
	kos_syscall_success(0);
}

/* SYS_XMEM_READ (`to_caller` set) and SYS_XMEM_WRITE: every range is
 * checked, then one direct copy between the two page lists. */
static void xmem_transfer(uint16_t block, uint8_t to_caller)
{
	struct kos_xmem_io_args *args = (struct kos_xmem_io_args *)block;
	struct xmem_object *obj;
	uint16_t allocated;
	uint16_t offset;
	uint16_t buffer;
	uint16_t length;

	if (!kos_syscall_block_ok(block, sizeof(*args))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	obj = xmem_lookup(args->handle, process_current_pid());
	offset = args->offset;
	length = args->length;
	buffer = args->buffer;
	if (obj == (struct xmem_object *)0 ||
	    (length != 0u && !kos_syscall_buffer_ok(buffer, length))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	allocated = (uint16_t)((uint16_t)obj->count << KOS_DAT_PAGE_SHIFT);
	if (offset > allocated ||
	    length > (uint16_t)(allocated - offset)) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	if (to_caller != 0u) {
		kos_pages_copy(obj->pages, offset, kos_caller_map(), buffer,
			       length);
	} else {
		kos_pages_copy(kos_caller_map(), buffer, obj->pages, offset,
			       length);
	}
	kos_syscall_success(length);
}

void kos_xmem_read(uint16_t block)
{
	xmem_transfer(block, 1u);
}

void kos_xmem_write(uint16_t block)
{
	xmem_transfer(block, 0u);
}

void kos_xmem_info(uint16_t block)
{
	struct kos_xmem_info_args *args = (struct kos_xmem_info_args *)block;
	struct xmem_object *obj;
	uint8_t record[8];
	uint16_t allocated;

	if (!kos_syscall_block_ok(block, sizeof(*args)) ||
	    !kos_syscall_buffer_ok(args->buffer,
				   (uint16_t)sizeof(struct kos_xmem_info))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	obj = xmem_lookup(args->handle, process_current_pid());
	if (obj == (struct xmem_object *)0) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	allocated = (uint16_t)((uint16_t)obj->count << KOS_DAT_PAGE_SHIFT);
	record[0] = (uint8_t)obj->requested;
	record[1] = (uint8_t)(obj->requested >> 8);
	record[2] = (uint8_t)allocated;
	record[3] = (uint8_t)(allocated >> 8);
	record[4] = obj->flags;
	record[5] = 0u;
	record[6] = obj->type;
	record[7] = 0u;
	kos_pages_copy((const uint8_t *)0, (uint16_t)record, kos_caller_map(),
		       args->buffer, (uint16_t)sizeof(record));
	kos_syscall_success(0);
}

/* SYS_XMEM_MAP: the object's far-window row, and the mapping handle
 * naming it with the window's address. */
void kos_xmem_map(uint16_t block)
{
	struct kos_xmem_map_args *args = (struct kos_xmem_map_args *)block;
	struct xmem_object *obj;

	if (!kos_syscall_block_ok(block, sizeof(*args))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	obj = xmem_lookup(args->handle, process_current_pid());
	if (obj == (struct xmem_object *)0 ||
	    (obj->flags & KOS_XMEM_FLAG_MAPPABLE) == 0u) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	args->address = KOS_XMEM_APERTURE_BASE;
	kos_syscall_success(xmem_far_row((uint8_t)(args->handle >> 4)));
}

/* SYS_SEG_INFO: one of the caller's own segments by container id; a DATA
 * segment reports its object handle, a FAR_CODE segment 0. */
void kos_seg_info(uint16_t block)
{
	struct kos_seg_info_args *args = (struct kos_seg_info_args *)block;
	struct xmem_object *obj;
	uint8_t record[8];
	uint8_t index = 0u;
	uint16_t handle = 0u;

	if (!kos_syscall_block_ok(block, sizeof(*args)) ||
	    !kos_syscall_buffer_ok(args->buffer,
				   (uint16_t)sizeof(struct kos_seg_info))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	obj = xmem_segment_find(process_current_pid(), args->segment, &index);
	if (obj == (struct xmem_object *)0) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	if (XMEM_SEG_KIND(obj->seg) == EXE_SEG_KIND_DATA) {
		handle = (uint16_t)(((uint16_t)index << 4) | obj->gen);
	}
	record[0] = (uint8_t)obj->requested;
	record[1] = (uint8_t)(obj->requested >> 8);
	record[2] = XMEM_SEG_KIND(obj->seg);
	record[3] = KOS_SEG_STATE_LOADED;
	record[4] = (uint8_t)handle;
	record[5] = (uint8_t)(handle >> 8);
	record[6] = obj->flags;
	record[7] = 0u;
	kos_pages_copy((const uint8_t *)0, (uint16_t)record, kos_caller_map(),
		       args->buffer, (uint16_t)sizeof(record));
	kos_syscall_success(0);
}

/* SYS_SEG_MAP: the mapping handle of one of the caller's own far-code
 * segments, always at 8000h. */
void kos_seg_map(uint16_t block)
{
	struct kos_seg_map_args *args = (struct kos_seg_map_args *)block;
	struct xmem_object *obj;
	uint8_t index = 0u;

	if (!kos_syscall_block_ok(block, sizeof(*args))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	obj = xmem_segment_find(process_current_pid(), args->segment, &index);
	if (obj == (struct xmem_object *)0 ||
	    XMEM_SEG_KIND(obj->seg) != EXE_SEG_KIND_FAR_CODE) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	kos_syscall_success(xmem_far_row(index));
}

void kos_xmem_dispatch(uint8_t number, uint16_t block)
{
	switch (number) {
	case KOS_SYSCALL_XMEM_ALLOC:
		kos_xmem_alloc(block);
		return;
	case KOS_SYSCALL_XMEM_FREE:
		kos_xmem_free(block);
		return;
	case KOS_SYSCALL_XMEM_READ:
		kos_xmem_read(block);
		return;
	case KOS_SYSCALL_XMEM_WRITE:
		kos_xmem_write(block);
		return;
	case KOS_SYSCALL_XMEM_INFO:
		kos_xmem_info(block);
		return;
	case KOS_SYSCALL_XMEM_MAP:
		kos_xmem_map(block);
		return;
	case KOS_SYSCALL_SEG_INFO:
		kos_seg_info(block);
		return;
	case KOS_SYSCALL_SEG_MAP:
		kos_seg_map(block);
		return;
	default:
		kos_syscall_error(KOS_ENOSYS);
		return;
	}
}

void kos_xmem_release_process(uint8_t pid)
{
	uint8_t index;

	for (index = 0u; index < XMEM_MAX_OBJECTS; ++index) {
		if (xmem_table[index].used != 0u &&
		    xmem_table[index].pid == pid) {
			xmem_drop(index);
		}
	}
}

#else /* Mk1 */

#include "kos/syscall.h"
#include "kos/xmem.h"
#include "kos/seg.h"
#include "kernel/syscall/rt.h"
#include "kos/errno.h"
#include "platform.h"
#include "process.h"
#include "exe.h"

#include "kernel/ipc/ipc.h"

#include "diag.h"
#include "xmem.h"

/* The public segment kinds are the container's kinds, pinned once. */
#if defined(__SDCC)
_Static_assert(EXE_SEG_KIND_FAR_CODE == KOS_SEG_KIND_FAR_CODE,
	       "segment kind FAR_CODE must match the ABI");
_Static_assert(EXE_SEG_KIND_DATA == KOS_SEG_KIND_DATA,
	       "segment kind DATA must match the ABI");
#endif

/* One table entry per live object, system-wide (architecture 4.8): every
 * process's runtime objects and loader segments count, a suspended
 * parent's included.  The handle packs the index above the four
 * generation bits, so 32 entries make a 9-bit handle. */
#define XMEM_MAX_OBJECTS 32u
#define XMEM_GENERATIONS 16u /* generations 1..15 cycle per entry */
#define XMEM_HANDLE_MAX  ((uint16_t)(XMEM_MAX_OBJECTS * XMEM_GENERATIONS - 1u))

/* The split between the two copy paths (architecture 4.8): slots 0-2
 * live at 0000h-5FFFh and use the mode-10 gate, slots 3-6 at
 * 6000h-DFFFh and use the mode-11 window. */
#define XMEM_WINDOW_BASE 0x6000u

struct xmem_object {
	uint8_t  used;
	uint8_t  pid;      /* owner, PROCESS_NO_PROCESS while free */
	uint8_t  gen;      /* handle generation, 1..15 */
	uint8_t  bank;
	uint8_t  slot;     /* first slot, 0-6 */
	uint8_t  slots;    /* whole slots, 1-4 */
	uint8_t  flags;    /* KOS_XMEM_FLAG_* */
	uint8_t  type;     /* KOS_XMEM_TYPE_* */
	uint8_t  seg_id;   /* SEGMENT: the EXE container id, 1..N */
	uint8_t  seg_kind; /* SEGMENT: EXE_SEG_KIND_* */
	uint16_t requested;/* logical bytes asked for */
};

static struct xmem_object xmem_table[XMEM_MAX_OBJECTS];

#if defined(__SDCC)
_Static_assert(sizeof(struct xmem_object) == 12,
	       "xmem object entry must be 12 packed bytes");
_Static_assert(XMEM_HANDLE_MAX == 0x1FFu,
	       "an xmem handle is 9 bits: index 0-31, generation 1-15");
#endif

/* The object `handle` names for `pid`, or null for a dead, stale or
 * foreign handle. */
static struct xmem_object *xmem_lookup(uint16_t handle, uint8_t pid)
{
	uint8_t index = (uint8_t)(handle >> 4);
	struct xmem_object *obj;

	if (handle == 0u || handle > XMEM_HANDLE_MAX) {
		return (struct xmem_object *)0;
	}
	obj = &xmem_table[index];
	if (obj->used == 0u || obj->pid != pid ||
	    (uint8_t)(handle & 0x0Fu) != obj->gen) {
		return (struct xmem_object *)0;
	}
	return obj;
}

static uint16_t xmem_object_address(const struct xmem_object *obj,
				    uint16_t offset)
{
	return (uint16_t)(KOS_AUX_SLOT_BASE(obj->slot) + offset);
}

/* One object-side chunk: the window for slots 3-6, the mode-10 gate for
 * slots 0-2.  The caller keeps every chunk inside one of the two regions
 * (they are split at 6000h). */
static uint8_t xmem_object_move(const struct xmem_object *obj, uint16_t addr,
				uint16_t length, uint8_t op)
{
	struct kos_bank_copy_req req;

	req.bank = obj->bank;
	req.op = op;
	req.addr = addr;
	req.buffer = kos_user_copy_staging;
	req.length = length;
	return kos_aux_copy(&req);
}

/* A chunk that stays inside one copy region. */
static uint16_t xmem_chunk(uint16_t addr, uint16_t length)
{
	uint16_t chunk = length;

	if (chunk > KOS_SYSCALL_STAGE_SIZE) {
		chunk = KOS_SYSCALL_STAGE_SIZE;
	}
	if (addr < XMEM_WINDOW_BASE &&
	    (uint16_t)(addr + chunk) > XMEM_WINDOW_BASE) {
		chunk = (uint16_t)(XMEM_WINDOW_BASE - addr);
	}
	return chunk;
}

void kos_xmem_alloc(uint16_t block)
{
	struct kos_xmem_alloc_args *args = (struct kos_xmem_alloc_args *)block;
	struct xmem_object *obj;
	uint8_t pid = process_current_pid();
	uint8_t mappable;
	uint8_t slots;
	uint8_t index;
	uint8_t bank = 0u;
	uint8_t slot = 0u;
	uint16_t size;
	uint16_t flags;

	if (!kos_syscall_block_ok(block, sizeof(*args)) ||
	    pid >= PROCESS_TABLE_SIZE) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	size = args->size;
	flags = args->flags;
	if (size == 0u ||
	    (flags & (uint16_t)~(uint16_t)KOS_XMEM_FLAG_MAPPABLE) != 0u) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	mappable = (uint8_t)((flags & KOS_XMEM_FLAG_MAPPABLE) != 0u);
	slots = (uint8_t)(((uint32_t)size + (KOS_XMEM_SLOT_SIZE - 1u)) >>
			  13);
	if (mappable != 0u) {
		if (size > KOS_XMEM_MAX_MAPPABLE) {
			kos_syscall_error(KOS_EINVAL);
			return;
		}
	} else if (slots > 4u) {
		/* No bank can hold more than the four non-aperture data
		 * slots (the aperture slice takes mappable objects only). */
		kos_syscall_error(KOS_ENOSPC);
		return;
	}
	for (index = 0u; index < XMEM_MAX_OBJECTS; ++index) {
		if (xmem_table[index].used == 0u) {
			break;
		}
	}
	if (index == XMEM_MAX_OBJECTS) {
		kos_syscall_error(KOS_ENOSPC);
		return;
	}
	if (process_aux_alloc(pid, slots,
			      mappable != 0u ? KOS_AUX_PLACE_MAPPABLE
					     : KOS_AUX_PLACE_DATA,
			      &bank, &slot) != 0) {
		kos_syscall_error(KOS_ENOSPC);
		return;
	}
	obj = &xmem_table[index];
	obj->gen = (uint8_t)(obj->gen + 1u);
	if (obj->gen >= XMEM_GENERATIONS) {
		obj->gen = 1u;
	}
	obj->used = 1u;
	obj->pid = pid;
	obj->bank = bank;
	obj->slot = slot;
	obj->slots = slots;
	obj->flags = flags;
	obj->type = KOS_XMEM_TYPE_RUNTIME;
	obj->seg_id = 0u;
	obj->seg_kind = 0u;
	obj->requested = size;
	kos_syscall_success((uint16_t)(((uint16_t)index << 4) | obj->gen));
}

/* One loader-created segment (kernel/process/exec.c): allocate its slots
 * and register the object before the child bank is touched, so a failure
 * is a clean rollback through process_slot_release().  A plain `DATA`
 * segment is non-mappable - it is reached by handle - and the caller
 * receives the placement it copies the payload into and zeroes the BSS
 * at.  A `FAR_CODE` segment is linked at 8000h, so it is placed in slot 4
 * (slots 4-5 when it is over 8 KiB): one far-code segment per auxiliary
 * bank, and it has no handle (SYS_SEG_MAP returns its aperture token).  A
 * `DATA` segment that asked for `EXE_SEG_FLAG_MAPPABLE` is
 * placed exactly the same way - its image is at 8000h where the link put
 * it - and its object carries `KOS_XMEM_FLAG_MAPPABLE`, so the program
 * opens the aperture onto it with SYS_XMEM_MAP; it keeps its handle. */
int kos_xmem_segment_alloc(uint8_t pid, uint8_t id, uint8_t kind,
			   uint16_t flags, uint16_t size, uint16_t *handle,
			   uint8_t *bank, uint8_t *slot)
{
	struct xmem_object *obj;
	uint8_t place;
	uint8_t slots;
	uint8_t index;

	if (handle == (uint16_t *)0 || bank == (uint8_t *)0 ||
	    slot == (uint8_t *)0 || pid >= PROCESS_TABLE_SIZE ||
	    id == 0u || size == 0u ||
	    (flags & ~(uint16_t)EXE_SEG_FLAG_MAPPABLE) != 0u) {
		return -KOS_EINVAL;
	}
	if (kind == EXE_SEG_KIND_DATA) {
		if (flags != 0u) {
			if (size > EXE_SEG_MAX_FAR_SIZE) {
				return -KOS_EINVAL;
			}
			place = KOS_AUX_PLACE_FAR;
		} else {
			if (size > EXE_SEG_MAX_SIZE) {
				return -KOS_EINVAL;
			}
			place = KOS_AUX_PLACE_DATA;
		}
	} else if (kind == EXE_SEG_KIND_FAR_CODE) {
		if (flags != 0u || size > EXE_SEG_MAX_FAR_SIZE) {
			return -KOS_EINVAL;
		}
		place = KOS_AUX_PLACE_FAR;
	} else {
		return -KOS_EINVAL;
	}
	slots = (uint8_t)(((uint32_t)size + (KOS_XMEM_SLOT_SIZE - 1u)) >>
			  13);
	for (index = 0u; index < XMEM_MAX_OBJECTS; ++index) {
		if (xmem_table[index].used == 0u) {
			break;
		}
	}
	if (index == XMEM_MAX_OBJECTS) {
		return -KOS_ENOSPC;
	}
	if (process_aux_alloc(pid, slots, place, bank, slot) != 0) {
		return -KOS_ENOSPC;
	}
	obj = &xmem_table[index];
	obj->gen = (uint8_t)(obj->gen + 1u);
	if (obj->gen >= XMEM_GENERATIONS) {
		obj->gen = 1u;
	}
	obj->used = 1u;
	obj->pid = pid;
	obj->bank = *bank;
	obj->slot = *slot;
	obj->slots = slots;
	obj->flags = (flags != 0u) ? KOS_XMEM_FLAG_MAPPABLE
				   : KOS_XMEM_FLAG_NONE;
	obj->type = KOS_XMEM_TYPE_SEGMENT;
	obj->seg_id = id;
	obj->seg_kind = kind;
	obj->requested = size;
	*handle = (kind == EXE_SEG_KIND_DATA)
		? (uint16_t)(((uint16_t)index << 4) | obj->gen)
		: 0u;
	return 0;
}

/* The live segment object of container id `id` owned by `pid`, plus its
 * table index, or null. */
static struct xmem_object *xmem_segment_find(uint8_t pid, uint8_t id,
					     uint8_t *index)
{
	uint8_t i;

	if (id == 0u) {
		return (struct xmem_object *)0;
	}
	for (i = 0u; i < XMEM_MAX_OBJECTS; ++i) {
		struct xmem_object *obj = &xmem_table[i];

		if (obj->used != 0u && obj->pid == pid &&
		    obj->type == KOS_XMEM_TYPE_SEGMENT &&
		    obj->seg_id == id) {
			*index = i;
			return obj;
		}
	}
	return (struct xmem_object *)0;
}

void kos_xmem_free(uint16_t block)
{
	struct kos_xmem_free_args *args = (struct kos_xmem_free_args *)block;
	struct xmem_object *obj;

	if (!kos_syscall_block_ok(block, sizeof(*args))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	obj = xmem_lookup(args->handle, process_current_pid());
	if (obj == (struct xmem_object *)0 ||
	    obj->type != KOS_XMEM_TYPE_RUNTIME) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	process_aux_free(obj->pid, obj->bank, obj->slot, obj->slots);
	obj->used = 0u;
	kos_syscall_success(0);
}

/* SYS_XMEM_READ (op KOS_BANK_OP_READ) and SYS_XMEM_WRITE (op
 * KOS_BANK_OP_WRITE) share one bounded, checked transfer. */
static void xmem_transfer(uint16_t block, uint8_t op)
{
	struct kos_xmem_io_args *args = (struct kos_xmem_io_args *)block;
	struct xmem_object *obj;
	uint16_t allocated;
	uint16_t offset;
	uint16_t buffer;
	uint16_t length;
	uint16_t done = 0u;

	if (!kos_syscall_block_ok(block, sizeof(*args))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	obj = xmem_lookup(args->handle, process_current_pid());
	offset = args->offset;
	length = args->length;
	buffer = args->buffer;
	if (obj == (struct xmem_object *)0 ||
	    (length != 0u && !kos_syscall_buffer_ok(buffer, length))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	allocated = (uint16_t)(obj->slots * KOS_XMEM_SLOT_SIZE);
	if (offset > allocated ||
	    length > (uint16_t)(allocated - offset)) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	while (done < length) {
		uint16_t addr = xmem_object_address(obj,
						    (uint16_t)(offset + done));
		uint16_t chunk = xmem_chunk(addr, (uint16_t)(length - done));

		if (op == KOS_BANK_OP_READ) {
			if (xmem_object_move(obj, addr, chunk,
					     KOS_BANK_OP_READ) != 0u ||
			    kos_syscall_bank_write(
				    (uint16_t)(buffer + done), chunk) != 0u) {
				kos_syscall_error(KOS_EFAULT);
				return;
			}
		} else {
			if (kos_syscall_bank_read(
				    (uint16_t)(buffer + done), chunk) != 0u ||
			    xmem_object_move(obj, addr, chunk,
					     KOS_BANK_OP_WRITE) != 0u) {
				kos_syscall_error(KOS_EFAULT);
				return;
			}
		}
		done = (uint16_t)(done + chunk);
	}
	kos_syscall_success(length);
}

void kos_xmem_read(uint16_t block)
{
	xmem_transfer(block, KOS_BANK_OP_READ);
}

void kos_xmem_write(uint16_t block)
{
	xmem_transfer(block, KOS_BANK_OP_WRITE);
}

/* SYS_XMEM_INFO: the logical description only - never a bank or slot. */
void kos_xmem_info(uint16_t block)
{
	struct kos_xmem_info_args *args = (struct kos_xmem_info_args *)block;
	struct xmem_object *obj;
	uint8_t *record = kos_user_copy_staging;
	uint16_t allocated;

	if (!kos_syscall_block_ok(block, sizeof(*args)) ||
	    !kos_syscall_buffer_ok(args->buffer,
				   (uint16_t)sizeof(struct kos_xmem_info))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	obj = xmem_lookup(args->handle, process_current_pid());
	if (obj == (struct xmem_object *)0) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	allocated = (uint16_t)(obj->slots * KOS_XMEM_SLOT_SIZE);
	record[0] = (uint8_t)obj->requested;
	record[1] = (uint8_t)(obj->requested >> 8);
	record[2] = (uint8_t)allocated;
	record[3] = (uint8_t)(allocated >> 8);
	record[4] = (uint8_t)obj->flags;
	record[5] = 0u;
	record[6] = obj->type;
	record[7] = 0u;
	if (kos_syscall_bank_write(args->buffer,
				   (uint16_t)sizeof(struct kos_xmem_info)) !=
	    0u) {
		kos_syscall_error(KOS_EFAULT);
		return;
	}
	kos_syscall_success(0);
}

/* SYS_XMEM_MAP: the token that opens the aperture onto the object plus the
 * object's address in it.  Only the caller's own mappable objects have
 * one; the token keeps the caller's bank field and selects P = 7 - bank,
 * so it can never show bank 6 or bank 7. */
void kos_xmem_map(uint16_t block)
{
	struct kos_xmem_map_args *args = (struct kos_xmem_map_args *)block;
	struct xmem_object *obj;
	uint8_t latch;
	uint8_t token;

	if (!kos_syscall_block_ok(block, sizeof(*args))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	obj = xmem_lookup(args->handle, process_current_pid());
	if (obj == (struct xmem_object *)0 ||
	    (obj->flags & KOS_XMEM_FLAG_MAPPABLE) == 0u) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	latch = ipc_gate_latch();
	token = (uint8_t)((latch & KOS_BANK_MASK) | KOS_MODE_APPLICATION |
			  (uint8_t)((7u - obj->bank) << KOS_ROM_PAGE_SHIFT));
	args->address = (uint16_t)(KOS_XMEM_APERTURE_BASE +
				   (uint16_t)(obj->slot - 4u) *
					   KOS_XMEM_SLOT_SIZE);
	kos_syscall_success(token);
}

/* SYS_SEG_INFO (26h): the running process's own EXE v2 segments, named by
 * their container ids.  A DATA segment reports its extended-memory handle
 * (the abi/kos/xmem.h calls reach it); a FAR_CODE segment has none (it is
 * executed in place through the aperture, not copied) and reports 0.  A
 * live segment of a loaded container is LOADED by construction - the
 * loader places every segment or the exec fails and rolls back. */
void kos_seg_info(uint16_t block)
{
	struct kos_seg_info_args *args = (struct kos_seg_info_args *)block;
	struct xmem_object *obj;
	uint8_t *record = kos_user_copy_staging;
	uint8_t index = 0u;
	uint16_t handle = 0u;

	if (!kos_syscall_block_ok(block, sizeof(*args)) ||
	    !kos_syscall_buffer_ok(args->buffer,
				   (uint16_t)sizeof(struct kos_seg_info))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	obj = xmem_segment_find(process_current_pid(), args->segment, &index);
	if (obj == (struct xmem_object *)0) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	if (obj->seg_kind == EXE_SEG_KIND_DATA) {
		handle = (uint16_t)(((uint16_t)index << 4) | obj->gen);
	}
	record[0] = (uint8_t)obj->requested;
	record[1] = (uint8_t)(obj->requested >> 8);
	record[2] = obj->seg_kind;
	record[3] = KOS_SEG_STATE_LOADED;
	record[4] = (uint8_t)handle;
	record[5] = (uint8_t)(handle >> 8);
	record[6] = obj->flags;
	record[7] = 0u; /* the ABI flags word is 16 bits, the object's 8 */
	if (kos_syscall_bank_write(args->buffer,
				   (uint16_t)sizeof(struct kos_seg_info)) !=
	    0u) {
		kos_syscall_error(KOS_EFAULT);
		return;
	}
	kos_syscall_success(0);
}

/* SYS_SEG_MAP (27h): the token that opens the aperture onto one of the
 * caller's own FAR_CODE segments.  The token keeps the caller's bank field
 * and selects P = 7 - bank, so it can never show bank 6 or bank 7; the
 * segment is always at 8000h (its linked address), so no address is
 * returned.  A DATA segment has no token - it is reached by handle. */
void kos_seg_map(uint16_t block)
{
	struct kos_seg_map_args *args = (struct kos_seg_map_args *)block;
	struct xmem_object *obj;
	uint8_t index = 0u;
	uint8_t latch;
	uint8_t token;

	if (!kos_syscall_block_ok(block, sizeof(*args))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	obj = xmem_segment_find(process_current_pid(), args->segment, &index);
	if (obj == (struct xmem_object *)0 ||
	    obj->seg_kind != EXE_SEG_KIND_FAR_CODE) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	latch = ipc_gate_latch();
	token = (uint8_t)((latch & KOS_BANK_MASK) | KOS_MODE_APPLICATION |
			  (uint8_t)((7u - obj->bank) << KOS_ROM_PAGE_SHIFT));
	kos_syscall_success(token);
}

void kos_xmem_dispatch(uint8_t number, uint16_t block)
{
	switch (number) {
	case KOS_SYSCALL_XMEM_ALLOC:
		kos_xmem_alloc(block);
		return;
	case KOS_SYSCALL_XMEM_FREE:
		kos_xmem_free(block);
		return;
	case KOS_SYSCALL_XMEM_READ:
		kos_xmem_read(block);
		return;
	case KOS_SYSCALL_XMEM_WRITE:
		kos_xmem_write(block);
		return;
	case KOS_SYSCALL_XMEM_INFO:
		kos_xmem_info(block);
		return;
	case KOS_SYSCALL_XMEM_MAP:
		kos_xmem_map(block);
		return;
	case KOS_SYSCALL_SEG_INFO:
		kos_seg_info(block);
		return;
	case KOS_SYSCALL_SEG_MAP:
		kos_seg_map(block);
		return;
	default:
		kos_syscall_error(KOS_ENOSYS);
		return;
	}
}

void kos_xmem_release_process(uint8_t pid)
{
	uint8_t index;

	for (index = 0u; index < XMEM_MAX_OBJECTS; ++index) {
		struct xmem_object *obj = &xmem_table[index];

		if (obj->used != 0u && obj->pid == pid) {
			process_aux_free(pid, obj->bank, obj->slot,
					 obj->slots);
			obj->used = 0u;
		}
	}
}

#endif /* Mk1 */
