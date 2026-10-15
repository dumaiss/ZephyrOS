/* KonaOS klibc: the stream positioning group.
 *
 * `fseek` flushes pending writes (so the SDK handle's position is the
 * logical one), discards the read buffer, calls the SDK `seek` and
 * clears EOF; `ftell` is the logical position the stream keeps, so it
 * accounts for the bytes buffered but not yet consumed or written.
 * `fflush(NULL)` flushes every open stream and the console.
 */

#include "kos.h"

#include "stdio_internal.h"

int fseek(FILE *stream, long offset, int whence)
{
	struct kos_stream *s = KOS_STREAM_SLOT(stream);
	long result;

	if (s == (struct kos_stream *)0) {
		return -KOS_EINVAL;
	}
	if (whence != SEEK_SET && whence != SEEK_CUR &&
	    whence != SEEK_END) {
		return -KOS_EINVAL;
	}
	result = kos_stream_push(s);
	if (result < 0) {
		return (int)result;
	}
	s->count = 0u;
	s->next = 0u;
	s->state &= (unsigned char)~KOS_STREAM_EOF;
	if (whence != SEEK_END) {
		/* The buffer is gone and the pending writes are out, so
		 * the logical position and the handle's agree: one
		 * absolute seek reaches any origin. */
		long target = whence == SEEK_CUR ? s->pos + offset :
						      offset;

		result = seek((unsigned int)s->handle, target, KFS_SEEK_SET);
	} else {
		/* Only the handle knows the file's length. */
		result = seek((unsigned int)s->handle, offset, KFS_SEEK_END);
	}
	if (result < 0) {
		s->state |= KOS_STREAM_ERR;
		return (int)result;
	}
	s->pos = result;
	return 0;
}

long ftell(FILE *stream)
{
	struct kos_stream *s = KOS_STREAM_SLOT(stream);

	return s == (struct kos_stream *)0 ? -1L : s->pos;
}

void rewind(FILE *stream)
{
	struct kos_stream *s = KOS_STREAM_SLOT(stream);

	if (s == (struct kos_stream *)0) {
		return;
	}
	(void)fseek(stream, 0L, SEEK_SET);
	s->state &= (unsigned char)~KOS_STREAM_ERR;
}

int fflush(FILE *stream)
{
	int result = 0;

	if (stream != (FILE *)0) {
		struct kos_stream *s = KOS_STREAM_SLOT(stream);

		return s == (struct kos_stream *)0 ? -KOS_EINVAL :
						     kos_stream_push(s);
	}
	{
		unsigned int i;

		for (i = 0u; i < KOS_STREAM_MAX; ++i) {
			if (kos_streams[i].handle != 0) {
				int pushed = kos_stream_push(&kos_streams[i]);

				if (pushed < 0 && result == 0) {
					result = pushed;
				}
			}
		}
	}
	{
		int flushed = console_flush();

		if (flushed < 0 && result == 0) {
			result = flushed;
		}
	}
	return result;
}
