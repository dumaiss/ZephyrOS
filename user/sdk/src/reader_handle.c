/* kos_reader_from_handle - the OS file handle adapter (libkos, own
 * member).  Borrows the handle: every read is an absolute `seek` then
 * a `read`, no buffering, no second open, no close.  The size is the
 * file's length, captured with a seek to the end at construction. */

#include "kos.h"
#include "kos/reader.h"
#include "kos/protocol.h"

static int handle_read_at(void *context, uint32_t offset, void *dst,
			  uint16_t requested)
{
	kos_handle_source_t *source = (kos_handle_source_t *)context;
	long position;

	if (source == (kos_handle_source_t *)0) {
		return -KOS_EINVAL;
	}
	position = seek(source->handle, (long)offset, KFS_SEEK_SET);
	if (position < 0) {
		return (int)position;
	}
	return read(source->handle, dst, (unsigned int)requested);
}

int kos_reader_from_handle(kos_reader_t *out, kos_handle_source_t *source)
{
	long size;

	if (out == (kos_reader_t *)0 || source == (kos_handle_source_t *)0) {
		return -KOS_EINVAL;
	}
	size = seek(source->handle, 0, KFS_SEEK_END);
	if (size < 0) {
		return (int)size;
	}
	out->context = source;
	out->read_at = handle_read_at;
	out->size = (uint32_t)size;
	return 0;
}
