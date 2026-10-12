# ZephyrOS ROMFS v1 Recovery Filesystem Format

**Status:** implemented by work package `11-m5-rom-recovery-filesystem`
(2026-10-03).  Host-validated and physically accepted: both Zephyr-80
smoke transcripts (success and corrupt) are recorded in
`docs/validation/2026-10-03-m5-romfs.md`.  Wrapped by work package
`25-m9-vfs-bootstrap-romfs` (2026-10-04, corrections 01-02): ROMFS is now
a statically linked, path-based VFS provider mounted at `/system`
(`docs/vfs.md`), it has exactly one lookup and that lookup folds case
(the case-sensitive `romfs_lookup` and the second folded variant were
merged), and `romfs_load_zmod()` is gone.  The on-ROM format, the
ROM-access primitive and the module-facing resource imports are
unchanged.

ROMFS is the tiny read-only recovery store that boots the SIO driver (and
later native recovery programs) out of the programmed ROM image.  It is
not the general ZephyrOS filesystem: there is no heap, no allocator, no
write support and no directories, and it is not a general-purpose
filesystem API -- since M9 it is the bootstrap *provider* behind the
kernel VFS, which owns path resolution, mount policy and the native
file/directory API.  The M4 module loader is unchanged; only the byte
source became a bounded read source, then a VFS-bound one.

## 1. Physical placement

The programmed image is eight 64-KiB ROM pages (512 KiB):

```text
page 0    boot page   copied to SRAM bank 0 by the reset copier
page 1    boot page   copied to SRAM bank 7 by the reset copier
pages 2-7 recovery    ROMFS logical stream windows
```

ROM is never permanently mapped at run time: runtime modes `10`/`11`
select no ROM at all, and the only mode that reads ROM is mode `00`, in
which **every** read (including instruction fetch) comes from the
selected page while every write targets the selected SRAM bank.  A
ROM-access transition is therefore a short stackless window entered
through a trampoline `OUT (00h),A` and left through a second `OUT` inside
a stub that is byte-identical in common SRAM and in every recovery page
(`EF00h-EF07h`, 8 bytes, mirrored by `tools/pack_image.py`).  Each
recovery page provides a **48-KiB logical window**:

```text
recovery pages    6
window size       0xC000  (48 KiB)
capacity          0x48000 (288 KiB)
```

Logical byte `L` of the stream lives at offset `L % 0xC000` of page
`2 + L / 0xC000`.  The `C000h-FFFFh` half of every recovery page, and
every unused byte of a window, is programmed as `FFh` — except the fixed
ROM-access stub mirror at `EF00h-EF07h`; no logical mapping can ever
address that half.  Pages 0/1 are never modified by ROMFS placement
(`tools/pack_image.py` enforces this; the acceptance tests compare the
boot pages with and without a recovery stream).

The geometry is single-sourced in `platform/zephyr80/private/memory.h` /
`memory.inc` (`ZOS_ROM_*`, `ZOS_ROMFS_*`); the host tools and mapcheck
read those declarations, and `tests/test_romfs.py` pins the values.

## 2. Logical layout

The logical stream is little-endian and contains, in order:

```text
16-byte header
file_count x 36-byte entries   (directory)
payloads                       (manifest order, no alignment gaps)
```

### 2.1 Header (16 bytes)

| Offset | Size | Field | Value |
| ---: | ---: | --- | --- |
| `00h` | 4 | `magic[4]` | `"ZRFS"` |
| `04h` | 1 | `version` | `1` |
| `05h` | 1 | `entry_size` | `36` |
| `06h` | 2 | `file_count` | number of entries |
| `08h` | 2 | `directory_size` | `file_count x 36` |
| `0Ah` | 2 | `reserved` | `0` |
| `0Ch` | 4 | `image_size` | total stream bytes |

### 2.2 Directory entry (36 bytes)

| Offset | Size | Field |
| ---: | ---: | --- |
| `00h` | 24 | `name[24]`: NUL-terminated, zero-padded, printable ASCII |
| `18h` | 4 | `logical_offset` of the payload |
| `1Ch` | 4 | `size` in bytes |
| `20h` | 2 | `flags` |
| `22h` | 2 | `reserved` (`0`) |

Names are stored exactly as written (native ZephyrOS names, not CP/M 8.3).  A name
must be 1..23 bytes of printable ASCII (`20h-7Eh`) followed by at least
one NUL byte; all padding bytes are zero.  Duplicate names are rejected
by the host tool.  A host tool must reject two names that differ only in
ASCII case: one ROMFS lookup serves the whole kernel and it matches
case-insensitively (architecture section 32.13), so such a pair would be
ambiguous.

Flags/media types: `ZMOD = 1`, `ZEX = 2`, `DATA = 3`.  Any other value is
rejected by both the tool and the kernel.

Payloads are packed in manifest order immediately after the complete
table, with no gaps or alignment padding.  `logical_offset` must be at
least `16 + directory_size` (table/payload overlap is rejected) and
`offset + size` must not exceed `image_size` in 32-bit arithmetic.

## 3. Manifest and host tooling

`tools/romfspack.py` builds, validates, and inspects the stream:

```sh
python3 tools/romfspack.py pack <manifest.json> <recovery.romfs> [--objdir DIR]
python3 tools/romfspack.py validate <recovery.romfs>
python3 tools/romfspack.py dump <recovery.romfs>
```

The manifest is JSON:

```json
{
  "format": "romfs-v1",
  "files": [
    {"name": "sio.zmod", "type": "ZMOD", "path": "sio.zmod"}
  ]
}
```

Input paths resolve against `--objdir` (default: the manifest directory).
The production manifest is `recovery/manifest.json`, which carries the
boot SIO module (`sio.zmod`) and the native monitor (`monitor.zex`) in
every image; the M7 acceptance image uses `tests/m7/manifest.json` with
the same two entries, and the M6 acceptance images use
`tests/m6/manifest.json` / `manifest-failure.json` (zextest plus the
malformed-ZEX fixture matrix).  The M5 acceptance image uses
`tests/m5/manifest.json`, which adds the deterministic cross-page
pattern asset `m5-crosspage.bin` (byte `i` is
`(i & 0xFF) ^ ((i >> 8) & 0xFF)`; it is 0xC040 bytes and spans the
48-KiB window boundary).

`pack` produces deterministic output and prints a human-readable map with
logical range, physical ROM page/range, type, size, and SHA-256 per file:

```text
  sio.zmod                ZMOD       457  00000034h-000001FCh  page 2 0034h-01FCh
                          sha256 2543e08e...d36dd5
```

`pack`/`validate` reject, deterministically: duplicate, empty, overlong,
non-printable or unterminated names; names containing `/`; names that
collide case-insensitively (M9 resolves ROMFS names case-insensitively);
unsupported flags; missing inputs; nonzero reserved fields;
table/payload overlap and overlapping payloads; 32-bit wrap; truncated
or padded images; capacity excess; mismatched `directory_size`; and any
logical mapping into a page's `C000h-FFFFh` half (structurally
impossible within capacity, re-checked defensively).

`tools/pack_image.py` consumes `build/recovery.romfs` (produced from the
selected manifest), places it across pages 2-7, fills every unused byte
with `FFh`, rejects a stream shorter than a header or larger than the
capacity, and emits per-page hashes plus the ROMFS placement in
`zephyros*.manifest.json`.

## 4. Kernel interface

`formats/include/romfs.h` defines the small allocation-free API:

```c
int romfs_init(void);
int romfs_lookup_ci(const char *name, romfs_file_t *out);
int romfs_read(const romfs_file_t *file, uint32_t offset, void *dst,
               uint16_t length);
int romfs_entry_at(uint16_t index, romfs_file_t *out, char *name,
                   uint16_t name_size);   /* ROMFS-boot builds only */
```

* `romfs_init()` reads the header and the complete directory through the
  ROM-access primitive and validates everything before publishing the
  filesystem.  A malformed image is rejected as a whole: no lookup or
  read can ever be driven by unvalidated metadata, and a corrupt image
  never drives a latch value or an executable address.
* `romfs_lookup_ci()` is **the** ROMFS lookup: it scans entries in order
  and compares names with ASCII case folding (architecture section 32.13).
  There is no case-sensitive variant beside it; VFS, the ZEX launcher and
  the module resource imports all arrive here.  It returns `-ZOS_ENOENT`
  for absent names and `-ZOS_EINVAL` for an over-long component.
* `romfs_read()` is strictly bounded: `offset <= size` and
  `length <= size - offset` are enforced in 32-bit arithmetic before
  anything is narrowed.  `offset == size` with `length == 0` is a clean
  EOF read (returns 0); any request that would read past EOF is rejected
  with `-ZOS_EINVAL`.  Reads may cross 48-KiB window boundaries; they are
  split internally at window edges and at the 128-byte staging size.
* `romfs_entry_at()` returns the entry at a validated index with its
  name, so the VFS provider can enumerate the directory without
  duplicating layout knowledge.  It is compiled only into ROMFS-boot
  builds; `tools/mapcheck.py` rejects it in an embedded-catalog image.

M9 removed `romfs_load_zmod()` and the case-sensitive `romfs_lookup`;
the file is no longer handed to the loader by the ROMFS reader.
`kernel/romfs/` is now a metadata/byte reader only, and the filesystem
path goes through the M9 VFS and the `fs/romfs` provider
(`docs/vfs.md`).

`kernel/romfs/romfs.c` avoids 32-bit division (window index by repeated
subtraction) and 32-bit multiplication (`count x 36` by shifts), so the
freestanding build needs no runtime library.  All state is bank-7 runtime
data; none of it is read while ROM is visible.

## 5. ROM-access primitive

`kernel/romfs/rom_access.asm` is the only code that changes the memory
mapping for ROMFS.  Its fixed transition block is linked in the
permanent common region (`_COMMON` at `EEFEh-EF07h`), as are its staging
buffer and saved transition state (`_COMMON_DATA`; sizes are declared in
`platform/zephyr80/private/memory.h/.inc` and enforced by `tools/mapcheck.py`).
There is deliberately no transition stack: the mode-00 window is
stackless by construction.

Its contract:

```text
uint8_t rom_access_copy(const struct rom_access_req *req);   /* A = 0/err */
```

1. read and validate the request: page in `2..7`, length in `1..128`,
   `offset + length <= 0xC000` with no 16-bit wrap;
2. refuse reentry (`ZOS_EBUSY`) before touching any state;
3. lock interrupts, capturing the exact IFF2 state (`ld a,i` then `di`);
4. save the exact caller latch byte;
5. arm the stub registers: `HL` = window source, `DE` = staging
   destination, `BC` = length, `A` = mode-00 latch (`page << 5`, bank 0),
   `A'` = exact caller latch;
6. jump to the trampoline at `EEFEh`: `OUT (00h),A` selects mode 00, and
   the next instruction fetch comes from the mirrored stub in the
   selected ROM page;
7. the stub (`EF00h`): `LDIR` copies ROM -> SRAM bank 0 staging, then
   `EX AF,AF'` / `OUT (00h),A` restores the exact caller latch before any
   stack access could happen;
8. a common epilogue clears the busy flag, restores the exact IFF state,
   and returns `0`.

Consequences enforced by tests:

* between the two `OUT`s the CPU executes only the eight mirrored stub
  bytes, performs no stack access, and reads no SRAM variable (every
  input travels in registers); the linked-byte simulation pins every
  mode-00 step to the stub extent and rejects any SP movement;
* the stub bytes in every recovery page match the linked common copy
  byte-for-byte (checked by the packer pattern/mirror rules and the
  artifact tests);
* the routine is non-reentrant; a busy flag (initialized to 0 by a `.db`,
  because `.ds` bytes are `FFh` in the ROM image) rejects nested calls;
* invalid page/offset/length combinations cannot change the mapping,
  cannot read outside the configured recovery pages, and cannot modify
  the staging buffer;
* the exact latch byte (including the SRAM bank field) and the exact IFF
  state are restored, and the caller stack is untouched.

The staging destination works because the mode-00 latch selects SRAM
bank 0: mode-00 writes land in bank 0 across the whole 64-KiB space, so
the common region stays writable while reads come from ROM.  Bank 7 is
never addressable during the copy, and no byte of `2000h-DFFFh` is
executed, returned into, or stacked on.

## 6. Reader integration

The reader is **fixed core** (package 38) and serves two callers, both
compiled against `formats/include/romfs.h`:

* `kernel/vfs/system.c` - the `/system` provider: `romfs_lookup_ci()`,
  `romfs_entry_at()` and `romfs_copy_logical()` behind an in-process
  `FILE` endpoint, so a bound `/system` handle is served without a bank-6
  driver;
* `kernel/driver/driver_loader.c` - the resident loader: it reads
  `drivers.man`, then each container's header, tables and segment images
  through `romfs_read()`, staging every copy in chunks through the common
  application-bank copy gate.  There is no abstract byte source any more:
  the arena loader that needed one is deleted (work packages 42b/42c), and
  a driver image is the only container form.

The reader's own access primitive is unchanged: every read goes through
the mode-00 ROM window (`kernel/romfs/rom_access.asm`), which restores the
exact latch, interrupt state and stack of its caller, so a corrupt
recovery image fails through the early console before any driver code
runs:

```text
vfs_init()                   fail -> romfs_diagnose() +
                                    panic("romfs: corrupt recovery image")
driver_load_all("drivers.man")
                             fail -> "driver loader: <name>: <what>"
```

`tools/mapcheck.py --production` rejects the retired embedded-catalog
symbols (`_z4_module_boot`, `_zmod_catalog`, `_module_catalog_load_all`):
production carries no boot blob, only the packed recovery store.

## 7. Acceptance evidence map

| Property | Evidence |
| --- | --- |
| deterministic packing, map, SHA-256 | `tests/test_romfs.py` (`pack`/`validate`/`dump`, reproducibility, rejections) |
| placement in pages 2-7, boot pages untouched, capacity/window rules | `tests/test_romfs.py`, `tests/test_pack_image.py`, `tests/test_validation_cli.py` |
| valid kernel reader, EOF/bound semantics, corrupt-metadata rejection | `tests/test_romfs_sim.py` (mutated header/table replays) |
| exact latch/IFF restoration, stackless mode-00 window pinned to the mirrored stub, reentry and bounds rejection | `tests/test_romfs_sim.py` (ROM-access primitive replays + `RomWindowWatcher`) |
| source-backed loading, reject-before-allocate, byte-identity with `sio.zmod` | `tests/test_romfs_sim.py`, `tests/test_romfs.py` |
| ROMFS as the `/system` root (in-process `FILE` endpoint, nucleus): one case-folded lookup, `/` the only directory, read-only semantics, `ZOS_ENOTDIR`/`ZOS_ENOENT` for deeper paths, `ZOS_EROFS` for writes | `tests/test_fs_selftest_sim.py` (`make fs-selftest`: the `/system` stat/open/read/close outcomes) |
| the single case-folded lookup resolves every spelling | `tests/test_romfs_sim.py` (case variants of `sio.zmod`) |
| production has no embedded boot blob/catalog | `tools/mapcheck.py --production` |
| corrupt image panics before module execution | `tests/test_romfs_sim.py`, image marker tests |
| physical success and corrupt transcripts | `docs/validation/2026-10-03-m5-romfs.md` (**pending hardware**) |
