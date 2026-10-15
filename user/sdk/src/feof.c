/* KonaOS klibc: the stream state group.
 *
 * `feof` and `ferror` read the stream's sticky flags: EOF is set by a
 * read that reached the end (and cleared by `fseek`/`rewind`), the error
 * flag by any failed SDK call (cleared by `rewind`).  Both are pure
 * reads of the stream's own state, so a program that links only these
 * pulls none of the stream core.
 */

#include "kos.h"

#include "stdio_internal.h"

int feof(FILE *stream)
{
	struct kos_stream *s = KOS_STREAM_SLOT(stream);

	return s != (struct kos_stream *)0 &&
	       (s->state & KOS_STREAM_EOF) != 0u;
}

int ferror(FILE *stream)
{
	struct kos_stream *s = KOS_STREAM_SLOT(stream);

	return s != (struct kos_stream *)0 &&
	       (s->state & KOS_STREAM_ERR) != 0u;
}
