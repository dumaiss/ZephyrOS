/* KRES host tests: the instrumented in-memory adapter.
 *
 * All tests drive the portable core through this adapter: files live
 * in memory, every read/write/seek/sync is counted, and a one-shot
 * fault can be armed against any op ordinal so the suite exercises
 * the design §11 failure model deterministically.
 */

#ifndef KRES_MEM_IO_H
#define KRES_MEM_IO_H

#include <stdint.h>

#include "kos/kres.h"
#include "kos/errno.h"

/* One stored file's bytes, for snapshots. */
struct mem_file {
	char name[128];
	uint8_t *data;
	uint32_t size;
	uint32_t capacity;
};

/* Reset the store, the counters and every armed fault. */
void mem_io_reset(void);

/* The contract mode: `truncate` never grows a file (the KonaOS
 * provider would extend it with undefined contents).  A growth request
 * answers -KOS_EINVAL; the tests enable this so any reliance on
 * growth-through-truncate fails loudly. */
void mem_truncate_refuses_growth(int on);

/* The adapter.  It supports two simultaneous opens (compaction). */
const struct kres_io *mem_io(void);

/* The op counters (since the last mem_io_reset). */
unsigned int mem_count_writes(void);
unsigned int mem_count_reads(void);
unsigned int mem_count_seeks(void);
unsigned int mem_count_syncs(void);

/* Arm a one-shot fault: the op with this ordinal returns `code`.
 * `short_read` makes that read return half its length instead of an
 * error.  Pass UINT_MAX to disarm. */
void mem_fail_write(unsigned int ordinal, int code);
void mem_fail_read(unsigned int ordinal, int code);
void mem_short_read(unsigned int ordinal);
void mem_fail_seek(unsigned int ordinal, int code);
void mem_fail_sync(unsigned int ordinal, int code);

/* The file named `path`, or NULL. */
const struct mem_file *mem_file_by_name(const char *path);

/* Replace the file's bytes wholesale (fixture corruption).  Returns 0,
 * or -1 when the file is missing. */
int mem_file_put(const char *path, const uint8_t *data, uint32_t size);

/* A fresh file with these bytes (no adapter open involved). */
void mem_file_new(const char *path, const uint8_t *data, uint32_t size);

#endif /* KRES_MEM_IO_H */
