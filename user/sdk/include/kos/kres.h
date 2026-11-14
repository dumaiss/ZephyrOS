/* KRES — Kona Resource SDK (docs/kres-sdk-design.md).
 *
 * A small WAD-style named-resource container in one backing file: any
 * number of named byte resources (up to 65,535), each one contiguous run
 * of 512-byte records, listed in one contiguous catalog sorted by name,
 * with crash-safe catalog commits and an explicit rebuild compaction.
 * Sealed containers drop the mutation machinery and are read-only.
 *
 * The portable core reaches storage only through `struct kres_io`
 * (section 6.3 of the design); the host edition ships a stdio adapter
 * and the KonaOS edition the SDK file API.  Programs that do not link
 * libkres pay nothing for it.
 *
 * Error convention: calls return 0 on success, or a negated KOS_E* /
 * KRES_E* code.
 */

#ifndef KOS_KRES_H
#define KOS_KRES_H

#include "kos/types.h"

/* Format constants (docs/kres-sdk-design.md §4). */
#define KRES_RECORD_SIZE     512u
#define KRES_ENTRIES_PER_RECORD 8u
#define KRES_NAME_MAX        48u
#define KRES_RESOURCE_MAX    65535u /* the 16-bit resource count       */
#define KRES_RECORD_LIMIT    65535u

/* Open flags. */
#define KRES_OPEN_READ   0x01u /* existing container, read path      */
#define KRES_OPEN_WRITE  0x02u /* existing container, mutation       */
#define KRES_OPEN_CREATE 0x04u /* create an empty container if the
				  file is missing (with WRITE)        */

/* Compaction policies (kres_compact). */
#define KRES_TIGHT 0x01u /* copy only the used records               */
#define KRES_SEAL  0x02u /* rebuild as a sealed container            */

/* KRES-specific errors, returned negated.  Common conditions use the
 * KOS codes (kos/errno.h): bad names and bounds KOS_EINVAL, duplicate
 * names KOS_EEXIST, missing resources KOS_ENOENT, sealed mutations
 * KOS_EROFS, adapter failures KOS_EIO, exhausted storage KOS_ENOSPC. */
#define KRES_E_FULL   0x30 /* the 65,536th resource                 */
#define KRES_E_RECLIM 0x31 /* the 65,535-record limit               */
#define KRES_E_BAD    0x32 /* not a valid KRES file                 */
#define KRES_E_GROW   0x33 /* a growth request cannot be satisfied  */

/* One catalog entry, as kres_stat and the iterator report it. */
struct kres_info {
	uint8_t name[KRES_NAME_MAX + 1u]; /* NUL-terminated */
	uint8_t name_length;       /* 1..48 */
	uint8_t flags;             /* zero */
	uint8_t reserved;
	uint32_t used_bytes;
	uint32_t allocated_records;
};

struct kres_io {
	/* Every call returns 0, or a negated KOS_E* code.  `open`
	 * receives the path and the KRES_OPEN_* flags; `size` fills
	 * the file's byte length. */
	int (*open)(void *ctx, const char *path, unsigned int flags);
	int (*close)(void *ctx);
	int (*size)(void *ctx, uint32_t *out);
	int (*read)(void *ctx, void *buf, unsigned int size);
	int (*write)(void *ctx, const void *buf, unsigned int size);
	int (*seek)(void *ctx, uint32_t absolute_offset);
	int (*sync)(void *ctx);
	int (*truncate)(void *ctx, uint32_t length);
	int (*rename)(void *ctx, const char *from, const char *to);
	int (*unlink)(void *ctx, const char *path);
};

typedef struct kres_container kres_container;
typedef struct kres_resource kres_resource;

/* --- container and resource state ----------------------------------------
 *
 * The field layout below is private: callers allocate the descriptors
 * and pass pointers, but only the library reads or writes the fields.
 * A descriptor is valid until its kres_close/kres_rclose, or until the
 * containing descriptor is closed.  Record numbers fit 16 bits (the
 * 65,535-record limit is checked at open). */

struct kres_container {
	void *io_ctx;            /* the adapter's per-open context      */
	uint8_t opened;          /* an open succeeded                   */
	uint8_t sealed;          /* the SEALED header flag              */
	uint8_t writable;        /* opened with KRES_OPEN_WRITE         */
	uint8_t dirty;           /* catalog/header changes pending      */
	uint32_t generation;     /* committed generation                */
	uint32_t next_record;    /* the allocation mark                 */
	uint16_t resource_count; /* live catalog entries                */
	uint8_t active_area;     /* 0 = catalog A, 1 = catalog B        */
	uint16_t catalog_start[2];   /* the two catalog extents (a sealed */
	uint16_t catalog_records[2]; /* container uses A only)            */
	uint16_t epoch;          /* bumped when catalog entries move    */
	uint32_t file_size;      /* backing file length                 */
};

struct kres_resource {
	kres_container *container;
	uint8_t open;
	uint8_t writable;    /* open for write */
	uint16_t entry_index; /* the entry's position at `epoch` */
	uint16_t epoch;
	uint32_t start_record;
	uint32_t allocated_records;
	uint32_t used_bytes;
	uint32_t cursor;     /* logical byte offset */
};

/* The catalog walk, in name order. */
struct kres_iter {
	kres_container *container;
	uint16_t index;
};

/* What kres_verify found: the live resource extents, the catalog
 * extents (both areas of a mutable container) and the records below
 * the allocation mark that neither references (abandoned, reclaimed by
 * compaction). */
struct kres_usage {
	uint16_t resources;
	uint32_t catalog_capacity;  /* entries the active catalog holds */
	uint32_t next_record;
	uint32_t live_records;
	uint32_t catalog_records;
	uint32_t abandoned_records;
};

/* --- container -------------------------------------------------------- */

/* The catalog store: the working copy of a writable container's
 * catalog (and of the sealed builder's), reached through a small
 * interface so the host backs it with memory (kres_cache_mem) and the
 * KonaOS edition with XMEM.  Every call is keyed by `ctx`.
 *
 *   load(ctx, start, count)   start a store holding the `count` entries
 *                             of the catalog at record `start` (count 0
 *                             starts an empty store);
 *   entry(ctx, index, out)    copy entry `index` into the 64-byte `out`;
 *   put(ctx, index, entry)    replace entry `index`;
 *   insert(ctx, index, count) open a slot at `index` among `count`
 *                             entries (its bytes are then put);
 *   remove(ctx, index, count) close the slot at `index`;
 *   store(ctx, start, count)  write the `count` entries as whole records
 *                             at record `start`, the final record
 *                             zero-padded;
 *   release(ctx)              drop the store.
 *
 * All return 0 or a negated error; a store that cannot grow answers
 * -KOS_ENOSPC.  No backend pointer escapes.  Read-only containers do
 * not use it: they read the catalog one record at a time. */
struct kres_cache {
	int (*load)(void *ctx, uint16_t start, uint16_t count);
	int (*entry)(void *ctx, uint16_t index, uint8_t *out);
	int (*put)(void *ctx, uint16_t index, const uint8_t *entry);
	int (*insert)(void *ctx, uint16_t index, uint16_t count);
	int (*remove)(void *ctx, uint16_t index, uint16_t count);
	int (*store)(void *ctx, uint16_t start, uint16_t count);
	int (*release)(void *ctx);
};

/* The memory backend (kres_cache_mem.c): host heap, two stores. */
extern const struct kres_cache kres_cache_mem;

/* Install the platform adapter and the per-process context (the
 * platform glue calls this once, before the first container call).
 * The read-only install: no mutation code and no catalog store is
 * linked through it, and containers open read-only. */
void kres_install(const struct kres_io *io, void *ctx);

/* The mutation-capable install (kres_mut): kres_install plus the
 * catalog store and the read path's hooks, so writable opens,
 * kres_open's create-on-missing fallback and kres_close's flush work.
 * A program that only reads calls plain kres_install. */
int kres_install_mut(const struct kres_io *io,
		     const struct kres_cache *cache, void *ctx);

/* Bind `c` to `path` through the installed adapter.  KRES_OPEN_CREATE
 * makes a missing file an empty mutable container. */
int kres_open(kres_container *c, const char *path, unsigned int flags);
int kres_create_container(const char *path);
int kres_close(kres_container *c);
int kres_flush(kres_container *c);

/* --- resources -------------------------------------------------------- */

int kres_get(kres_container *c, const char *name, kres_resource *r,
	     unsigned int flags);
int kres_create(kres_container *c, const char *name,
		uint16_t capacity_records, kres_resource *r);
int kres_rclose(kres_resource *r);
int kres_read(kres_resource *r, void *dst, uint16_t len);
int kres_write(kres_resource *r, const void *src, uint16_t len);
int kres_seek(kres_resource *r, uint32_t offset);
int kres_resize(kres_resource *r, uint32_t used_bytes);
int kres_reserve(kres_resource *r, uint16_t capacity_records);

/* Record operations are resource-relative; the library enforces the
 * capacity and length bounds and handles a partial final record. */
int kres_read_records(kres_resource *r, uint16_t first_record,
		      uint16_t record_count, void *dst);
int kres_write_records(kres_resource *r, uint16_t first_record,
		       uint16_t record_count, const void *src);

/* --- catalog ---------------------------------------------------------- */

int kres_stat(kres_container *c, const char *name, struct kres_info *out);
int kres_delete(kres_container *c, const char *name);
int kres_rename(kres_container *c, const char *old_name,
		const char *new_name);
int kres_iter_begin(kres_container *c, struct kres_iter *it);
int kres_iter_next(struct kres_iter *it, struct kres_info *out);

/* --- maintenance ------------------------------------------------------ */

/* Copy one resource's bytes into a new file at `path`. */
int kres_extract(kres_container *c, const char *name, const char *path);

/* Rebuild `path` in a temporary file and swap names; KRES_TIGHT drops
 * reserved slack, KRES_SEAL produces a sealed container.  The default
 * preserves every live reservation. */
int kres_compact(const char *path, unsigned int flags);

/* The validation pass: fail closed on a malformed file.  `usage`
 * (may be NULL) receives the extent accounting. */
int kres_verify(const char *path, struct kres_usage *usage);

/* --- the sealed builder ----------------------------------------------- */

/* The sealed builder, open-ended: begin creates the file with a blank
 * header; each add() starts a resource (closing the previous one) and
 * write() streams its bytes; end() appends the sorted catalog after
 * the last resource and writes the header last.  The catalog
 * accumulates in the catalog store (kres_install_mut), sorted as names
 * arrive: names added in order append, others shift the later
 * entries.  An interrupted build is aborted with kres_build_abort (the
 * build file is closed and left invalid). */
int kres_build_begin(const char *path);
int kres_build_add(const char *name);
int kres_build_write(const void *src, uint16_t len);
int kres_build_end(void);
int kres_build_abort(void);

#endif /* KOS_KRES_H */
