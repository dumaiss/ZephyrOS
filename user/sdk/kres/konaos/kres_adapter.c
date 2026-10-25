/* KRES: the KonaOS edition's adapter, read side (libkres member).
 *
 * The kres_io half a sealed-container reader needs: open/close/size/
 * read/seek/sync over the SDK file API, with the 8.3 swap-name mapping
 * compaction's temporary and recovery files need.  The card serves at
 * most two open files system-wide, so this member holds two slots,
 * keyed by the container each was opened for.
 *
 * Programs that mutate install `kres_io_konaos` (the mutation-side
 * member); programs that only read install `kres_io_konaos_read` and
 * link no write, truncate, rename or unlink call.
 */

#include "kos.h"

#include "kos/kres.h"
#include "kos/kres_konaos.h"
#include "kos/errno.h"

#include "kres_internal.h"
#include "kona_internal.h"

struct kona_slot {
	void *ctx;
	int handle;      /* the SDK file handle, >= 1 */
	uint8_t open;    /* 1: this slot holds a live file */
};

static struct kona_slot kona_slots[2];

static int kona_slot_of(void *ctx)
{
	unsigned int i;

	for (i = 0u; i < 2u; ++i) {
		if (kona_slots[i].open != 0u && kona_slots[i].ctx == ctx) {
			return (int)i;
		}
	}
	return -1;
}

int kres_kona_handle(void *ctx)
{
	int slot = kona_slot_of(ctx);

	return slot < 0 ? -KOS_EIO : kona_slots[slot].handle;
}

/* The 8.3 spelling of a compaction swap name: the core appends one
 * suffix character ('~' or '#') to the container's path, which an 8.3
 * filesystem cannot hold; the suffix moves before the extension, the
 * stem truncates to 7 and the extension to 3.  The card's driver
 * rejects '~' (the FatFs character set), so the temporary '~' spells
 * with '$' instead; '#' passes through.  A name whose basename does
 * not end in a suffix passes through unchanged. */
void kres_kona_map_swap_name(char *out, const char *path)
{
	uint16_t length = 0u;
	uint16_t base;
	uint16_t dot;
	uint16_t stem;
	char suffix;
	unsigned int i;

	while (path[length] != '\0') {
		++length;
	}
	base = length;
	while (base > 0u && path[base - 1u] != '/' && path[base - 1u] != '\\') {
		--base;
	}
	if (base >= length || (path[length - 1u] != '~' &&
			       path[length - 1u] != '#')) {
		memcpy(out, path, length + 1u);
		return;
	}
	suffix = path[length - 1u];
	if (suffix == '~') {
		suffix = '$'; /* '~' is not a legal on-card character */
	}
	dot = length;
	for (i = base; i < length - 1u; ++i) {
		if (path[i] == '.') {
			dot = i;
		}
	}
	stem = length - 1u; /* no extension: everything before the suffix */
	if (dot < length) {
		stem = dot;
	}
	/* directory + stem(7) + suffix + .ext(3) */
	{
		unsigned int n = 0u;
		unsigned int count;

		for (i = 0u; i < base; ++i) {
			out[n++] = path[i];
		}
		count = stem - base;
		if (count > 7u) {
			count = 7u;
		}
		for (i = 0u; i < count; ++i) {
			out[n++] = path[base + i];
		}
		out[n++] = suffix;
		if (dot < length) {
			unsigned int ext = length - 1u - dot;

			out[n++] = '.';
			if (ext > 3u) {
				ext = 3u;
			}
			for (i = 0u; i < ext; ++i) {
				out[n++] = path[dot + 1u + i];
			}
		}
		out[n] = '\0';
	}
}

int kres_kona_open(void *ctx, const char *path, unsigned int flags)
{
	char mapped[KOS_PATH_MAX];
	unsigned char kfs = KFS_O_READ;
	int slot = kona_slot_of(ctx);
	int handle;

	if ((flags & KRES_OPEN_WRITE) != 0u) {
		kfs |= KFS_O_WRITE;
	}
	if ((flags & KRES_OPEN_CREATE) != 0u) {
		kfs |= KFS_O_CREATE;
	}
	kres_kona_map_swap_name(mapped, path);
	handle = open(mapped, kfs);
	if (handle < 0) {
		return handle;
	}
	if (slot < 0) {
		unsigned int i;

		for (i = 0u; i < 2u; ++i) {
			if (kona_slots[i].open == 0u) {
				slot = (int)i;
				break;
			}
		}
		if (slot < 0) {
			(void)close((unsigned int)handle);
			return -KOS_EBUSY; /* the card serves two files */
		}
	} else {
		/* A reopen after the create fallback closes the old. */
		(void)close((unsigned int)kona_slots[slot].handle);
	}
	kona_slots[slot].ctx = ctx;
	kona_slots[slot].handle = handle;
	kona_slots[slot].open = 1u;
	return 0;
}

int kres_kona_close(void *ctx)
{
	int slot = kona_slot_of(ctx);

	if (slot < 0) {
		return 0;
	}
	{
		int result = close((unsigned int)kona_slots[slot].handle);

		kona_slots[slot].handle = -1;
		kona_slots[slot].ctx = (void *)0;
		kona_slots[slot].open = 0u;
		return result;
	}
}

int kres_kona_size(void *ctx, uint32_t *out)
{
	int handle = kres_kona_handle(ctx);
	long position;

	if (handle < 0) {
		return handle;
	}
	position = seek((unsigned int)handle, 0L, KFS_SEEK_END);
	if (position < 0L) {
		return (int)position;
	}
	*out = (uint32_t)position;
	return 0;
}

int kres_kona_read(void *ctx, void *dst, unsigned int size)
{
	int handle = kres_kona_handle(ctx);

	if (handle < 0) {
		return handle;
	}
	return read((unsigned int)handle, dst, size);
}

int kres_kona_seek(void *ctx, uint32_t offset)
{
	int handle = kres_kona_handle(ctx);
	long position;

	if (handle < 0) {
		return handle;
	}
	position = seek((unsigned int)handle, (long)offset, KFS_SEEK_SET);
	if (position < 0L) {
		return (int)position;
	}
	return 0;
}

int kres_kona_sync(void *ctx)
{
	int handle = kres_kona_handle(ctx);

	if (handle < 0) {
		return handle;
	}
	return sync((unsigned int)handle);
}

/* The read-only install table: the mutation-side slots are NULL, so
 * this member does not reference the write/truncate/rename/unlink
 * calls at all. */
const struct kres_io kres_io_konaos_read = {
	kres_kona_open,
	kres_kona_close,
	kres_kona_size,
	kres_kona_read,
	(int (*)(void *, const void *, unsigned int))0,
	kres_kona_seek,
	kres_kona_sync,
	(int (*)(void *, uint32_t))0,
	(int (*)(void *, const char *, const char *))0,
	(int (*)(void *, const char *))0,
};
