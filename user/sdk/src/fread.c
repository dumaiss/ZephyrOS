/* KonaOS klibc: the stream reading group.
 *
 * `fread`, `fgetc`/`getc` and `fgets` all come through the stream's
 * 512-byte buffer (kos_stream_refill): a byte loop costs one buffer copy
 * per byte and one file read per 512, not one IPC call per byte.  A read
 * of a buffer or more goes straight into the caller's memory - one copy,
 * not two - after the buffered bytes are consumed; the direct reads are
 * chunked to the one descriptor a call carries (KOS_STREAM_IO_MAX).
 */

#include "kos.h"

#include "stdio_internal.h"

/* Read `size` bytes into `dst`; returns the bytes read, which fall short
 * of `size` at end of file or after an error (that sets the stream's
 * flag).  The buffered bytes go first, in runs; a request of a buffer or
 * more then reads straight into `dst`, and a smaller tail goes through
 * the buffer. */
static unsigned int fread_span(struct kos_stream *s, unsigned char *dst,
			       unsigned int size)
{
	unsigned int got = 0u;

	while (got < size) {
		unsigned int remaining = size - got;

		if (s->next < s->count) {
			unsigned int available = s->count - s->next;
			unsigned int take = remaining < available ?
					    remaining : available;

			memcpy(dst + got, s->buf + s->next, take);
			s->next += take;
			s->pos += (long)take;
			got += take;
			continue;
		}
		if (remaining >= KOS_STREAM_BUF_SIZE) {
			unsigned int chunk = remaining > KOS_STREAM_IO_MAX ?
					     KOS_STREAM_IO_MAX : remaining;
			int result = read((unsigned int)s->handle, dst + got,
					  chunk);

			if (result < 0) {
				s->state |= KOS_STREAM_ERR;
				break;
			}
			if (result == 0) {
				s->state |= KOS_STREAM_EOF;
				break;
			}
			got += (unsigned int)result;
			s->pos += (long)result;
			continue;
		}
		if (kos_stream_refill(s) <= 0) {
			break;
		}
	}
	return got;
}

/* `ptr` must name `size * nmemb` bytes inside the 64 KiB address space
 * (the product is not checked against a wrap of it).  The items go in
 * groups of whole items of at most 65535 bytes, so no 16-bit total
 * wraps; the count is the whole items read, and the bytes of a partial
 * final item are consumed from the stream. */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	struct kos_stream *s = KOS_STREAM_SLOT(stream);
	unsigned char *at = (unsigned char *)ptr;
	unsigned int limit, group, bytes, got, items = 0u;

	if (size == 0u || nmemb == 0u) {
		return 0u;
	}
	if (s == (struct kos_stream *)0 || (s->mode & KOS_STREAM_READ) == 0u ||
	    ptr == (void *)0) {
		if (s != (struct kos_stream *)0) {
			s->state |= KOS_STREAM_ERR;
		}
		return 0u;
	}
	if (size == 1u) {
		return (size_t)fread_span(s, at, nmemb);
	}
	if (nmemb == 1u) {
		return fread_span(s, at, size) == size ? 1u : 0u;
	}
	if ((size | nmemb) <= 0xFFu) {
		bytes = nmemb * size;
		got = fread_span(s, at, bytes);
		return (size_t)(got == bytes ? nmemb : got / size);
	}
	limit = 0xFFFFu / size;
	while (nmemb != 0u) {
		group = nmemb < limit ? nmemb : limit;
		bytes = group * size;
		got = fread_span(s, at, bytes);
		if (got != bytes) {
			items += got / size;
			break;
		}
		items += group;
		at += bytes;
		nmemb -= group;
	}
	return (size_t)items;
}

int fgetc(FILE *stream)
{
	struct kos_stream *s = KOS_STREAM_SLOT(stream);

	if (s == (struct kos_stream *)0 || (s->mode & KOS_STREAM_READ) == 0u) {
		if (s != (struct kos_stream *)0) {
			s->state |= KOS_STREAM_ERR;
		}
		return EOF;
	}
	if (s->next >= s->count && kos_stream_refill(s) <= 0) {
		return EOF;
	}
	++s->pos;
	return (int)(unsigned char)s->buf[s->next++];
}

int getc(FILE *stream)
{
	return fgetc(stream);
}

char *fgets(char *dst, int size, FILE *stream)
{
	int i = 0;

	if (dst == (char *)0 || size <= 0) {
		return (char *)0;
	}
	while (i + 1 < size) {
		int ch = fgetc(stream);

		if (ch < 0) {
			break;
		}
		dst[i] = (char)ch;
		++i;
		if ((char)ch == '\n') {
			break;
		}
	}
	if (i == 0) {
		return (char *)0;
	}
	dst[i] = '\0';
	return dst;
}
