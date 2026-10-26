/* kos_reader_from_memory - the memory adapter (libkos, own member).
 *
 * Borrows a caller-owned byte array: every read is a bounds check and
 * a memcpy, no buffering, no allocation. */

#include "kos.h"
#include "kos/reader.h"

static int memory_read_at(void *context, uint32_t offset, void *dst,
			  uint16_t requested)
{
	kos_memory_source_t *source = (kos_memory_source_t *)context;

	if (source == (kos_memory_source_t *)0 || source->data == (void *)0) {
		return -KOS_EINVAL;
	}
	if (offset >= source->size) {
		return 0; /* the end */
	}
	if ((uint32_t)requested > source->size - offset) {
		requested = (uint16_t)(source->size - offset);
	}
	memcpy(dst, (const unsigned char *)source->data + offset, requested);
	return (int)requested;
}

int kos_reader_from_memory(kos_reader_t *out, kos_memory_source_t *source)
{
	if (out == (kos_reader_t *)0 || source == (kos_memory_source_t *)0 ||
	    (source->data == (void *)0 && source->size != 0u)) {
		return -KOS_EINVAL;
	}
	out->context = source;
	out->read_at = memory_read_at;
	out->size = source->size;
	return 0;
}
