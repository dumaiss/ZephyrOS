# KRES — Kona Resource SDK

**Status:** KRES format v1 frozen
**Scope:** Portable userland library and utilities; **not** a KonaOS kernel service
**Primary consumers:** `EDIT.EXE`, KonaM2 frontend, Z80 backend, linker, and other applications
**Container extension:** `.KPR` for project/workspace containers; the KRES format is not project-specific

## 1. Purpose

KRES is a small, **WAD-style named-resource container**. It exists to work around the OS's file limits, and it must stay **simple and fast**: every feature below is weighed against code size and card I/O. It stores many logically independent objects in **one ordinary backing file**, so an application can work with many resources while holding **one KonaOS file handle**. That matters because the SD firmware serves at most two open files system-wide (`docs/filesystem.md`).

A container is either **mutable** (edited in place, crash-safe metadata) or **sealed** (built once, read-only, its resources packed one after the other; §8). Both use the same layout and the same reader.

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

KRES v1 is **not** a filesystem, database, object store, POSIX layer, archive compressor, transactional storage engine, or substitute for KonaOS VFS. It has no directories, path resolution, block chains, block-usage tracking, sparse files, interleaved extents, free-space allocator, journal, snapshots, access permissions, or concurrent multi-writer support. A container holds at most **32 resources**. Slash (`/`) is an ordinary name byte.

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
| `flags` | Reserved; zero in v1 |

Invariants:

- `used_bytes <= allocated_records * 512`.
- An allocation starts on a record boundary, lies below the header's `next_record`, and overlaps neither the header, the catalog areas, nor another *live* resource.
- A resource is one contiguous extent; there is no extent chain or linked list.
- Reading at or beyond `used_bytes` returns EOF, even with spare capacity.
- Names are unique byte strings within a container, compared byte for byte.
- Deleting or relocating a resource leaves its former extent **unreferenced** until compaction. Allocation never reuses holes: it only advances `next_record`.

### 3.1 Names are flat, opaque bytes

```text
src/MAIN.MOD
src/LIB/MATH.DEF
build/MAIN.REL
```

These are three **flat catalog keys**. Neither `src` nor `build` is a directory. The forward slash has **no internal semantics**: no traversal, parents, directory entries, or implicit creation.

A name is **1–48 bytes**, any value except NUL, compared byte for byte with no case folding or canonicalization. The library attaches no encoding to names. The tools read and print them as **CP850**, KonaOS's character set (the console, the font and KonaEdit). A literal `..` is legal inside KRES; **export tools** reject names that would make unsafe host paths.

## 4. On-disk format (v1)

All multibyte integers are **little-endian**. No compiler structure padding is part of the format.

`RECORD_SIZE = 512` bytes, permanently for format v1. `CATALOG_RECORDS = 4`: a catalog is four records, 32 entries. Record numbers and lengths are unsigned 32-bit on disk. The KonaOS edition limits a container to **65,535 records (32 MiB)**, so it can keep record numbers in 16 bits (§6.2). A file beyond the native limit is refused at open.

### 4.1 Physical layout

A mutable container:

```text
record 0                      header
records 1 .. 4                catalog area A
records 5 .. 8                catalog area B
records 9 .. next_record-1    resource allocations, appended in order,
                              with abandoned extents between them
```

The catalog is **fixed at the front**: two areas of four records. The header names the active area, and each flush writes the other one (§5.6), so an interrupted flush never damages the catalog in use. A sealed container has a single area (records 1–4); its resources start at record 5 (§8).

### 4.2 Header record

Record 0 holds the header. Unassigned bytes are zero on creation and ignored on read in v1.

| Offset | Size | Field | Description |
|---:|---:|---|---|
| `00h` | 4 | `magic` | ASCII `KRES` |
| `04h` | 2 | `major` | `1` |
| `06h` | 2 | `minor` | `0` |
| `08h` | 2 | `record_size` | `512` |
| `0Ah` | 2 | `catalog_records` | `4` (records per catalog area) |
| `0Ch` | 4 | `generation` | Incremented by every committed flush |
| `10h` | 1 | `active_area` | `0` = A, `1` = B |
| `11h` | 1 | `reserved` | Zero |
| `12h` | 2 | `resource_count` | Live entries in the active area |
| `14h` | 4 | `next_record` | Allocation mark: the first record not yet allocated |
| `18h` | 4 | `flags` | Bit 0 `SEALED` (§8); other bits zero |
| `1Ch` | 484 | `reserved` | Zero |

The header is one 512-byte write, the container's single commit point.

### 4.3 Catalog entries

A **64-byte fixed entry**, eight per record. An entry never crosses a record boundary, so updating one entry touches exactly one record.

| Offset | Size | Field | Description |
|---:|---:|---|---|
| `00h` | 48 | `name` | Name bytes, zero-padded; not NUL-terminated at full length |
| `30h` | 4 | `start_record` | First allocation record |
| `34h` | 4 | `allocated_records` | Reserved capacity |
| `38h` | 4 | `used_bytes` | Logical byte size |
| `3Ch` | 1 | `name_length` | 1–48 |
| `3Dh` | 1 | `flags` | Zero in v1 |
| `3Eh` | 2 | `reserved` | Zero |

The active area holds exactly `resource_count` entries, densely, from its first record. A container holds at most 32 resources; creating a 33rd fails with a "catalog full" error. No entry address is a public resource ID. Enumeration order is unspecified and may change after a delete or compaction; an application that needs sorted order sorts in userland.

## 5. Mutation and allocation rules

### 5.1 Create

`kres_create(name, capacity_records)` allocates `capacity_records` (at least 1) at `next_record`, advances `next_record`, and adds a catalog entry with `used_bytes = 0`. Creating writes no payload records. Capacity beyond `used_bytes` is never readable, so it is never zero-filled. The physical file grows only when bytes are written; the KonaOS adapter extends the file with zero records when a write lands beyond its physical end.

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
- **Delete:** remove the catalog entry; the extent is reclaimed only by compaction.
- **Rename:** replace the name, subject to uniqueness and length checks.
- **Reserve:** grow the allocation without changing the logical size; this may relocate.

### 5.5 Catalog cache

`kres_open` loads the active catalog area into an XMEM cache, and lookups, creates, deletes, renames and relocations work on the cache. A lookup costs no disk read: on the board, reading 512 bytes takes about 17 ms, so scanning a catalog from the card on every lookup is too slow. The cache is 2 KiB (four records), held in XMEM rather than the primary address space.

### 5.6 Flush: the commit order

`kres_flush` (and `kres_close`) make every pending change in a mutable container durable in this order:

1. Write all pending resource data, and sync.
2. Write the cached catalog into the **inactive** area, and sync.
3. Write the header: `active_area` switched, `generation` incremented, `resource_count` and `next_record` updated; sync.

An interruption before step 3 completes leaves the header naming the previous area, whose catalog is intact. Records allocated since the last flush are then merely unreferenced, and compaction reclaims them. Metadata changes are coalesced to flush or close; nothing rewrites the catalog per byte written.

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

/* Sealed containers (§8). */
int kres_build_begin(const char *path, uint8_t entry_count);
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

- **One shared buffer:** the core uses a single shared I/O buffer, 512 bytes, optionally 1–2 KiB. It never keeps a buffer per logical resource.
- **16-bit positions:** the KonaOS edition works in record numbers (16 bits, the 65,535-record limit) plus a 9-bit offset within a record. 32-bit arithmetic is confined to the edges: the `used_bytes` field, `kres_seek` and `kres_resize`. On SDCC every 32-bit operation costs several times the code and time of a 16-bit one.
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
Before: [HDR][CAT A][CAT B][A][dead][B][dead][C][unreferenced tail]
After:  [HDR][CAT A][CAT B][A][B][C]
```

Policies:

- **Preserve reservations (default):** copy each live resource's `allocated_records`, keeping editor growth headroom.
- **Tight pack:** copy only `ceil(used_bytes / 512)` records (at least one), releasing reserved slack as well as abandoned extents.
- **Seal:** write a sealed container (§8) from the live resources.

### 7.1 Native compaction: rebuild and rename

KonaOS serves two open files, so the native compactor rebuilds the container in a temporary file and then swaps names, the pattern KonaEdit's save uses:

1. Create a temporary container.
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

- **Layout:** the header, one catalog area (records 1–4), then the resources **one after the other** from record 5, each starting on the next record after the previous one ends (`allocated_records = ceil(used_bytes / 512)`, at least one). The entries mean exactly what they mean in a mutable container, so there is one reader and every read is whole records. The padding costs at most 511 bytes per resource.
- **No commit machinery:** no area B and no `generation` (zero); `next_record` marks the end of the last resource. The header's `SEALED` flag makes the library open the container read-only and refuse every mutation.
- **Building:**
  - `kres_build_begin(path, entry_count)` creates the file, with the catalog reserved at the front;
  - for each resource, `kres_build_add(name)` starts it and `kres_build_write` streams its bytes. Writes are sequential on one handle, and the catalog accumulates in memory;
  - `kres_build_end()` seeks back once, writes the catalog, then writes the header last, its magic being the final bytes written.

  An interrupted build leaves a file without a valid header, which every reader refuses. No crash-safety machinery is needed: either a complete sealed container exists, or an invalid file does. Adding more than `entry_count` resources is an error.
- **Sealing a mutable container:** `kres seal` (host) or `kres_compact` with `KRES_SEAL` (native) builds a sealed container from a mutable one's live resources. That reuses the rebuild copy loop (§7.1), so a project is edited mutable and shipped sealed. A sealed container is never appended to: it is rebuilt, or compacted back to mutable.
- **Library size:** the read path, the mutation and flush machinery, and the builder are separate archive members. A program that only reads sealed containers links none of the catalog-commit, allocation or compaction code.

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

- **Name encoding:** the tools treat names as CP850 and convert at the host boundary only; payloads are byte for byte, with no newline or encoding conversion.
- **Unsafe names:** export rejects absolute paths, drive prefixes, `..` components and other unsafe names.
- **Collisions:** export refuses name collisions on case-insensitive host filesystems instead of overwriting.
- **Git:** the default export policy covers sources and selected project metadata, not transient IR/REL/diagnostic resources unless requested. Git access stays outside `libkres`: users export to a directory and use ordinary Git tools.

## 10. Failure model and validation

- **Metadata:** crash-safe. An interrupted flush leaves the previous committed catalog (§5.6).
- **In-place edit within capacity:** a reset mid-write can leave that resource's bytes partially updated. Its metadata stays consistent.
- **Growth:** the new extent is written before the catalog names it. An interruption leaves the old entry and the old bytes intact, plus unreferenced records.
- **Compaction:** the rebuild is crash-safe (§7.1).
- **Sealed build:** an interrupted build leaves an invalid file, never a half-valid container (§8).

`kres_verify` checks:
- the magic, the version, `record_size`, `catalog_records` and the `SEALED` flag;
- that `active_area` and `resource_count` fit the catalog;
- every name's length and uniqueness;
- every extent's geometry against `next_record` and the actual file size;
- integer overflow;
- that no live extent overlaps another, the header, or the catalog areas.

It fails closed on a malformed file, and never follows an unchecked offset or trusts a declared size.

Checksums and journaling may come later, only if evidence justifies them.

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
| Catalog full | The 33rd create refused |
| Commit order | An interruption at each flush step leaves the previous catalog |
| Buffered I/O | Reads and writes across records and the partial final record |
| Fault injection | Short read/write, failed seek/sync, interrupted copy |
| Verify | Out-of-range, overlapping, duplicate and malformed entries refused |
| Compaction | Names and data preserved; capacities per policy; rebuild survives interruption |
| Sealed | Built and sealed containers read back byte for byte; mutations refused; an interrupted build refused at open |
| Export/import | Byte-for-byte round trips, CP850 names, safe paths |
| Cross-platform | Host-made containers read on the Z80, and the reverse |
| Pipeline | Editor → M2C → Z80GEN → KLINK, one open container per process |

Use randomized mutation sequences checked against an in-memory oracle, and deterministic corruption fixtures. On the target, measure code size, buffer use and file I/O calls as well as correctness.

## 12. Delivery sequence

**R0 — Freeze the format.** Header, catalog areas and entry encoding, name rules, limits, binary fixtures.

**R1 — Portable core and host tests.** Open/create/list/read/write, capacity, relocation, commit order, verify; instrument one-handle behavior.

**R2 — Compaction, sealed containers and host utilities.** Rebuild compaction, the builder and sealing, verify, import/export, round trips.

**R3 — KonaOS adapter and `truncate`.** The SDK `truncate(handle, length)` over the provider's FS2 truncate; the XMEM catalog cache; bounded buffering; native acceptance.

**R4 — EDIT.EXE integration.** Open and save named resources in a `.KPR` alongside ordinary files; reopen around child runs. KonaEdit has little room left, so this needs a lean subset (open, read whole, write whole, flush) or far code.

**R5 — KonaM2 integration.** Source → IR → REL → linked EXE as resource stages, sharing the library without container-layout logic in the compiler modules.

## 13. Open decisions

1. Default initial capacities and growth minimums for source and binary resources (policy, not format).

---

**Architectural rule:** KRES is a **flat, contiguous-record, mutable resource archive**, not a miniature filesystem. Its value is moving richer storage policy into reusable **userland** while KonaOS stays a small byte-stream provider.
