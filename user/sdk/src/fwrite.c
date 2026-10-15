/* KonaOS klibc: the stream writing group.
 *
 * `fwrite`, `fputc`/`putc` and `fputs` collect into the stream's
 * 512-byte buffer (kos_stream_push writes it out): a byte loop costs one
 * buffer store per byte and one file write per 512.  A write of a buffer
 * or more flushes the pending bytes and then goes out directly, chunked
 * to the one descriptor a call carries (KOS_STREAM_IO_MAX).
 *
 * A call reports the bytes it accepted; a write error is sticky
 * (ferror), and the flush that meets it returns the error.
 */

#include "kos.h"

#include "stdio_internal.h"

/* Write one item of `size` bytes from `src`; returns the bytes written,
 * which fall short of `size` after an error (that sets the stream's
 * flag).  One item at a time keeps the loop free of a multiply
 * (docs/sdk.md section 5) and lets fwrite count whole items only. */
static int fwrite_item(struct kos_stream *s, const unsigned char *src,
		       unsigned int size)
{
	unsigned int done = 0u;

	while (done < size) {
		unsigned int remaining = size - done;
		unsigned int room, take;

		if (remaining >= KOS_STREAM_BUF_SIZE) {
			unsigned int chunk = remaining > KOS_STREAM_IO_MAX ?
					     KOS_STREAM_IO_MAX : remaining;
			int result;

			if (kos_stream_push(s) < 0) {
				break;
			}
			result = write((unsigned int)s->handle, src + done,
				       chunk);
			if (result <= 0) {
				s->state |= KOS_STREAM_ERR;
				break;
			}
			done += (unsigned int)result;
			s->pos += (long)result;
			continue;
		}
		room = KOS_STREAM_BUF_SIZE - s->count;
		take = remaining < room ? remaining : room;
		memcpy(s->buf + s->count, src + done, take);
		s->count += take;
		done += take;
		s->pos += (long)take;
		s->state |= KOS_STREAM_DIRTY;
		if (s->count == KOS_STREAM_BUF_SIZE &&
		    kos_stream_push(s) < 0) {
			break;
		}
	}
	return (int)done;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	struct kos_stream *s = KOS_STREAM_SLOT(stream);
	const unsigned char *at = (const unsigned char *)ptr;
	unsigned int items;

	if (size == 0u || nmemb == 0u) {
		return 0u;
	}
	if (s == (struct kos_stream *)0 ||
	    (s->mode & KOS_STREAM_WRITE) == 0u || ptr == (const void *)0) {
		if (s != (struct kos_stream *)0) {
			s->state |= KOS_STREAM_ERR;
		}
		return 0u;
	}
	for (items = 0u; items < nmemb; ++items) {
		if (fwrite_item(s, at, size) != (int)size) {
			break;
		}
		at += size;
	}
	return (size_t)items;
}

int fputc(int c, FILE *stream)
{
	struct kos_stream *s = KOS_STREAM_SLOT(stream);

	if (s == (struct kos_stream *)0 ||
	    (s->mode & KOS_STREAM_WRITE) == 0u) {
		if (s != (struct kos_stream *)0) {
			s->state |= KOS_STREAM_ERR;
		}
		return EOF;
	}
	if (s->count == (unsigned int)KOS_STREAM_BUF_SIZE &&
	    kos_stream_push(s) < 0) {
		return EOF;
	}
	s->buf[s->count] = (char)(unsigned char)c;
	++s->count;
	s->state |= KOS_STREAM_DIRTY;
	++s->pos;
	return (int)(unsigned char)c;
}

int putc(int c, FILE *stream)
{
	return fputc(c, stream);
}

int fputs(const char *src, FILE *stream)
{
	unsigned int length = strlen(src);

	return fwrite(src, 1u, length, stream) == length ? 0 : EOF;
}
