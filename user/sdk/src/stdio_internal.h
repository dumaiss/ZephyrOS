/* KonaOS libkos internal: the stream layout and the core stream calls.
 *
 * `klibc/stdio.h` declares the FILE type and the ISO entry points; the
 * pool, the refill and the flush live in `klibc/stdio.c` and the entry
 * points divide into one archive member per function group (fread.c:
 * reading, fwrite.c: writing, fseek.c: positioning, feof.c: state).
 * This header is private to user/sdk/src and is never installed.
 */

#ifndef KOS_SRC_STDIO_INTERNAL_H
#define KOS_SRC_STDIO_INTERNAL_H

#include "klibc/stdio.h"

/* Stream modes and state bits (`struct kos_stream.mode` is read or
 * write, never both; the state bits are the sticky flags). */
#define KOS_STREAM_READ  0x01u
#define KOS_STREAM_WRITE 0x02u
#define KOS_STREAM_DIRTY 0x04u
#define KOS_STREAM_EOF   0x08u
#define KOS_STREAM_ERR   0x10u

/* The one descriptor a FILE call carries is at most this many bytes
 * (kos/ipc.h), so a direct read or write is chunked to it. */
#define KOS_STREAM_IO_MAX 4096u

/* One stream.  `handle` is 0 for a free slot (a bound handle is never
 * 0); `count`/`next` are the valid bytes and the next byte's index for a
 * reader and the buffered bytes for a writer; `pos` is the logical file
 * position, so a read's position is ahead of the SDK handle's by the
 * bytes sitting in the buffer, and a writer's behind it by the same.
 * The timing fields are `kos/stream.h`'s: a stream without `fmeasure`
 * never reads the clock. */
struct kos_stream {
	int handle;
	unsigned char mode;
	unsigned char state;
	unsigned int count;
	unsigned int next;
	long pos;
	int timer;
	uint32_t refill_min;
	uint32_t refill_max;
	uint32_t refill_sum;
	unsigned int refill_count;
	char buf[KOS_STREAM_BUF_SIZE];
};

/* The pool. */
extern struct kos_stream kos_streams[KOS_STREAM_MAX];

/* The open stream `f` names, or NULL: every entry point starts with
 * this. */
#define KOS_STREAM_SLOT(f) \
	(((f) != (FILE *)0 && (f)->handle != 0) ? (f) : (FILE *)0)

/* Read the next chunk into the stream's buffer (the one place a read
 * touches the file).  Returns the bytes read, 0 at end of file, or the
 * negated error with the stream's error flag set. */
int kos_stream_refill(struct kos_stream *s);

/* Write the stream's buffered bytes out.  Returns 0, or the negated
 * error with the stream's error flag set. */
int kos_stream_push(struct kos_stream *s);

/* Flush the console and every open stream: the exit path's flush (the
 * console client calls it through its hook). */
void kos_stdio_flush_all(void);

#endif /* KOS_SRC_STDIO_INTERNAL_H */
