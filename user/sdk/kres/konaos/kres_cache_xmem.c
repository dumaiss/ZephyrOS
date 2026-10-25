/* KRES: the KonaOS edition's catalog cache in XMEM (libkres member).
 *
 * One 2 KiB XMEM object per open container (design §5.5), allocated
 * at open and freed at close, including every error path after the
 * allocation.  Two containers are live during compaction, so the
 * backend keeps two slots keyed by the ctx each was loaded for, with
 * least-recently used replacement beyond two.
 *
 * The 512-byte shared buffer stages every object move, so this member
 * needs no primary-space tables of its own.
 */

#include "kos.h"

#include "kos/kres.h"
#include "kos/kres_konaos.h"
#include "kos/errno.h"

#include "kres_internal.h"

#define XMEM_SLOTS 2u
#define XMEM_BYTES (KRES_CATALOG_RECORDS * KRES_RECORD_SIZE)

struct xmem_slot {
	void *ctx;
	int handle;   /* the XMEM object, >= 1 */
	uint8_t open; /* 1: this slot holds a live object */
};

static struct xmem_slot xmem_slots[XMEM_SLOTS];
static unsigned int xmem_stamps[XMEM_SLOTS];
static unsigned int xmem_tick;

static int xmem_slot_of(void *ctx)
{
	unsigned int i;

	for (i = 0u; i < XMEM_SLOTS; ++i) {
		if (xmem_slots[i].open != 0u && xmem_slots[i].ctx == ctx) {
			return (int)i;
		}
	}
	return -1;
}

/* A free slot, else the least-recently used one, claimed for `ctx`;
 * returns the slot or a negated error when XMEM is exhausted. */
static int xmem_claim(void *ctx)
{
	int victim = -1;
	int i;

	for (i = 0; i < (int)XMEM_SLOTS; ++i) {
		if (xmem_slots[i].open == 0u) {
			victim = i;
			break;
		}
	}
	if (victim < 0) {
		if (xmem_stamps[1] < xmem_stamps[0]) {
			victim = 1;
		} else {
			victim = 0;
		}
	}
	if (xmem_slots[victim].open != 0u) {
		(void)xfree((unsigned int)xmem_slots[victim].handle);
	}
	{
		int handle = xalloc(XMEM_BYTES, KOS_XMEM_FLAG_NONE);

		if (handle < 0) {
			xmem_slots[victim].handle = -1;
			xmem_slots[victim].ctx = (void *)0;
			xmem_slots[victim].open = 0u;
			return handle;
		}
		xmem_slots[victim].handle = handle;
	}
	xmem_slots[victim].ctx = ctx;
	xmem_slots[victim].open = 1u;
	xmem_stamps[victim] = ++xmem_tick;
	return victim;
}

static void xmem_touch(int slot)
{
	xmem_stamps[slot] = ++xmem_tick;
}

/* Copy the four catalog records between the file and the object. */
static int xmem_move(void *ctx, int handle, uint32_t area_record,
		     int writing)
{
	unsigned int i;

	for (i = 0u; i < KRES_CATALOG_RECORDS; ++i) {
		int result = kres_io->seek(ctx,
				kres_record_offset(area_record + i));

		if (result < 0) {
			return result;
		}
		if (writing != 0) {
			result = xread((unsigned int)handle,
				       i * KRES_RECORD_SIZE, kres_shared,
				       KRES_RECORD_SIZE);
			if (result != (int)KRES_RECORD_SIZE) {
				return (result < 0) ? result : -KOS_EIO;
			}
			result = kres_io->write(ctx, kres_shared,
						KRES_RECORD_SIZE);
		} else {
			result = kres_io->read(ctx, kres_shared,
					       KRES_RECORD_SIZE);
			if (result != (int)KRES_RECORD_SIZE) {
				return (result < 0) ? result : -KOS_EIO;
			}
			result = xwrite((unsigned int)handle,
					i * KRES_RECORD_SIZE, kres_shared,
					KRES_RECORD_SIZE);
		}
		if (result != (int)KRES_RECORD_SIZE) {
			return (result < 0) ? result : -KOS_EIO;
		}
	}
	return 0;
}

static int xmem_load(void *ctx, uint32_t area_record)
{
	int slot = xmem_slot_of(ctx);
	int result;

	if (slot < 0) {
		slot = xmem_claim(ctx);
		if (slot < 0) {
			return slot;
		}
	} else {
		xmem_touch(slot);
	}
	result = xmem_move(ctx, xmem_slots[slot].handle, area_record, 0);
	if (result < 0) {
		/* The error path after the allocation frees it. */
		(void)xfree((unsigned int)xmem_slots[slot].handle);
		xmem_slots[slot].handle = -1;
		xmem_slots[slot].ctx = (void *)0;
		xmem_slots[slot].open = 0u;
	}
	return result;
}

static int xmem_entry(void *ctx, unsigned int index, uint8_t *out)
{
	int slot = xmem_slot_of(ctx);
	int result;

	if (slot < 0 || index >= KRES_RESOURCE_MAX) {
		return -KOS_EIO;
	}
	xmem_touch(slot);
	result = xread((unsigned int)xmem_slots[slot].handle,
		       index * KRES_ENTRY_SIZE, out, KRES_ENTRY_SIZE);
	return result == (int)KRES_ENTRY_SIZE ? 0
	       : (result < 0 ? result : -KOS_EIO);
}

static int xmem_put(void *ctx, unsigned int index, const uint8_t *entry)
{
	int slot = xmem_slot_of(ctx);
	int result;

	if (index >= KRES_RESOURCE_MAX) {
		return -KOS_EIO;
	}
	if (slot < 0) {
		/* A put without a load starts an empty area (the sealed
		 * builder accumulates its catalog this way). */
		unsigned int i;

		slot = xmem_claim(ctx);
		if (slot < 0) {
			return slot;
		}
		memset(kres_shared, 0, KRES_RECORD_SIZE);
		for (i = 0u; i < KRES_CATALOG_RECORDS; ++i) {
			result = xwrite((unsigned int)xmem_slots[slot].handle,
					i * KRES_RECORD_SIZE, kres_shared,
					KRES_RECORD_SIZE);
			if (result != (int)KRES_RECORD_SIZE) {
				(void)xfree((unsigned int)
					    xmem_slots[slot].handle);
				xmem_slots[slot].handle = -1;
				xmem_slots[slot].ctx = (void *)0;
				xmem_slots[slot].open = 0u;
				return (result < 0) ? result : -KOS_EIO;
			}
		}
	} else {
		xmem_touch(slot);
	}
	result = xwrite((unsigned int)xmem_slots[slot].handle,
			index * KRES_ENTRY_SIZE, entry, KRES_ENTRY_SIZE);
	return result == (int)KRES_ENTRY_SIZE ? 0
	       : (result < 0 ? result : -KOS_EIO);
}

static int xmem_store(void *ctx, uint32_t area_record)
{
	int slot = xmem_slot_of(ctx);
	int result;

	if (slot < 0) {
		return -KOS_EIO;
	}
	xmem_touch(slot);
	result = xmem_move(ctx, xmem_slots[slot].handle, area_record, 1);
	if (result < 0) {
		(void)xfree((unsigned int)xmem_slots[slot].handle);
		xmem_slots[slot].handle = -1;
		xmem_slots[slot].ctx = (void *)0;
		xmem_slots[slot].open = 0u;
	}
	return result;
}

static int xmem_release(void *ctx)
{
	int slot = xmem_slot_of(ctx);

	if (slot >= 0) {
		int result = xfree((unsigned int)xmem_slots[slot].handle);

		xmem_slots[slot].handle = -1;
		xmem_slots[slot].ctx = (void *)0;
		xmem_slots[slot].open = 0u;
		return result;
	}
	return 0;
}

const struct kres_cache kres_cache_xmem = {
	xmem_load,
	xmem_entry,
	xmem_put,
	xmem_store,
	xmem_release,
};
