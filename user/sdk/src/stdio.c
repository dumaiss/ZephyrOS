/* KonaOS klibc: the stream pool, `fopen`/`fclose` and the core calls.
 *
 * The pool is four streams with their 512-byte buffers, so a program's
 * stream memory is fixed and small: `fopen` returns NULL when all four
 * are open, which is the pool's exhaustion.  The three calls every other
 * member builds on live here:
 *
 *   kos_stream_refill  pull the next chunk into the read buffer (and,
 *                      when `fmeasure` asked for it, record the refill's
 *                      clock cost);
 *   kos_stream_push    write the buffered bytes out (a partial write
 *                      resumes from where the driver stopped);
 *   kos_stdio_flush_all  every open stream, for the exit hook.
 *
 * `fopen` is also what registers the exit hook: `exit` flushes open
 * streams through `kos_stream_flush`, and a program that never calls
 * `fopen` never links this member at all (libkos is an archive).
 */

#include "kos.h"
#include "kos/timer.h"

#include "kos_internal.h"
#include "stdio_internal.h"

struct kos_stream kos_streams[KOS_STREAM_MAX];

/* The mode's open flags and access direction; 0 for an unparsable
 * mode. */
static unsigned int stdio_mode(const char *mode, unsigned char *direction)
{
	unsigned int i = 1u;
	unsigned int flags;

	switch (mode[0]) {
	case 'r':
		flags = KFS_O_READ;
		*direction = KOS_STREAM_READ;
		break;
	case 'w':
		flags = KFS_O_WRITE | KFS_O_CREATE | KFS_O_TRUNC;
		*direction = KOS_STREAM_WRITE;
		break;
	case 'a':
		flags = KFS_O_WRITE | KFS_O_CREATE;
		*direction = KOS_STREAM_WRITE;
		break;
	default:
		return 0u;
	}
	/* A 'b' is accepted and ignored: the streams are byte streams. */
	while (mode[i] == 'b') {
		++i;
	}
	return mode[i] == '\0' ? flags : 0u;
}

int kos_stream_refill(struct kos_stream *s)
{
	uint32_t start = 0u;
	uint32_t end = 0u;
	int result;

	if (s->timer != 0) {
		if (timer_now(s->timer, &start) < 0) {
			start = 0u;
		}
	}
	result = read((unsigned int)s->handle, s->buf, KOS_STREAM_BUF_SIZE);
	if (s->timer != 0 && timer_now(s->timer, &end) == 0) {
		uint32_t counts = end - start;

		if (s->refill_count == 0u || counts < s->refill_min) {
			s->refill_min = counts;
		}
		if (counts > s->refill_max) {
			s->refill_max = counts;
		}
		s->refill_sum += counts;
		++s->refill_count;
	}
	if (result < 0) {
		s->state |= KOS_STREAM_ERR;
		return result;
	}
	if (result == 0) {
		s->state |= KOS_STREAM_EOF;
		s->count = 0u;
		s->next = 0u;
		return 0;
	}
	s->count = (unsigned int)result;
	s->next = 0u;
	return result;
}

int kos_stream_push(struct kos_stream *s)
{
	unsigned int done = 0u;

	if ((s->state & KOS_STREAM_DIRTY) == 0u) {
		/* Nothing was written: the bytes a read stream holds in
		 * its buffer are not ours to send. */
		return 0;
	}
	while (done < s->count) {
		int result = write((unsigned int)s->handle, s->buf + done,
				   s->count - done);

		if (result <= 0) {
			s->state |= KOS_STREAM_ERR;
			return result < 0 ? result : -KOS_EIO;
		}
		done += (unsigned int)result;
	}
	s->count = 0u;
	s->state &= (unsigned char)~KOS_STREAM_DIRTY;
	return 0;
}

FILE *fopen(const char *path, const char *mode)
{
	unsigned char direction = 0u;
	unsigned int flags;
	unsigned int i;
	struct kos_stream *s = (struct kos_stream *)0;

	if (path == (const char *)0 || mode == (const char *)0 ||
	    mode[0] == '\0') {
		return (FILE *)0;
	}
	flags = stdio_mode(mode, &direction);
	if (flags == 0u) {
		return (FILE *)0;
	}
	for (i = 0u; i < KOS_STREAM_MAX; ++i) {
		if (kos_streams[i].handle == 0) {
			s = &kos_streams[i];
			break;
		}
	}
	if (s == (struct kos_stream *)0) {
		return (FILE *)0;   /* the pool is exhausted */
	}

	s->handle = open(path, (unsigned char)flags);
	if (s->handle < 0) {
		s->handle = 0;
		return (FILE *)0;
	}
	s->mode = direction;
	s->state = 0u;
	s->count = 0u;
	s->next = 0u;
	s->pos = 0L;
	s->timer = 0;
	s->refill_count = 0u;
	s->refill_min = 0u;
	s->refill_max = 0u;
	s->refill_sum = 0u;
	if (mode[0] == 'a') {
		/* The write position is the end of the file. */
		long end = seek((unsigned int)s->handle, 0L, KFS_SEEK_END);

		if (end < 0) {
			(void)close((unsigned int)s->handle);
			s->handle = 0;
			return (FILE *)0;
		}
		s->pos = end;
	}
	kos_stream_flush = kos_stdio_flush_all;
	return (FILE *)s;
}

int fclose(FILE *stream)
{
	struct kos_stream *s = KOS_STREAM_SLOT(stream);
	int result;

	if (s == (struct kos_stream *)0) {
		return -KOS_EINVAL;
	}
	result = kos_stream_push(s);
	{
		int closed = close((unsigned int)s->handle);

		if (result >= 0) {
			result = closed;
		}
	}
	s->handle = 0;
	s->mode = 0u;
	s->state = 0u;
	s->count = 0u;
	s->next = 0u;
	s->pos = 0L;
	s->timer = 0;
	s->refill_count = 0u;
	return result;
}

void kos_stdio_flush_all(void)
{
	unsigned int i;

	for (i = 0u; i < KOS_STREAM_MAX; ++i) {
		if (kos_streams[i].handle != 0) {
			(void)kos_stream_push(&kos_streams[i]);
		}
	}
}
