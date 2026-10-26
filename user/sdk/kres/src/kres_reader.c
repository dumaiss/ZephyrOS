/* kos_reader_from_kres - the KRES resource adapter (libkres member).
 *
 * Borrows the logical resource: every read is an absolute `kres_seek`
 * then a `kres_read` through the container's one underlying handle (no
 * second open), no extra cache, no `kres_rclose`.  The size is the
 * resource's used byte length. */

#if defined(__SDCC)
#include "kos.h"
#else
/* The host kres build must not pull the SDK kos.h: its klibc
 * declarations collide with the host's own stdio builtins. */
#include "kos/errno.h"
#endif
#include "kos/reader.h"
#include "kos/kres.h"

static int kres_read_at(void *context, uint32_t offset, void *dst,
			uint16_t requested)
{
	kres_resource *resource = (kres_resource *)context;
	int result;

	if (resource == (kres_resource *)0) {
		return -KOS_EINVAL;
	}
	if (requested == 0u) {
		return 0;
	}
	result = kres_seek(resource, offset);
	if (result < 0) {
		return result;
	}
	return kres_read(resource, dst, requested);
}

int kos_reader_from_kres(kos_reader_t *out, kres_resource *resource)
{
	if (out == (kos_reader_t *)0 || resource == (kres_resource *)0) {
		return -KOS_EINVAL;
	}
	out->context = resource;
	out->read_at = kres_read_at;
	out->size = resource->used_bytes;
	return 0;
}
