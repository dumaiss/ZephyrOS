/* KRES — Kona Resource SDK (docs/kres-sdk-design.md).
 *
 * A small WAD-style named-resource container in one backing file: up to
 * 32 named byte resources, each one contiguous run of 512-byte records,
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
#define KRES_CATALOG_RECORDS 4u
#define KRES_NAME_MAX        48u
#define KRES_RESOURCE_MAX    32u
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
#define KRES_E_FULL   0x30 /* the 33rd resource: catalog full       */
#define KRES_E_RECLIM 0x31 /* the 65,535-record limit               */
#define KRES_E_BAD    0x32 /* not a KRES v1 file                    */
#define KRES_E_GROW   0x33 /* a growth request cannot be satisfied  */

/* One catalog entry, as kres_stat and the iterator report it. */
struct kres_info {
	uint8_t name[KRES_NAME_MAX];
	uint8_t name_length;       /* 1..48 */
	uint8_t flags;             /* zero in v1 */
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
 * containing descriptor is closed. */

struct kres_container {
	void *io_ctx;            /* the adapter's per-open context      */
	uint8_t opened;          /* an open succeeded                   */
	uint8_t sealed;          /* the SEALED header flag              */
	uint8_t writable;        /* opened with KRES_OPEN_WRITE         */
	uint8_t dirty;           /* catalog/header changes pending      */
	uint32_t generation;     /* committed generation                */
	uint32_t next_record;    /* the allocation mark                 */
	uint16_t resource_count; /* live entries in the active area     */
	uint8_t active_area;     /* 0 = A, 1 = B                        */
	uint32_t file_size;      /* backing file length at open         */
};

struct kres_resource {
	kres_container *container;
	uint8_t open;
	uint8_t writable;    /* open for write */
	uint8_t entry_index; /* index in the active catalog */
	uint8_t reserved;
	uint32_t start_record;
	uint32_t allocated_records;
	uint32_t used_bytes;
	uint32_t cursor;     /* logical byte offset */
};

/* One independent logical cursor per resource. */
struct kres_iter {
	kres_container *container;
	unsigned int index;
};

/* --- container -------------------------------------------------------- */

/* The catalog cache: the four-record area the current catalog lives
 * in, reached through a small interface so the host backs it with
 * memory (kres_cache_mem) and the KonaOS edition with XMEM.  `load` fills the
 * four records of `area_record` into the backend's storage; `entry`
 * copies entry `index` (0..31) into the 64-byte `out`; `put` replaces
 * entry `index`, starting an empty area when none is loaded (the
 * sealed builder accumulates its catalog this way); `store` writes the
 * four records back; `release` drops the area loaded for `ctx` (a
 * container close).  All return 0 or a negated error.  No backend
 * pointer escapes. */
struct kres_cache {
	int (*load)(void *ctx, uint32_t area_record);
	int (*entry)(void *ctx, unsigned int index, uint8_t *out);
	int (*put)(void *ctx, unsigned int index, const uint8_t *entry);
	int (*store)(void *ctx, uint32_t area_record);
	int (*release)(void *ctx);
};

/* The memory backend (kres_cache_mem.c): a 2 KiB static buffer. */
extern const struct kres_cache kres_cache_mem;

/* Install the platform adapter, the catalog cache and the per-process
 * context both receive (the platform glue calls this once, before the
 * first container call).  The read-only install: no mutation code is
 * linked through it. */
void kres_install(const struct kres_io *io,
		  const struct kres_cache *cache, void *ctx);

/* The mutation-capable install (kres_mut): kres_install plus the
 * read path's hooks, so kres_open's create-on-missing fallback and
 * kres_close's flush work.  A program that only reads calls plain
 * kres_install. */
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

/* The §10 validation pass: fail closed on a malformed file. */
int kres_verify(const char *path);

/* --- the sealed builder ----------------------------------------------- */

/* The sealed builder: begin creates the file with the catalog reserved
 * at the front; each add() starts a resource (closing the previous
 * one) and write() streams its bytes; end() writes the catalog and the
 * header last, its magic the final bytes.  An interrupted build is
 * aborted with kres_build_abort (the build file is closed and left
 * invalid). */
int kres_build_begin(const char *path, uint8_t entry_count);
int kres_build_add(const char *name);
int kres_build_write(const void *src, uint16_t len);
int kres_build_end(void);
int kres_build_abort(void);

#endif /* KOS_KRES_H */
