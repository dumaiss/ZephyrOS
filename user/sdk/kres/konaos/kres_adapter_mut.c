/* KRES: the KonaOS edition's adapter, mutation side (libkres member).
 *
 * The write, truncate, rename and unlink halves of the kres_io table,
 * over the SDK file API, and the full install table.  A program that
 * only reads installs `kres_io_konaos_read` (the read-side member)
 * and never links this file.
 */

#include "kos.h"

#include "kos/kres.h"
#include "kos/kres_konaos.h"
#include "kos/errno.h"

#include "kona_internal.h"

static int kona_write(void *ctx, const void *src, unsigned int size)
{
	int handle = kres_kona_handle(ctx);

	if (handle < 0) {
		return handle;
	}
	return write((unsigned int)handle, src, size);
}

static int kona_truncate(void *ctx, uint32_t size)
{
	int handle = kres_kona_handle(ctx);

	if (handle < 0) {
		return handle;
	}
	return truncate((unsigned int)handle, (unsigned long)size);
}

static int kona_rename(void *ctx, const char *from, const char *to)
{
	char mapped_from[KOS_PATH_MAX];
	char mapped_to[KOS_PATH_MAX];

	(void)ctx;
	kres_kona_map_swap_name(mapped_from, from);
	kres_kona_map_swap_name(mapped_to, to);
	return rename(mapped_from, mapped_to);
}

static int kona_unlink(void *ctx, const char *path)
{
	char mapped[KOS_PATH_MAX];

	(void)ctx;
	kres_kona_map_swap_name(mapped, path);
	return unlink(mapped);
}

/* The full table references the read-side members' functions, so
 * installing it links both adapter members. */
const struct kres_io kres_io_konaos = {
	kres_kona_open,
	kres_kona_close,
	kres_kona_size,
	kres_kona_read,
	kona_write,
	kres_kona_seek,
	kres_kona_sync,
	kona_truncate,
	kona_rename,
	kona_unlink,
};
