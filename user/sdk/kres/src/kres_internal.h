/* KRES core internal state (private; not installed).
 *
 * One container context with one backing file, opened through the
 * installed `struct kres_io` adapter, and multiple in-process logical
 * resource descriptors.  The catalog cache is the four-record area the
 * current catalog lives in, reached through a small interface so the
 * host backs it with memory and the KonaOS edition with XMEM.
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

/* The mutation-side hooks (installed by the mutation member's startup
 * initializer; NULL when no mutation member is linked). */
extern int (*kres_create_hook)(const char *path);
extern int (*kres_flush_hook)(kres_container *c);

/* --- the catalog cache interface -------------------------------------- */

/* Defined in kos/kres.h: `load` fills the four records of `area`
 * (KRES_AREA_A_BASE or _B_BASE) into the backend's storage.  `entry`
 * copies entry `index` (0..31) into the 64-byte `out` buffer.  `put`
 * replaces entry `index`.  `store` writes the four records back to
 * `area`.  All return 0 or a negated error.  No backend pointer
 * escapes, so the KonaOS XMEM backend fits the same interface. */

/* The cache a container uses (set to &kres_cache_mem by the host glue;
 * the KonaOS edition substitutes its XMEM backend). */
extern const struct kres_cache *kres_cache;

/* --- container and resource state ---------------------------------------- */

/* struct kres_container and struct kres_resource are defined in
 * kos/kres.h; the private fields below belong to the library. */

/* --- shared internals -------------------------------------------------- */

/* The byte offset of a record number. */
#define kres_record_offset(record) ((record) * KRES_RECORD_SIZE)

/* The longest path the core buffers (compaction's swap and recovery
 * spellings); the KonaOS edition's own limit is 127 bytes plus NUL. */
#define KRES_PATH_MAX 160u

/* One full-record adapter operation with the shared buffer, in three
 * bounded calls; `length` is a whole number of records.  Returns the
 * count of records read/written, or a negative error. */
int kres_io_blocks(void *ctx, uint32_t record, uint8_t *buffer,
		   uint32_t count, int writing);

/* Read `count` whole records into `dst` (which is at least count *
 * 512 bytes).  Returns the records actually read, or a negative
 * error. */
int kres_read_records_at(void *ctx, uint32_t record, uint8_t *dst,
			 uint32_t count);

/* Parse and validate the header in `header`, filling `out`'s fields.
 * Returns 0, or a negated error (KRES_E_BAD, KOS_EIO). */
int kres_header_parse(const uint8_t *header, kres_container *out);

/* Fill one catalog entry from a live resource's in-memory state. */
void kres_entry_emit(uint8_t *entry, const uint8_t *name,
		     uint8_t name_length, uint32_t start,
		     uint32_t allocated, uint32_t used);

/* Parse one catalog entry; fills the kres_info form. */
void kres_entry_info(const uint8_t *entry, struct kres_info *out);

/* Name helpers: length 1..48, no NUL, in `name`; returns 0xFFFF on
 * a bad name. */
uint16_t kres_name_check(const char *name);

/* Locate `name` in the cache; returns the entry index (0..31) or
 * 0xFFFF when absent. */
unsigned int kres_find(kres_container *c, const char *name,
		       uint8_t name_length);

/* Copy catalog entry `index` into `out` (64 bytes). */
int kres_entry_get(kres_container *c, unsigned int index, uint8_t *out);

/* Replace one field of entry `index` in the cache (`offset` is a
 * KRES_ENT_* field start). */
int kres_entry_put32(kres_container *c, unsigned int index,
		     uint16_t offset, uint32_t value);

/* Fill a resource descriptor from catalog entry `index`
 * (kres_read.c). */
int resource_fill(kres_container *c, unsigned int index,
		  kres_resource *r);

#endif /* KRES_INTERNAL_H */
