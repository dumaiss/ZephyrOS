# KRES — Kona Resource SDK

**Status:** KRES format 1.0
**Scope:** Portable userland library and utilities; **not** a KonaOS kernel service
**Primary consumers:** `EDIT.EXE`, KonaM2 frontend, Z80 backend, linker, and other applications
**Container extension:** `.KPR` for project/workspace containers; the KRES format is not project-specific

## 1. Purpose

KRES is a small, **WAD-style named-resource container**. It exists to work around the OS's file limits, and it must stay **simple and fast**: every feature below is weighed against code size and card I/O. It stores many logically independent objects in **one ordinary backing file**, so an application can work with many resources while holding **one KonaOS file handle**. That matters because the SD firmware serves at most two open files system-wide (`docs/filesystem.md`).

A container is either **mutable** (edited in place, crash-safe metadata) or **sealed** (built once, read-only, its resources packed one after the other and the catalog last; §8). Both use the same header, the same sorted catalog and the same reader.

A container holds **any number of resources** up to the format's 65,535, within the container's record limit. Its catalog is one contiguous, name-sorted run of entries whose location and capacity the header records; it grows by relocation, never by a compile-time maximum.

Resources are *not* miniature files in a hierarchical filesystem. Each is a named byte sequence backed by **one contiguous allocation** of 512-byte records. A resource grows inside its spare allocated capacity. When it outgrows that capacity, KRES copies it into a larger allocation at the container's allocation mark and abandons the old one. An explicit compaction reclaims abandoned records.

The format and core library are portable C. The KonaOS edition uses the SDK's file API. A host-PC build of the same core provides import, export, verification and compaction without KonaOS.

### 1.1 Goals

- Many **logical resources** through a single open backing file.
- A small, inspectable, deterministic format, implementable in portable C.
- An independent logical cursor per resource, without per-resource OS handles.
- **Contiguous 512-byte records**, favoring sequential and batched I/O.
- Cheap updates within preallocated spare capacity.
- Relocate only the resource that outgrows its allocation, never its neighbors.
- Crash-safe metadata: an interrupted flush leaves the previous catalog intact.
- Explicit compaction, from the C API and from a utility.
- Resource names and payload bytes preserved through host import/export.
- Storage policy and project semantics stay in **userland**, outside KonaOS banks 6/7.
- Serve KonaM2's source, IR, symbol, relocatable object, diagnostics and executable resources without separate physical files.

### 1.2 Non-goals

KRES is **not** a filesystem, database, object store, POSIX layer, archive compressor, transactional storage engine, or substitute for KonaOS VFS. It has no directories, mount points, path resolution, block chains, block-usage tracking, sparse files, interleaved extents, free-space allocator, automatic defragmentation, journal, B-tree or hash index, snapshots, access permissions, or concurrent multi-writer support. Slash (`/`) is an ordinary name byte.

**Compaction** means removing abandoned allocations and, optionally, unused reserved capacity. It does not mean compressing payloads (e.g., ZIP/LZ).

## 2. Layering and ownership

```text
  EDIT.EXE       M2C.EXE       Z80GEN.EXE      KLINK.EXE       other apps
       \             |             |              |               /
        +------------+-------------+--------------+--------------+
                                     |
                         libkres — userland SDK (LIBS=kres)
                    names / catalog / logical handles
                    allocation / buffering / compaction
                                     |
                         one backing-file handle
                                     |
                    KonaOS SDK: open/read/write/seek/sync/truncate
                                     |
                     KonaOS VFS / IOC-FAT provider / FAT
```

- **KonaOS** owns files, byte-stream I/O, synchronization and physical storage.
- **`libkres`** owns resource metadata, offset translation, allocation, relocation and compaction. It is an optional SDK archive like `libtui` and `libgfx`: programs that do not link it pay nothing.
- **Applications** own the meaning of resource names, types, dependencies and project layout.
- **Host tools** are built from the same portable C core, so host and target agree on the format by construction. They handle file import/export and offline maintenance.

Nothing in the format requires a kernel driver, a mounted volume, a new OS namespace, or physical-bank knowledge.

## 3. Resource model

Each live resource has exactly one catalog entry:

| Attribute | Meaning |
|---|---|
| `name` | Unique, opaque name within the container |
| `start_record` | Physical starting record in the backing file |
| `allocated_records` | Contiguous 512-byte records reserved |
| `used_bytes` | Logical length of the resource in bytes |
| `flags` | Reserved; zero |

Invariants:

- `used_bytes <= allocated_records * 512`.
- An allocation starts on a record boundary, lies below the header's `next_record`, and overlaps neither the header, a catalog area, nor another *live* resource.
- A resource is one contiguous extent; there is no extent chain or linked list.
- Reading at or beyond `used_bytes` returns EOF, even with spare capacity.
- Names are unique byte strings within a container, compared byte for byte.
- Deleting or relocating a resource, or relocating a catalog area, leaves the former extent **abandoned** until compaction. Allocation never reuses holes: it only advances `next_record`, for resources and catalogs alike.

### 3.1 Names are flat, opaque bytes

```text
src/MAIN.MOD
src/LIB/MATH.DEF
build/MAIN.REL
```

These are three **flat catalog keys**. Neither `src` nor `build` is a directory. The forward slash has **no internal semantics**: no traversal, parents, directory entries, or implicit creation.

A name is **1–48 bytes**, any value except NUL, compared byte for byte as unsigned values with no case folding, normalization or locale; a proper prefix sorts before the longer name. That byte order is also the catalog order (§4.3). The library attaches no encoding to names. The tools read and print them as **CP850**, KonaOS's character set (the console, the font and KonaEdit). A literal `..` is legal inside KRES; **export tools** reject names that would make unsafe host paths.

## 4. On-disk format

All multibyte integers are **little-endian**, written field by field; no compiler structure layout is part of the format.

`RECORD_SIZE = 512` bytes. A catalog entry is 64 bytes, **eight per record**. Record numbers and lengths are unsigned 32-bit on disk; the resource count is 16-bit, so the format holds at most **65,535 resources**. The library limits a container to **65,535 records (32 MiB)**, so it keeps record numbers in 16 bits on the Z80 (§6.2); a header naming a record beyond the limit is refused at open. The count a container reaches is bounded by that record limit (every resource owns at least one record, every eight entries one catalog record), not by any catalog constant.

### 4.1 Physical layout

The header is record 0. Everything else — resource extents and catalog areas — is allocated at the allocation mark `next_record`, so there is no fixed catalog position.

**A fresh mutable container** has two one-record catalog areas:

```text
record 0                  header (active area A, next_record 3)
record 1                  catalog area A   (capacity 8 entries)
record 2                  catalog area B   (capacity 8 entries)
records 3 ..              resource extents, appended in order
```

The header names the **active** area; each flush writes the whole catalog into the **inactive** one and then switches (§5.6), so an interrupted flush never touches the catalog in use.

**A mutable container after catalog relocation.** When a flush's catalog no longer fits the inactive area, that area moves to the allocation mark with a doubled capacity; its old records become abandoned:

```text
record 0                  header (active B, next_record 21)
record 1                  catalog area A   (1 record, active before)
record 2                  abandoned: area B's old extent
records 3 .. 18           resource extents (and abandoned ones)
records 19 .. 20          catalog area B   (2 records, 16 entries) <- active
```

The next flush that outgrows area A relocates it the same way. Abandoned catalog extents are ordinary abandoned records: compaction drops them.

**A sealed container** has one catalog, written after the last resource:

```text
record 0                  header (SEALED, one catalog, no area B)
records 1 .. N            resource payloads, one after the other
records N+1 .. M          the sorted catalog, ceil(count / 8) records
                          (next_record = M + 1: nothing follows)
```

### 4.2 Header record

Record 0 holds the header. The meaningful fields fill bytes 00h–2Dh; the rest is zero on creation, ignored by open, and refused by `kres_verify` when nonzero.

| Offset | Size | Field | Description |
|---:|---:|---|---|
| `00h` | 4 | `magic` | ASCII `KRES` |
| `04h` | 2 | `major` | `1` |
| `06h` | 2 | `minor` | `0` |
| `08h` | 2 | `record_size` | `512` |
| `0Ah` | 2 | `reserved` | Zero |
| `0Ch` | 4 | `generation` | Incremented by every committed flush; zero when sealed |
| `10h` | 1 | `active_area` | `0` = catalog A, `1` = catalog B; zero when sealed |
| `11h` | 1 | `reserved` | Zero |
| `12h` | 2 | `resource_count` | Live entries in the active catalog |
| `14h` | 4 | `next_record` | Allocation mark: the first record not yet allocated |
| `18h` | 4 | `flags` | Bit 0 `SEALED` (§8); other bits zero |
| `1Ch` | 4 | `catalog_a_start` | Catalog A's first record |
| `20h` | 4 | `catalog_a_records` | Catalog A's capacity in records |
| `24h` | 4 | `catalog_b_start` | Catalog B's first record; zero when sealed |
| `28h` | 4 | `catalog_b_records` | Catalog B's capacity in records; zero when sealed |
| `2Ch` | 2 | `header_check` | CRC-16/CCITT (polynomial 1021h, initial FFFFh, unreflected) of bytes 00h–2Bh |
| `2Eh` | 466 | `reserved` | Zero |

Open accepts a header only when:
- the magic, version, record size and `header_check` match, and no unknown flag is set;
- `next_record` is at most 65,535;
- each catalog extent lies inside records 1 .. `next_record` − 1;
- `active_area` is 0 or 1, and `resource_count` fits the active area: `ceil(count / 8)` ≤ its records;
- for a **mutable** container, both areas have at least one record and do not overlap;
- for a **sealed** one, area B is 0/0, and `active_area` and `generation` are zero.

The catalog capacity is always the allocated records × 8. Unused capacity is padding, never live entries.

**The header is the commit point.** It is one 512-byte record, written with one write and synced. The design assumes that a 512-byte, record-aligned write to the card either lands whole or not at all (the SD card and FatFs write whole sectors); every field that changes in a commit lies in the first 46 bytes. A header that is torn anyway — any mix of old and new bytes — or otherwise corrupted fails `header_check`, and the container is refused (`KRES_E_BAD`) rather than read under a mix of two states. There is no second header to fall back to: detection, not recovery, is the guarantee.

### 4.3 Catalog entries

A **64-byte fixed entry**, eight per record. An entry never crosses a record boundary.

| Offset | Size | Field | Description |
|---:|---:|---|---|
| `00h` | 48 | `name` | Name bytes, zero-padded; not NUL-terminated at full length |
| `30h` | 4 | `start_record` | First allocation record |
| `34h` | 4 | `allocated_records` | Reserved capacity |
| `38h` | 4 | `used_bytes` | Logical byte size |
| `3Ch` | 1 | `name_length` | 1–48 |
| `3Dh` | 1 | `flags` | Zero |
| `3Eh` | 2 | `reserved` | Zero |

The active catalog holds exactly `resource_count` entries, densely from its first record, **sorted by name** in unsigned byte order (§3.1), with no duplicates. Entry *i* lives in record `catalog_start + i / 8` at byte `(i % 8) × 64`. The remaining bytes of the last record are zero. No entry index is a resource identity: entries move when names are inserted, deleted or renamed, and when a container is compacted or sealed.

### 4.4 Lookup and enumeration

The sorted catalog is the directory; there is no other index.

- **Lookup** is a binary search over the entries. Each probe reads the record holding entry *mid* — through a **one-record cache** for a read-only container — and compares names in place. A lookup in an N-entry catalog reads at most `ceil(log2(N + 1))` catalog records, fewer when consecutive probes fall in one record (§11.1 has the measured counts).
- **Enumeration** walks the entries in order through the same cache: one record read per eight entries.
- A read-only open costs one header read. It loads no catalog, allocates no XMEM, and works for any catalog size.

## 5. Mutation and allocation rules

### 5.1 Create

`kres_create(name, capacity_records)` allocates `capacity_records` (at least 1) at `next_record`, advances `next_record`, and inserts a catalog entry with `used_bytes = 0` at the name's sorted position; the later entries shift up one. Creating writes no payload records. Capacity beyond `used_bytes` is never readable, so it is never zero-filled. The physical file grows only when bytes are written; the KonaOS adapter extends the file with zero records when a write lands beyond its physical end.

Editors may request 8 to 20 records of initial capacity; small metadata resources may request one.

### 5.2 Modify within capacity

A write that stays within `allocated_records * 512` modifies the allocation in place, updates `used_bytes` as needed, and touches no other resource. Buffering coalesces reads and writes into 512-byte operations.

A seek past the logical EOF followed by a write is **rejected**. `kres_resize` enlarges explicitly and zero-fills the newly exposed bytes.

### 5.3 Grow beyond capacity

If a write or `kres_reserve` exceeds capacity:

1. Choose a new capacity of at least the required record count.
2. Allocate it at `next_record`; never reuse a hole.
3. Copy the existing logical bytes with the bounded buffer.
4. Apply the pending update, zero-filling newly exposed bytes as required.
5. Point the in-memory catalog entry at the new extent; the old extent is abandoned.
6. The change becomes durable at the next flush (§5.6).

The default growth policy is `max(required, old_capacity + ceil(old_capacity / 2), minimum)`, with an exact-capacity override. This is **policy**, not format.

### 5.4 Shrink, delete, rename, reserve

- **Shrink:** reduce `used_bytes`; keep the capacity for future growth.
- **Delete:** remove the catalog entry (the later entries shift down); the extent is reclaimed only by compaction.
- **Rename:** replace the name, subject to uniqueness and length checks, and move the entry to the new name's sorted position.
- **Reserve:** grow the allocation without changing the logical size; this may relocate.

### 5.5 The catalog store

A **writable** open loads the active catalog into the **catalog store**, a working copy that lookups, creates, deletes, renames and relocations edit; a flush writes it back whole. The store is reached through `struct kres_cache` (`kos/kres.h`), so each edition backs it its own way:

- **KonaOS:** XMEM, never the primary address space. An XMEM object holds at most 32 KiB, so the store is up to 16 chunks of 16 KiB (256 entries each). Chunk 0 starts at 2 KiB (32 entries) and doubles in place to 16 KiB; further chunks are whole. The working set is therefore 2, 4, 8 or 16 KiB up to 32, 64, 128 or 256 entries, then one more 16 KiB chunk per 256 entries, up to **4,096 entries (256 KiB of XMEM)** for one writable container. Inserting and deleting shift the later entries through the 512-byte shared buffer.
- **Host:** heap memory, doubling.

Two stores exist at once (a compaction's target and the sealed builder, or two writable containers). When the store cannot grow — the 4,096-entry ceiling, or XMEM exhausted — the create, rename or open fails with `KOS_ENOSPC` and the catalog is unchanged; any XMEM the failed operation took is freed. Read-only opens never use the store, so a sealed container of any size opens and reads with no XMEM at all.

Open descriptors are not invalidated by entries moving: a descriptor remembers its entry's position together with the container's catalog epoch, and after the epoch changes finds its entry again by its start record (live extents never share one). A descriptor whose resource was deleted reports `KOS_ENOENT` on its next write.

### 5.6 Flush: the commit order

`kres_flush` (and `kres_close`) make every pending change in a mutable container durable in this order:

1. Write all pending resource data, and sync.
2. Write the whole sorted catalog into the **inactive** area, and sync. When `ceil(count / 8)` exceeds that area's capacity, first **relocate** it: allocate a new extent at `next_record` with the larger area's capacity doubled until the catalog fits (exactly the needed records when doubling would pass the record limit), and write the catalog there. The area in use is not touched.
3. Write the header: `active_area` switched, `generation` incremented, `resource_count`, `next_record` and — after a relocation — the moved area's descriptor updated, `header_check` recomputed; sync.

A catalog is relocated only when it outgrows its area: a flush whose inactive area has room writes in place and allocates nothing. Relocation is predictable doubling: 1, 2, 4, 8 … records, each area catching up with the other at its next relocation, so a catalog that has grown to C records has abandoned fewer than 2·C records of earlier areas.

An interruption before step 3's header write leaves the header naming the previous area, whose catalog is intact; records allocated since the last flush — resource extents and a relocated catalog area — lie beyond the committed `next_record` and are simply unreferenced. If the header write itself completed but its sync failed, either the previous or the new state is on the card, each complete (§4.2). The in-memory container keeps its committed state until the header is synced, so a retried flush repeats the same steps. Metadata changes are coalesced to flush or close; nothing rewrites the catalog per byte written. Rewriting the whole catalog per commit is the deliberate trade: commits are rare, and a contiguous sorted catalog needs no other structure.

## 6. SDK API

The shipped API is `kos/kres.h`, described in `docs/sdk.md` §13; this section gives its shape.

The API exposes **one container context with one backing file**, and multiple in-process logical resource descriptors. *Container* and *resource* are distinct from KonaOS file handles.

```c
typedef struct kres_container kres_container;
typedef struct kres_resource  kres_resource;

int kres_open(kres_container *c, const char *path, unsigned flags);
int kres_create_container(const char *path);
int kres_close(kres_container *c);
int kres_flush(kres_container *c);

int kres_get(kres_container *c, const char *name,
             kres_resource *r, unsigned flags);
int kres_create(kres_container *c, const char *name,
                uint16_t capacity_records, kres_resource *r);
int kres_rclose(kres_resource *r);
int kres_read(kres_resource *r, void *dst, uint16_t len);
int kres_write(kres_resource *r, const void *src, uint16_t len);
int kres_seek(kres_resource *r, uint32_t offset);
int kres_resize(kres_resource *r, uint32_t used_bytes);
int kres_reserve(kres_resource *r, uint16_t capacity_records);
int kres_stat(kres_container *c, const char *name, /* out */ void *info);
int kres_delete(kres_container *c, const char *name);
int kres_rename(kres_container *c, const char *old_name,
                const char *new_name);
int kres_iter_begin(kres_container *c, /* out */ void *iterator);
int kres_iter_next(/* iterator */, /* out */ void *info);
int kres_extract(kres_container *c, const char *name, const char *path);
int kres_compact(const char *path, unsigned flags);  /* KRES_TIGHT, KRES_SEAL */
int kres_verify(const char *path, struct kres_usage *usage);

/* Sealed containers (§8). */
int kres_build_begin(const char *path);
int kres_build_add(const char *name);
int kres_build_write(const void *src, uint16_t len);
int kres_build_end(void);
```

Convenience calls for high-throughput callers:

```c
int kres_read_records(kres_resource *r, uint16_t first_record,
                      uint16_t record_count, void *dst);
int kres_write_records(kres_resource *r, uint16_t first_record,
                       uint16_t record_count, const void *src);
```

Record operations are **resource-relative**. The library enforces the capacity and length bounds and handles a partial final record; callers never see container offsets.

### 6.1 Handle and write discipline

- **One backing file handle** per container, and at most one open container per process.
- Multiple logical readers, each with an independent offset.
- **One active writer** per container. Other resources may be read while it writes; the shared buffer is flushed or repositioned between them.
- No concurrent process writers. Another process sees changes after the writer's close and its own reopen.
- `kres_flush` runs the commit order (§5.6) and propagates every error.
- `kres_close` flushes, then closes the handle; callers check its result.
- Descriptors and iterators are invalid after close; no raw catalog pointer escapes.

### 6.2 Buffering, arithmetic and limits

- **One shared buffer:** the core uses a single shared I/O buffer, 512 bytes. It never keeps a buffer per logical resource. A read-only container adds the 512-byte catalog record cache (§4.4).
- **16-bit positions:** the KonaOS edition works in record numbers and entry indexes (16 bits, the 65,535-record and 65,535-entry limits) plus a 9-bit offset within a record. The catalog arithmetic is 16-bit: entry *i* is record `start + (i >> 3)`, offset `(i & 7) << 6`. 32-bit arithmetic is confined to the edges: the `used_bytes` field, byte offsets for the adapter's `seek`, `kres_seek` and `kres_resize`. On SDCC every 32-bit operation costs several times the code and time of a 16-bit one.
- **Seek range:** a container's byte offsets stay below 32 MiB, well inside the SDK's signed `long seek()`.

### 6.3 OS I/O adapter

The core runs on a small adapter:

```c
struct kres_io {
    int (*read)(void *ctx, void *buf, unsigned size);
    int (*write)(void *ctx, const void *buf, unsigned size);
    int (*seek)(void *ctx, uint32_t absolute_offset);
    int (*sync)(void *ctx);
    int (*truncate)(void *ctx, uint32_t length);
};
```

The real adapter also covers open, close and the size query. The KonaOS adapter's `truncate` uses the SDK's `truncate(handle, length)`. That wrapper is a seek followed by the IOC-FAT provider's file truncate, which is the FS2 `TRUNCATE` command (40h): FatFs truncates at the current position. KRES depends on that generic file facility only, never on KRES-specific kernel logic.

## 7. Compaction

Compaction is **explicit**, never part of ordinary writes, and requires every logical resource to be closed.

```text
Before: [HDR][CAT A][dead cat][A][dead][B][dead][C][CAT B][unreferenced tail]
After:  [HDR][CAT A][CAT B][A][B][C]
```

The rebuild reads the source catalog in name order, so every entry appends to the target's catalog without shifting. The target's two areas are created with `ceil(count / 8)` records each (at least one), so its first flush relocates nothing, and every abandoned resource **and catalog** extent of the source is left behind.

Policies:

- **Preserve reservations (default):** copy each live resource's `allocated_records`, keeping editor growth headroom.
- **Tight pack:** copy only `ceil(used_bytes / 512)` records (at least one), releasing reserved slack as well as abandoned extents.
- **Seal:** write a sealed container (§8) from the live resources.

### 7.1 Native compaction: rebuild and rename

KonaOS serves two open files, so the native compactor rebuilds the container in a temporary file and then swaps names, the pattern KonaEdit's save uses:

1. Create a temporary container, its catalog areas sized for the live entries.
2. Copy each live resource into it under the chosen policy, with the bounded buffer.
3. Flush and close it.
4. Rename the original to a recovery name.
5. Rename the temporary to the original name.
6. Remove the recovery file.

An interruption at any step leaves at least one complete, valid container on the card. It needs free space for the live data, and it holds both file slots while it runs. Without the space, compaction fails cleanly and changes nothing; there is no in-place variant.

### 7.2 Host compaction

The host utility rebuilds into a temporary file, verifies it, then replaces the original. It is the preferred maintenance route during early development.

## 8. Sealed containers

A sealed container is built once and then only read: fonts, messages, game assets, a linker's output, a shipped project snapshot. It drops the mutable machinery:

- **Layout:** the header, then the resources **one after the other** from record 1, each starting on the next record after the previous one ends (`allocated_records = ceil(used_bytes / 512)`; an empty resource still owns one zero record), then the sorted catalog in exactly `ceil(count / 8)` records, ending the file (§4.1). The entries mean exactly what they mean in a mutable container, so there is one reader and every read is whole records. The padding costs at most 511 bytes per resource.
- **No commit machinery:** one catalog (area A), no area B, `generation` zero, `next_record` one past the catalog. The header's `SEALED` flag makes the library open the container read-only and refuse every mutation.
- **Building, open-ended:** the resource count need not be known in advance.

  ```text
  kres_build_begin(path)          create the file, a zeroed header record
      kres_build_add(name)        start a resource (closing the previous)
      kres_build_write(src, len)  stream its bytes (only the last write
                                  may end in a partial record)
      ...                         any number of resources
  kres_build_end()                append the sorted catalog after the
                                  last resource, sync, write the header
                                  last, sync, close
  ```

  The catalog accumulates in the catalog store (§5.5) — host memory, or XMEM on KonaOS with the store's 4,096-entry ceiling — sorted as names arrive: a name that sorts after every earlier one appends, others shift the later entries, a duplicate is refused at `kres_build_add` (`KOS_EEXIST`). The host packer sorts its names first, so it only appends.

  An interrupted build leaves a file whose header record is zero, which every reader refuses. No crash-safety machinery is needed: either a complete sealed container exists, or an invalid file does.
- **Sealing a mutable container:** `kres seal` (host) or `kres_compact` with `KRES_SEAL` (native) builds a sealed container from a mutable one's live resources, in name order. That reuses the rebuild copy loop (§7.1), so a project is edited mutable and shipped sealed. A sealed container is never appended to: it is rebuilt, or compacted back to mutable.
- **Library size:** the read path, the mutation and flush machinery, the builder and the catalog store are separate archive members. A program that only reads containers links none of the catalog-commit, allocation, compaction or XMEM-store code.

## 9. Workflows and export

### 9.1 KonaM2 workspace

```text
PROJECT.KPR
    src/MAIN.MOD
    src/MATH.DEF
    src/MATH.MOD
    build/MAIN.IR
    build/MATH.SYM
    build/MAIN.REL
    build/PROGRAM.EXE
    build/ERRORS.TXT
```

Every line is **one resource name**, not a directory hierarchy.

```text
EDIT.EXE       modifies src/MAIN.MOD
M2C.EXE        reads source resources; writes IR/SYM resources
Z80GEN.EXE     reads IR resources; writes REL resources
KLINK.EXE      reads REL resources; writes the final EXE resource
```

KonaOS `run()` executes a child synchronously, and the firmware's two file slots are shared by every process. So each tool flushes and **closes the container before launching the next tool**. The child opens the same `.KPR` afresh, does its stage, closes it and exits; the parent reopens it afterwards.

The executable loader runs an ordinary `.EXE` file. A `build/PROGRAM.EXE` resource is run by extracting it first: `kres_extract` copies it into a file, which the two file slots allow. The loader is unchanged.

### 9.2 Host utilities

```text
kres create   PROJECT.KPR
kres list     PROJECT.KPR
kres show     PROJECT.KPR src/MAIN.MOD
kres import   PROJECT.KPR ./src/MAIN.MOD src/MAIN.MOD
kres extract  PROJECT.KPR src/MAIN.MOD ./src/MAIN.MOD
kres unpack   PROJECT.KPR ./export/
kres pack     ./project/ PROJECT.KPR
kres verify   PROJECT.KPR
kres compact  PROJECT.KPR [--tight]
kres seal     PROJECT.KPR BUNDLE.KPR
```

- **Pack:** packs every regular file under the directory into a sealed container, any number of them up to 65,535, with the names sorted (CP850 bytes) before the build; duplicate names are refused.
- **Verify:** prints the extent accounting — the resource count and catalog capacity, then the records held by the header, the catalog areas, live resources and abandoned extents.

- **Name encoding:** the tools treat names as CP850 and convert at the host boundary only; payloads are byte for byte, with no newline or encoding conversion.
- **Unsafe names:** export rejects absolute paths, drive prefixes, `..` components and other unsafe names.
- **Collisions:** export refuses name collisions on case-insensitive host filesystems instead of overwriting.
- **Git:** the default export policy covers sources and selected project metadata, not transient IR/REL/diagnostic resources unless requested. Git access stays outside `libkres`: users export to a directory and use ordinary Git tools.

## 10. Failure model and validation

- **Metadata:** crash-safe. An interrupted flush leaves the previous committed catalog (§5.6), whether or not it was relocating a catalog area.
- **Header:** a torn or corrupted header fails its check and the container is refused (§4.2).
- **In-place edit within capacity:** a reset mid-write can leave that resource's bytes partially updated. Its metadata stays consistent.
- **Growth:** the new extent is written before the catalog names it. An interruption leaves the old entry and the old bytes intact, plus unreferenced records.
- **Compaction:** the rebuild is crash-safe (§7.1).
- **Sealed build:** an interrupted build leaves an invalid file, never a half-valid container (§8).
- **Store exhaustion:** a create, rename or writable open that cannot grow the catalog store fails with `KOS_ENOSPC` and changes nothing (§5.5).

`kres_verify` checks:
- the header: magic, version, record size, check, flags, reserved bytes, the sealed rules;
- the catalog descriptors: inside the container, mutable areas disjoint and nonempty, the count within the active area's capacity, a sealed catalog exactly `ceil(count / 8)` records ending the container;
- that the file holds the whole active catalog (truncation);
- every entry: name length, no NUL inside the name and zero padding after it, flags and reserved bytes zero, the extent inside records 1 .. `next_record` − 1, `used_bytes` within the allocation and within the file;
- that the names strictly increase — sorted, and unique at once;
- integer overflow in every extent computation;
- that no two of the header, the catalog areas (both, for a mutable container) and the live resource extents share a record.

The overlap pass marks records in a bitmap window of 4,096 records held in the shared buffer, one catalog walk per window (at most 16 for the record limit), so verification needs bounded memory for any catalog size. `struct kres_usage` reports the result: live resource records, catalog records, and **abandoned** records — those below `next_record` that neither references, which compaction reclaims.

It fails closed on a malformed file, and never follows an unchecked offset or trusts a declared size.

## 11. Test and acceptance plan

A host reference suite drives the portable C core through an instrumented single-handle adapter, and the same binary fixtures run under KonaOS in the simulator.

| Test family | Acceptance criterion |
|---|---|
| Create/open/list | Correct header, catalog areas and flat names |
| Names | 1–48 bytes, duplicates refused, `/` opaque, no NUL |
| Logical handles | Independent read cursors, one physical handle |
| Contiguous allocation | Every resource is one record run below `next_record` |
| Update in capacity | Extent start unchanged, neighbors unchanged |
| Overflow growth | New extent at `next_record`, old one abandoned, bytes preserved |
| Resize/shrink | EOF honors the logical length; capacity kept |
| Delete/rename | Catalog-only effects |
| Catalog size | 0, 1, 8, 31, 32, 33, 64, 128, 255, 256, 257, 512 and 1,024 resources, sealed and mutable: verify, sorted walk, every lookup, every byte |
| Lookup | First, middle, last, missing, near-match, prefix, case, `/`, CP850 bytes; bounded catalog reads per lookup |
| Catalog growth | Relocation at each outgrown flush, doubling; reuse of an inactive area with room; abandoned areas reclaimed by compaction |
| Commit order | A failed write or sync at each flush step, with and without catalog relocation, leaves the previous catalog; torn headers refused |
| Buffered I/O | Reads and writes across records and the partial final record |
| Fault injection | Short read/write, failed seek/sync, interrupted copy |
| Verify | Bad descriptors, out-of-range, overlapping, unsorted, duplicate and malformed entries, truncation refused |
| Compaction | Names and data preserved; capacities per policy; rebuild survives interruption |
| Sealed | Built and sealed containers read back byte for byte; mutations refused; an interrupted build refused at open |
| Export/import | Byte-for-byte round trips, CP850 names, safe paths |
| Cross-platform | Host-made containers (1,024 resources) read on the Z80; Z80-made containers, grown across commits, read by the host |
| Pipeline | Editor → M2C → Z80GEN → KLINK, one open container per process |

Use randomized mutation sequences checked against an in-memory oracle, and deterministic corruption fixtures. The KonaOS XMEM catalog store is also run on the host, over an XMEM shim with the native object rules. On the target, measure code size, buffer use and file I/O calls as well as correctness.

### 11.1 Measured costs

Native figures are simulator T-states at 20 MHz with the simulated card; each catalog record read is one 512-byte card read, about 17 ms on the board.

| Catalog | Open (read-only) | Sorted walk | Lookup, average |
|---:|---|---|---|
| 32 | 28 ms, 1 header read | 48 ms, 4 records | 16 ms, 1.7 record reads |
| 256 | 33 ms | 315 ms, 32 records | 46 ms, 6.8 record reads |
| 1,024 | 28 ms | 1,237 ms, 128 records | 61 ms, 9.05 record reads |

A lookup reads at most `ceil(log2(N + 1))` records: 6 at 32 entries, 9 at 256, 11 at 1,024. Creating 40 resources on the card with a commit every ten (four catalog relocations) took 1.75 s; compacting the result 1.37 s.

| Footprint (KonaOS) | Code | Static data | XMEM |
|---|---:|---:|---:|
| Read-only (util, read, adapter read side) | 7,325 B | 1,048 B | none |
| Mutation adds (mut, adapter write side, XMEM store) | 12,493 B | 76 B | per writable container: 2 KiB up to 32 entries, doubling to 16 KiB at 256, then 16 KiB per 256 entries (≤ 256 KiB) |
| Builder, compaction, verify, extract | 2,032 / 1,778 / 2,443 / 230 B | 18 / 320 / 4 / 0 B | the builder's store as above |

## 12. Delivery sequence

**R0 — The format.** Header, relocatable sorted catalog and entry encoding, name rules, limits, binary fixtures.

**R1 — Portable core and host tests.** Open/create/list/read/write, capacity, relocation, commit order, verify; instrument one-handle behavior.

**R2 — Compaction, sealed containers and host utilities.** Rebuild compaction, the builder and sealing, verify, import/export, round trips.

**R3 — KonaOS adapter and `truncate`.** The SDK `truncate(handle, length)` over the provider's FS2 truncate; the record cache and the XMEM catalog store; bounded buffering; native acceptance.

**R4 — EDIT.EXE integration.** Open and save named resources in a `.KPR` alongside ordinary files; reopen around child runs. KonaEdit has little room left, so this needs a lean subset (open, read whole, write whole, flush) or far code.

**R5 — KonaM2 integration.** Source → IR → REL → linked EXE as resource stages, sharing the library without container-layout logic in the compiler modules.

## 13. Open decisions

1. Default initial capacities and growth minimums for source and binary resources (policy, not format).
2. An optional in-memory lookup aid (for example caching the top records of the binary search) if measured lookup cost on the board calls for it; the sorted catalog stays the only on-disk directory.

---

**Architectural rule:** KRES is a **flat, contiguous-record, mutable resource archive**, not a miniature filesystem. Its value is moving richer storage policy into reusable **userland** while KonaOS stays a small byte-stream provider.
