/* ZephyrOS extended-memory syscall services (architecture 36.15-36.18,
 * work packages 43, 44a and 44b).
 *
 * Implements SYS_XMEM_ALLOC / _FREE / _READ / _WRITE / _INFO / _MAP,
 * SYS_SEG_INFO and SYS_SEG_MAP.  A runtime object is process-owned,
 * read/write and non-executable; a loader-created ZEX segment is the same
 * object with type ZOS_XMEM_TYPE_SEGMENT and a container id - a `DATA`
 * segment is reached by handle, a `FAR_CODE` segment is executed in place
 * through the aperture and its map token comes from SYS_SEG_MAP.  The
 * size is rounded up to whole 8-KiB auxiliary slots and the object is
 * contiguous inside one bank (kernel/process/process.c owns the slot map).
 * **Contents are undefined**: nothing here clears a slot (36.19) - a
 * previous object's bytes are what a new one starts with.
 *
 * Handles are opaque and owner-tagged: `(index << 4) | generation`, so a
 * freed-and-reused table entry hands out a different handle and a stale
 * or foreign handle is refused before any state changes.  `SYS_XMEM_MAP`
 * returns the aperture token (the latch value mode 10 | Q = caller bank |
 * P = 7 - bank) and the object's aperture address, `SYS_SEG_MAP` the
 * token of a FAR_CODE segment at its fixed address 8000h; tokens
 * therefore never name bank 6 or bank 7 and never another process's
 * object or segment.
 *
 * Copies: every range is checked before a byte moves, then the transfer
 * goes chunk by chunk between the object and the common staging block.
 * Objects in slots 3-6 are reachable through the mode-11 window
 * (`zos_window_copy`), slots 0-2 through the mode-10 gate
 * (`zos_bank_copy`); the caller side goes through the syscall copy
 * helpers, which resolve the buffer-identity rule (a buffer behind the
 * aperture is read from and written to the far bank).
 */

#include "zos/syscall.h"
#include "zos/xmem.h"
#include "zos/seg.h"
#include "zephyros/errno.h"
#include "zephyros/platform.h"
#include "zephyros/process.h"
#include "zephyros/zex.h"

#include "kernel/ipc/ipc.h"

#include "diag.h"
#include "xmem.h"

/* The public segment kinds are the container's kinds, pinned once. */
#if defined(__SDCC)
_Static_assert(ZEX_SEG_KIND_FAR_CODE == ZOS_SEG_KIND_FAR_CODE,
	       "segment kind FAR_CODE must match the ABI");
_Static_assert(ZEX_SEG_KIND_DATA == ZOS_SEG_KIND_DATA,
	       "segment kind DATA must match the ABI");
#endif

/* One table entry per live object; the acceptance image holds several
 * objects per bank plus a full bank map, so sixteen entries cover a whole
 * slot map.  The handle packs the index into the top four bits, so the
 * capacity is fixed at XMEM_GENERATIONS entries. */
#define XMEM_MAX_OBJECTS 16u
#define XMEM_GENERATIONS 16u /* generations 1..15 cycle per entry */

/* The split between the two copy paths (architecture 36.16): slots 0-2
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
	uint8_t  flags;    /* ZOS_XMEM_FLAG_* */
	uint8_t  type;     /* ZOS_XMEM_TYPE_* */
	uint8_t  seg_id;   /* SEGMENT: the ZEX container id, 1..N */
	uint8_t  seg_kind; /* SEGMENT: ZEX_SEG_KIND_* */
	uint16_t requested;/* logical bytes asked for */
};

static struct xmem_object xmem_table[XMEM_MAX_OBJECTS];

#if defined(__SDCC)
_Static_assert(sizeof(struct xmem_object) == 12,
	       "xmem object entry must be 12 packed bytes");
#endif

/* The object `handle` names for `pid`, or null for a dead, stale or
 * foreign handle. */
static struct xmem_object *xmem_lookup(uint16_t handle, uint8_t pid)
{
	uint8_t index = (uint8_t)(handle >> 4);
	struct xmem_object *obj;

	if (handle == 0u || handle > 0xFFu) {
		return (struct xmem_object *)0;
	}
	if (index >= XMEM_MAX_OBJECTS) {
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
	return (uint16_t)(ZOS_AUX_SLOT_BASE(obj->slot) + offset);
}

/* One object-side chunk: the window for slots 3-6, the mode-10 gate for
 * slots 0-2.  The caller keeps every chunk inside one of the two regions
 * (they are split at 6000h). */
static uint8_t xmem_object_move(const struct xmem_object *obj, uint16_t addr,
				uint16_t length, uint8_t op)
{
	struct zos_bank_copy_req req;

	req.bank = obj->bank;
	req.op = op;
	req.addr = addr;
	req.buffer = zos_user_copy_staging;
	req.length = length;
	if (addr >= XMEM_WINDOW_BASE) {
		return zos_window_copy(&req);
	}
	return zos_bank_copy(&req);
}

/* A chunk that stays inside one copy region. */
static uint16_t xmem_chunk(uint16_t addr, uint16_t length)
{
	uint16_t chunk = length;

	if (chunk > ZOS_SYSCALL_STAGE_SIZE) {
		chunk = ZOS_SYSCALL_STAGE_SIZE;
	}
	if (addr < XMEM_WINDOW_BASE &&
	    (uint16_t)(addr + chunk) > XMEM_WINDOW_BASE) {
		chunk = (uint16_t)(XMEM_WINDOW_BASE - addr);
	}
	return chunk;
}

void zos_xmem_alloc(uint16_t block)
{
	struct zos_xmem_alloc_args *args = (struct zos_xmem_alloc_args *)block;
	struct xmem_object *obj;
	uint8_t pid = process_current_pid();
	uint8_t mappable;
	uint8_t slots;
	uint8_t index;
	uint8_t bank = 0u;
	uint8_t slot = 0u;
	uint16_t size;
	uint16_t flags;

	if (!zos_syscall_block_ok(block, sizeof(*args)) ||
	    pid >= PROCESS_TABLE_SIZE) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	size = args->size;
	flags = args->flags;
	if (size == 0u ||
	    (flags & (uint16_t)~(uint16_t)ZOS_XMEM_FLAG_MAPPABLE) != 0u) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	mappable = (uint8_t)((flags & ZOS_XMEM_FLAG_MAPPABLE) != 0u);
	slots = (uint8_t)(((uint32_t)size + (ZOS_XMEM_SLOT_SIZE - 1u)) >>
			  13);
	if (mappable != 0u) {
		if (size > ZOS_XMEM_MAX_MAPPABLE) {
			zos_syscall_error(ZOS_EINVAL);
			return;
		}
	} else if (slots > 4u) {
		/* No bank can hold more than the four non-aperture data
		 * slots (the aperture slice takes mappable objects only). */
		zos_syscall_error(ZOS_ENOSPC);
		return;
	}
	for (index = 0u; index < XMEM_MAX_OBJECTS; ++index) {
		if (xmem_table[index].used == 0u) {
			break;
		}
	}
	if (index == XMEM_MAX_OBJECTS) {
		zos_syscall_error(ZOS_ENOSPC);
		return;
	}
	if (process_aux_alloc(pid, slots,
			      mappable != 0u ? ZOS_AUX_PLACE_MAPPABLE
					     : ZOS_AUX_PLACE_DATA,
			      &bank, &slot) != 0) {
		zos_syscall_error(ZOS_ENOSPC);
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
	obj->type = ZOS_XMEM_TYPE_RUNTIME;
	obj->seg_id = 0u;
	obj->seg_kind = 0u;
	obj->requested = size;
	zos_syscall_success((uint16_t)(((uint16_t)index << 4) | obj->gen));
}

/* One loader-created segment (kernel/process/exec.c): allocate its slots
 * and register the object before the child bank is touched, so a failure
 * is a clean rollback through process_slot_release().  A `DATA` segment
 * is non-mappable - it is reached by handle - and the caller receives the
 * placement it copies the payload into and zeroes the BSS at.  A
 * `FAR_CODE` segment is linked at 8000h, so it is placed in slot 4 (slots
 * 4-5 when it is over 8 KiB): one far-code segment per auxiliary bank,
 * and it has no handle (SYS_SEG_MAP returns its aperture token). */
int zos_xmem_segment_alloc(uint8_t pid, uint8_t id, uint8_t kind,
			   uint16_t size, uint16_t *handle, uint8_t *bank,
			   uint8_t *slot)
{
	struct xmem_object *obj;
	uint8_t place;
	uint8_t slots;
	uint8_t index;

	if (handle == (uint16_t *)0 || bank == (uint8_t *)0 ||
	    slot == (uint8_t *)0 || pid >= PROCESS_TABLE_SIZE ||
	    id == 0u || size == 0u) {
		return -ZOS_EINVAL;
	}
	if (kind == ZEX_SEG_KIND_DATA) {
		if (size > ZEX_SEG_MAX_SIZE) {
			return -ZOS_EINVAL;
		}
		place = ZOS_AUX_PLACE_DATA;
	} else if (kind == ZEX_SEG_KIND_FAR_CODE) {
		if (size > ZEX_SEG_MAX_FAR_SIZE) {
			return -ZOS_EINVAL;
		}
		place = ZOS_AUX_PLACE_FAR;
	} else {
		return -ZOS_EINVAL;
	}
	slots = (uint8_t)(((uint32_t)size + (ZOS_XMEM_SLOT_SIZE - 1u)) >>
			  13);
	for (index = 0u; index < XMEM_MAX_OBJECTS; ++index) {
		if (xmem_table[index].used == 0u) {
			break;
		}
	}
	if (index == XMEM_MAX_OBJECTS) {
		return -ZOS_ENOSPC;
	}
	if (process_aux_alloc(pid, slots, place, bank, slot) != 0) {
		return -ZOS_ENOSPC;
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
	obj->flags = ZOS_XMEM_FLAG_NONE;
	obj->type = ZOS_XMEM_TYPE_SEGMENT;
	obj->seg_id = id;
	obj->seg_kind = kind;
	obj->requested = size;
	*handle = (kind == ZEX_SEG_KIND_DATA)
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
		    obj->type == ZOS_XMEM_TYPE_SEGMENT &&
		    obj->seg_id == id) {
			*index = i;
			return obj;
		}
	}
	return (struct xmem_object *)0;
}

void zos_xmem_free(uint16_t block)
{
	struct zos_xmem_free_args *args = (struct zos_xmem_free_args *)block;
	struct xmem_object *obj;

	if (!zos_syscall_block_ok(block, sizeof(*args))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	obj = xmem_lookup(args->handle, process_current_pid());
	if (obj == (struct xmem_object *)0 ||
	    obj->type != ZOS_XMEM_TYPE_RUNTIME) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	process_aux_free(obj->pid, obj->bank, obj->slot, obj->slots);
	obj->used = 0u;
	zos_syscall_success(0);
}

/* SYS_XMEM_READ (op ZOS_BANK_OP_READ) and SYS_XMEM_WRITE (op
 * ZOS_BANK_OP_WRITE) share one bounded, checked transfer. */
static void xmem_transfer(uint16_t block, uint8_t op)
{
	struct zos_xmem_io_args *args = (struct zos_xmem_io_args *)block;
	struct xmem_object *obj;
	uint16_t allocated;
	uint16_t offset;
	uint16_t buffer;
	uint16_t length;
	uint16_t done = 0u;

	if (!zos_syscall_block_ok(block, sizeof(*args))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	obj = xmem_lookup(args->handle, process_current_pid());
	offset = args->offset;
	length = args->length;
	buffer = args->buffer;
	if (obj == (struct xmem_object *)0 ||
	    (length != 0u && !zos_syscall_buffer_ok(buffer, length))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	allocated = (uint16_t)(obj->slots * ZOS_XMEM_SLOT_SIZE);
	if (offset > allocated ||
	    length > (uint16_t)(allocated - offset)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	while (done < length) {
		uint16_t addr = xmem_object_address(obj,
						    (uint16_t)(offset + done));
		uint16_t chunk = xmem_chunk(addr, (uint16_t)(length - done));

		if (op == ZOS_BANK_OP_READ) {
			if (xmem_object_move(obj, addr, chunk,
					     ZOS_BANK_OP_READ) != 0u ||
			    zos_syscall_bank_write(
				    (uint16_t)(buffer + done), chunk) != 0u) {
				zos_syscall_error(ZOS_EFAULT);
				return;
			}
		} else {
			if (zos_syscall_bank_read(
				    (uint16_t)(buffer + done), chunk) != 0u ||
			    xmem_object_move(obj, addr, chunk,
					     ZOS_BANK_OP_WRITE) != 0u) {
				zos_syscall_error(ZOS_EFAULT);
				return;
			}
		}
		done = (uint16_t)(done + chunk);
	}
	zos_syscall_success(length);
}

void zos_xmem_read(uint16_t block)
{
	xmem_transfer(block, ZOS_BANK_OP_READ);
}

void zos_xmem_write(uint16_t block)
{
	xmem_transfer(block, ZOS_BANK_OP_WRITE);
}

/* SYS_XMEM_INFO: the logical description only - never a bank or slot. */
void zos_xmem_info(uint16_t block)
{
	struct zos_xmem_info_args *args = (struct zos_xmem_info_args *)block;
	struct xmem_object *obj;
	uint8_t *record = zos_user_copy_staging;
	uint16_t allocated;

	if (!zos_syscall_block_ok(block, sizeof(*args)) ||
	    !zos_syscall_buffer_ok(args->buffer,
				   (uint16_t)sizeof(struct zos_xmem_info))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	obj = xmem_lookup(args->handle, process_current_pid());
	if (obj == (struct xmem_object *)0) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	allocated = (uint16_t)(obj->slots * ZOS_XMEM_SLOT_SIZE);
	record[0] = (uint8_t)obj->requested;
	record[1] = (uint8_t)(obj->requested >> 8);
	record[2] = (uint8_t)allocated;
	record[3] = (uint8_t)(allocated >> 8);
	record[4] = (uint8_t)obj->flags;
	record[5] = 0u;
	record[6] = obj->type;
	record[7] = 0u;
	if (zos_syscall_bank_write(args->buffer,
				   (uint16_t)sizeof(struct zos_xmem_info)) !=
	    0u) {
		zos_syscall_error(ZOS_EFAULT);
		return;
	}
	zos_syscall_success(0);
}

/* SYS_XMEM_MAP: the token that opens the aperture onto the object plus the
 * object's address in it.  Only the caller's own mappable objects have
 * one; the token keeps the caller's bank field and selects P = 7 - bank,
 * so it can never show bank 6 or bank 7. */
void zos_xmem_map(uint16_t block)
{
	struct zos_xmem_map_args *args = (struct zos_xmem_map_args *)block;
	struct xmem_object *obj;
	uint8_t latch;
	uint8_t token;

	if (!zos_syscall_block_ok(block, sizeof(*args))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	obj = xmem_lookup(args->handle, process_current_pid());
	if (obj == (struct xmem_object *)0 ||
	    (obj->flags & ZOS_XMEM_FLAG_MAPPABLE) == 0u) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	latch = ipc_gate_latch();
	token = (uint8_t)((latch & ZOS_BANK_MASK) | ZOS_MODE_APPLICATION |
			  (uint8_t)((7u - obj->bank) << ZOS_ROM_PAGE_SHIFT));
	args->address = (uint16_t)(ZOS_XMEM_APERTURE_BASE +
				   (uint16_t)(obj->slot - 4u) *
					   ZOS_XMEM_SLOT_SIZE);
	zos_syscall_success(token);
}

/* SYS_SEG_INFO (26h): the running process's own ZEX v2 segments, named by
 * their container ids.  A DATA segment reports its extended-memory handle
 * (the abi/zos/xmem.h calls reach it); a FAR_CODE segment has none (it is
 * executed in place through the aperture, not copied) and reports 0.  A
 * live segment of a loaded container is LOADED by construction - the
 * loader places every segment or the exec fails and rolls back. */
void zos_seg_info(uint16_t block)
{
	struct zos_seg_info_args *args = (struct zos_seg_info_args *)block;
	struct xmem_object *obj;
	uint8_t *record = zos_user_copy_staging;
	uint8_t index = 0u;
	uint16_t handle = 0u;

	if (!zos_syscall_block_ok(block, sizeof(*args)) ||
	    !zos_syscall_buffer_ok(args->buffer,
				   (uint16_t)sizeof(struct zos_seg_info))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	obj = xmem_segment_find(process_current_pid(), args->segment, &index);
	if (obj == (struct xmem_object *)0) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	if (obj->seg_kind == ZEX_SEG_KIND_DATA) {
		handle = (uint16_t)(((uint16_t)index << 4) | obj->gen);
	}
	record[0] = (uint8_t)obj->requested;
	record[1] = (uint8_t)(obj->requested >> 8);
	record[2] = obj->seg_kind;
	record[3] = ZOS_SEG_STATE_LOADED;
	record[4] = (uint8_t)handle;
	record[5] = (uint8_t)(handle >> 8);
	record[6] = 0u;
	record[7] = 0u;
	if (zos_syscall_bank_write(args->buffer,
				   (uint16_t)sizeof(struct zos_seg_info)) !=
	    0u) {
		zos_syscall_error(ZOS_EFAULT);
		return;
	}
	zos_syscall_success(0);
}

/* SYS_SEG_MAP (27h): the token that opens the aperture onto one of the
 * caller's own FAR_CODE segments.  The token keeps the caller's bank field
 * and selects P = 7 - bank, so it can never show bank 6 or bank 7; the
 * segment is always at 8000h (its linked address), so no address is
 * returned.  A DATA segment has no token - it is reached by handle. */
void zos_seg_map(uint16_t block)
{
	struct zos_seg_map_args *args = (struct zos_seg_map_args *)block;
	struct xmem_object *obj;
	uint8_t index = 0u;
	uint8_t latch;
	uint8_t token;

	if (!zos_syscall_block_ok(block, sizeof(*args))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	obj = xmem_segment_find(process_current_pid(), args->segment, &index);
	if (obj == (struct xmem_object *)0 ||
	    obj->seg_kind != ZEX_SEG_KIND_FAR_CODE) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	latch = ipc_gate_latch();
	token = (uint8_t)((latch & ZOS_BANK_MASK) | ZOS_MODE_APPLICATION |
			  (uint8_t)((7u - obj->bank) << ZOS_ROM_PAGE_SHIFT));
	zos_syscall_success(token);
}

void zos_xmem_dispatch(uint8_t number, uint16_t block)
{
	switch (number) {
	case ZOS_SYSCALL_XMEM_ALLOC:
		zos_xmem_alloc(block);
		return;
	case ZOS_SYSCALL_XMEM_FREE:
		zos_xmem_free(block);
		return;
	case ZOS_SYSCALL_XMEM_READ:
		zos_xmem_read(block);
		return;
	case ZOS_SYSCALL_XMEM_WRITE:
		zos_xmem_write(block);
		return;
	case ZOS_SYSCALL_XMEM_INFO:
		zos_xmem_info(block);
		return;
	case ZOS_SYSCALL_XMEM_MAP:
		zos_xmem_map(block);
		return;
	case ZOS_SYSCALL_SEG_INFO:
		zos_seg_info(block);
		return;
	case ZOS_SYSCALL_SEG_MAP:
		zos_seg_map(block);
		return;
	default:
		zos_syscall_error(ZOS_ENOSYS);
		return;
	}
}

void zos_xmem_release_process(uint8_t pid)
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
