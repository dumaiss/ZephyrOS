/* KRES core internal state (private; not installed).
 *
 * One container context with one backing file, opened through the
 * installed `struct kres_io` adapter, and multiple in-process logical
 * resource descriptors.  A read-only container reads its sorted catalog
 * through a one-record cache; a writable container (and the sealed
 * builder) keeps its whole catalog in the installed catalog store.
 */

#ifndef KRES_INTERNAL_H
#define KRES_INTERNAL_H

/* The hosted <string.h> on the host, the SDK's klibc subset on the
 * target (--nostdinc: no hosted headers exist there). */
#if defined(__SDCC)
#include "klibc/string.h"
#else
#include <string.h>
#endif

#include "kos/errno.h"
#include "kos/kres.h"

#include "kres_layout.h"

/* The one shared I/O buffer: 512 bytes, never one per resource. */
extern uint8_t kres_shared[KRES_RECORD_SIZE];

/* A whole zero record in const storage: the file-extension writes. */
extern const uint8_t kres_zero_page[KRES_RECORD_SIZE];

/* The platform adapter, installed before kres_open. */
extern const struct kres_io *kres_io;

/* The installed per-process context (kres_install). */
extern void *kres_ctx;

/* The catalog store (kres_install_mut; NULL after a read-only
 * install). */
extern const struct kres_cache *kres_cache;

/* The mutation-side hooks (installed by kres_install_mut; NULL when no
 * mutation member is linked). */
extern int (*kres_create_hook)(const char *path);
extern int (*kres_flush_hook)(kres_container *c);

/* --- shared internals -------------------------------------------------- */

/* The byte offset of a record number. */
#define kres_record_offset(record) ((uint32_t)(record) * KRES_RECORD_SIZE)

/* The longest path the core buffers (compaction's swap and recovery
 * spellings); the KonaOS edition's own limit is 127 bytes plus NUL. */
#define KRES_PATH_MAX 160u

/* No entry: kres_find's miss. */
#define KRES_NO_ENTRY 0xFFFFu

/* One full-record adapter operation with the shared buffer: `count`
 * whole records at the current position, into or out of `buffer`.
 * Returns the count of records moved, or a negative error. */
int kres_io_blocks(void *ctx, uint8_t *buffer, uint16_t count,
		   int writing);

/* Read `count` whole records at `record` into `dst` (at least count *
 * 512 bytes).  Returns the records actually read, or a negative
 * error. */
int kres_read_records_at(void *ctx, uint32_t record, uint8_t *dst,
			 uint16_t count);

/* Parse and validate the header in `header`, filling `out`'s fields.
 * Returns 0, or -KRES_E_BAD. */
int kres_header_parse(const uint8_t *header, kres_container *out);

/* Build the header bytes for `c`'s state into `header` (512 bytes). */
void kres_header_emit(const kres_container *c, uint8_t *header);

/* Fill one catalog entry. */
void kres_entry_emit(uint8_t *entry, const uint8_t *name,
		     uint8_t name_length, uint32_t start,
		     uint32_t allocated, uint32_t used);

/* Parse one catalog entry; fills the kres_info form. */
void kres_entry_info(const uint8_t *entry, struct kres_info *out);

/* Name helpers: length 1..48, no NUL, in `name`; returns 0xFFFF on
 * a bad name. */
uint16_t kres_name_check(const char *name);

/* The byte order of the catalog: <0, 0 or >0 as `name` (`length`
 * bytes) sorts before, with or after `entry`'s name; unsigned bytes,
 * a proper prefix first. */
int kres_name_order(const uint8_t *name, uint8_t length,
		    const uint8_t *entry);

/* Entry `index` of the container's catalog: a pointer into the record
 * cache (read-only containers), or `scratch` (64 bytes) filled from
 * the catalog store (writable ones).  NULL on an I/O error. */
const uint8_t *kres_entry_at(kres_container *c, uint16_t index,
			     uint8_t *scratch);

/* The binary search: returns 1 when `name` is entry *pos, 0 when it is
 * absent (*pos is then its sorted insertion point), or a negated
 * error. */
int kres_search(kres_container *c, const char *name, uint8_t length,
		uint16_t *pos);

/* Locate `name`; returns the entry index or KRES_NO_ENTRY. */
uint16_t kres_find(kres_container *c, const char *name,
		   uint8_t name_length);

/* Copy catalog entry `index` into `out` (64 bytes). */
int kres_entry_get(kres_container *c, uint16_t index, uint8_t *out);

/* Drop the record cache if it belongs to `c`. */
void kres_record_cache_drop(const kres_container *c);

/* Fill a resource descriptor from catalog entry `index`. */
int resource_fill(kres_container *c, uint16_t index, kres_resource *r);

/* An empty mutable container at the open adapter `ctx` whose catalog
 * areas hold `catalog_records` records each (kres_mut.c). */
int kres_create_sized(const char *path, uint16_t catalog_records);

#endif /* KRES_INTERNAL_H */
