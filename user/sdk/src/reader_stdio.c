/* kos_reader_from_stdio - the `FILE *` adapter (libkos, own member).
 *
 * Borrows the buffered stream: every read is an absolute `fseek` then
 * an `fread`, using the stream's own buffer (no extra cache), no
 * second open, no close.  The size is captured with `fseek`/`ftell`
 * at construction. */

#include "kos.h"
#include "kos/reader.h"

static int stdio_read_at(void *context, uint32_t offset, void *dst,
			 uint16_t requested)
{
	FILE *stream = (FILE *)context;
	int result;

	if (stream == (FILE *)0) {
		return -KOS_EINVAL;
	}
	/* A sequential read is served from the stream's own buffer: seek
	 * only when the stream is somewhere else (ftell is the stream's
	 * logical position, so the caller's own use of the stream is
	 * seen too). */
	if (ftell(stream) != (long)offset) {
		result = fseek(stream, (long)offset, SEEK_SET);
		if (result < 0) {
			return result;
		}
	}
	return (int)fread(dst, 1u, (size_t)requested, stream);
}

int kos_reader_from_stdio(kos_reader_t *out, FILE *stream)
{
	long size;
	int result;

	if (out == (kos_reader_t *)0 || stream == (FILE *)0) {
		return -KOS_EINVAL;
	}
	result = fseek(stream, 0, SEEK_END);
	if (result != 0) {
		return result;
	}
	size = ftell(stream);
	if (size < 0) {
		return (int)size;
	}
	out->context = stream;
	out->read_at = stdio_read_at;
	out->size = (uint32_t)size;
	return 0;
}
