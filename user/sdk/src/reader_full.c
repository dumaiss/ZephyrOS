/* kos_reader_read_full - complete-range reads (libkos, own member).
 *
 * Retries short reads until the range is complete: returns the length,
 * a negative error from the adapter, or -KOS_EIO at an early end.
 * The offset plus the length must not wrap a 32-bit offset. */

#include "kos.h"
#include "kos/reader.h"

int kos_reader_read_full(const kos_reader_t *reader, uint32_t offset,
			 void *dst, uint16_t length)
{
	uint16_t done = 0u;

	if (reader == (const kos_reader_t *)0 || dst == (void *)0) {
		return -KOS_EINVAL;
	}
	if ((uint32_t)length > reader->size ||
	    offset > reader->size - (uint32_t)length) {
		return -KOS_EIO; /* the range runs past the end */
	}
	while (done < length) {
		int result = reader->read_at(
			reader->context, (uint32_t)(offset + done),
			(unsigned char *)dst + done, (uint16_t)(length - done));

		if (result < 0) {
			return result;
		}
		if (result == 0) {
			return -KOS_EIO; /* an early end */
		}
		done = (uint16_t)(done + (uint16_t)result);
	}
	return (int)length;
}
