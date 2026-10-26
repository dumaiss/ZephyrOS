/* kos/reader.h - the shared read-at view (libkos).
 *
 * One small interface over every thing a program can read sequentially:
 * an OS file handle, a `FILE *`, a KRES resource, or a caller-owned
 * byte array.  A reader is a borrowed, positioned view: every
 * `read_at` is absolute from byte zero, the `size` is the exact
 * logical length captured at construction, and no adapter closes its
 * source.  The adapters are separate archive members, so including
 * this header links nothing and each constructor pulls only its own
 * source library.
 *
 * The read contract:
 *   - `read_at` returns 0..requested bytes, 0 at the end, or a
 *     negative error.  A short read is legal; consumers retry through
 *     `kos_reader_read_full`.
 *   - `kos_reader_read_full` retries short reads until the range is
 *     complete: it returns the length, a negative error, or -KOS_EIO
 *     at an early end.
 */

#ifndef KOS_READER_H
#define KOS_READER_H

#include "kos/types.h"

struct kos_stream;
struct kres_resource;

/* Absolute read from byte `offset`: 0..requested, 0 at the end, or a
 * negative error.  The source is borrowed; the adapter never closes
 * it. */
typedef int (*kos_read_at_fn)(void *context, uint32_t offset,
			      void *dst, uint16_t requested);

/* The borrowed view: three fields only. */
typedef struct kos_reader {
	void *context;	       /* the borrowed backing object */
	kos_read_at_fn read_at;
	uint32_t size;	       /* the exact logical byte length */
} kos_reader_t;

/* The sources an adapter borrows.  The memory and handle constructors
 * take a small caller-owned descriptor; the stdio and KRES sources are
 * already descriptor-bearing objects. */
typedef struct kos_handle_source {
	unsigned int handle;
} kos_handle_source_t;

typedef struct kos_memory_source {
	const void *data;
	uint32_t size;
} kos_memory_source_t;

int kos_reader_from_handle(kos_reader_t *out, kos_handle_source_t *source);
int kos_reader_from_stdio(kos_reader_t *out, struct kos_stream *stream);
int kos_reader_from_kres(kos_reader_t *out, struct kres_resource *resource);
int kos_reader_from_memory(kos_reader_t *out, kos_memory_source_t *source);

int kos_reader_read_full(const kos_reader_t *reader, uint32_t offset,
			 void *dst, uint16_t length);

#endif /* KOS_READER_H */
