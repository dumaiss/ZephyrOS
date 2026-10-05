# ZephyrOS VFS Bootstrap and the ROMFS Provider

**Status:** package `25-m9-vfs-bootstrap-romfs` (2026-10-04, corrections
01 and 02) implemented the VFS core, the generated mount table and the
ROMFS provider; package `26-m9-provider-abi-cache` (2026-10-04) completed
the kernel file/directory API (32-bit seek, root merge, mountpoint
errors), added driver ABI v3 `fs_register` with the loader registration
transaction, and proved it with the test-only writable `ramfs` provider;
package `27-m9-fs-syscalls` (2026-10-05) exposed the VFS to native
programs: the global owner-tagged descriptor pool, per-process CWD, the
pinned `FS_*` syscall family (`10h-1Fh`), the user runtime wrappers and
the `fstest.zex` acceptance program.  Host-validated, simulator-accepted
and physically accepted 2026-10-05 (`make smoke-hardware-m9-fs` observed
`fstest: PASS 9/9`, `fstest: launcher OK`, `m9 fs selftest: OK` on
hardware; see `docs/validation/2026-10-05-m9-fs-syscalls.md`).  Package
`28-m9-nested-exec` adds the `exec` case to `fstest.zex`; its acceptance
image was re-run on the bench the same day, where the extended suite printed
`fstest: PASS 10/10` and the target hook printed `m9 exec selftest: OK`
(`docs/validation/2026-10-05-m9-nested-exec.md`).  Bank-7 residency is reported against the architecture
section 32.39 ledger.

This document records the implementation of the M9 bootstrap slice:
the kernel VFS core, the generated static mount table, and the ROMFS
filesystem provider mounted at `/SYSTEM` before any module loads.
Architecture sections 32.1-32.14, 32.32 and 32.33 are the normative
description; `docs/filesystem.md` is the M9 filesystem design and
`work/M9-REFERENCE.md` pins the ABI values.  This document adds only the
implementation facts of package 25, plus what package 25 deliberately
leaves to later packages.

## 1. Scope of this package

Implemented here:

```text
vfs_init()            register the built-in provider set, mount /SYSTEM
module phase          boot ZMODs and the monitor load by VFS path
vfs_mount_all()       instantiate the remaining configured mounts
VFS_PHASE_READY       namespace fixed; kernel file/dir API available
```

Deliberately **not** implemented here (later packages):

| Deferred | Package |
| --- | --- |
| data cache and generation keys (provider-local: IOC-FAT) | 30 |
| writable providers, IOC-FAT at `/`, block class, ZDEV ABI v3 | 26, 29, 30 |
| `/` filesystem merge into root enumeration | 26 |
| cross-filesystem move/rename semantics | 31 |

Package 27 implemented the previously deferred rows "syscalls,
per-process descriptors, `SYS_FS_*`" and "current working directory
(`chdir`/`getcwd`)"; see section 12 below.

VFS is kernel mechanism: no executable code exists under `platform/` or
`config/`, and `fs/romfs` includes no driver header.

## 2. Interfaces

Three headers, with a deliberate visibility split:

| Header | Audience | Content |
| --- | --- | --- |
| `include/zephyros/fs.h` | native applications, syscalls, providers | `ZOS_PATH_MAX` (128), `ZFS_NAME_MAX` (64), `ZFS_TYPE_*`, `ZFS_ATTR_*`, `ZFS_TIME_UNKNOWN`, `struct zfs_stat` (16 bytes), `struct zfs_dirent` (80 bytes), `ZFS_O_*`, `ZFS_SEEK_*`.  There is no `zfs_statfs` in M9. |
| `include/zephyros/vfs.h` | module-facing provider ABI | `struct zfs_ops` (the 14 path-based operations of architecture 32.12, in order), `struct zfs_provider` |
| `kernel/vfs/vfs.h` | kernel only | lifecycle, registry, mount runtime state, path helpers, kernel file/dir API, the mutation entry points, `zmod_source` adapter, `VFS_O_EXEC` |

`include/zephyros/vfs.h` is module-facing and passes the package 22
module-header boundary checks; kernel-only declarations are never
reachable from a ZMOD.  All three layouts are pinned by
`_Static_assert` and by host tests (`tests/test_m9_vfs.py`, which also
checks the operation list and every signature against section 32.12).

Providers are **path-based**.  Handles (`fs`, `file`, `dir`) are opaque
to VFS and carry no object identity: VFS keeps no node objects, no node
lifetimes and no per-component traversal state, and there is no
`object_id` and no cache key.  Providers never learn their external
mountpoint: VFS passes a provider-relative path that always begins with
`/`.

Every path argument obeys one contract: absolute inside the provider
(`/` is its root), already normalized by VFS (no `.`, `..`, empty or
repeated components, no trailing `/`), NUL-terminated and at most
`ZOS_PATH_MAX - 1` bytes, and matched by the provider with ASCII
case-insensitive semantics.  VFS folds only the first component.

A read-only provider leaves the mutation pointers null (`write`, `sync`,
`mkdir`, `rmdir`, `unlink`, `rename`) and VFS turns a null mutation
operation into `ZOS_EROFS` in one place.

## 3. Source layout

```text
kernel/vfs/mount_config.h    generated-table build contract (kernel-private)
kernel/vfs/path.c            normalization + case-insensitive compare
kernel/vfs/mount.c           registry, runtime state, mount selection, phases
kernel/vfs/file.c            resolve-path, file API, zmod_source adapter
kernel/vfs/dir.c             directory API, root enumeration
kernel/vfs/mutate.c          mutation entry points (the EROFS rule's home)
fs/romfs/romfs_provider.c    ROMFS provider over the unchanged M5 reader
fs/romfs/romfs_provider.h    provider declaration used by the build table
tools/mountgen.py            manifest -> generated C mount table
config/mounts.json           production policy (only `/SYSTEM`)
tests/m9/mounts-*.json       acceptance policies
```

The provider registry has capacity 4 (`VFS_PROVIDER_MAX`) and the mount
table capacity is 4 (`VFS_MAX_MOUNTS`); the generator rejects a larger
manifest and VFS re-checks the capacity at run time.

## 4. Path rules and mount selection

`vfs_path_normalize()` accepts absolute and relative paths and writes an
absolute canonical path: repeated and trailing slashes are collapsed,
`.` is dropped, `..` pops the last component and traversal above `/` is
clamped to `/`.  Case is preserved (the provider folds names; the native
namespace preserves what the user typed).  `ZOS_PATH_MAX` and
`ZFS_NAME_MAX` include the terminating NUL, so the longest usable path is
127 bytes and the longest component is 63; both over-length forms and a
too-small output buffer return `ZOS_EINVAL`.  Paths resolve against the
`/` base for the kernel launcher; package 27 resolves a relative path
against the calling process' CWD instead.

`vfs_name_equal()` compares ASCII case-insensitively (`A-Z` == `a-z`,
every other byte exact).  ZephyrOS performs no Unicode folding.

Selection is **flat** (architecture section 32.8) and uses only `MOUNTED`
entries.  The first path component is compared case-insensitively with
the mounted root-level mountpoints (a configured path is `/` or one
component, section 32.6), so `/SYSTEMX` does **not** select `/SYSTEM`.
When no root-level mountpoint matches, the filesystem attached at `/`
owns the path.  With neither, the path is unowned and resolution reports
`ZOS_ENODEV`.

The result is the mount index plus the provider-relative path, which
always begins with `/` (a mountpoint itself resolves to `"/"`).  There is
no longest-match search and no per-component walk: `VFS` hands
`/SYSTEM/sio.zmod` to the provider as the single path `/sio.zmod`.

`vfs_is_mountpoint()` reports whether a normalized path is a configured,
mounted mountpoint (or `/` itself).  VFS answers `stat()` for those paths
itself, as directories, and rejects `open()` on them with `ZOS_EISDIR`.

VFS owns exactly one shared 128-byte pathname scratch buffer
(`kernel/vfs/mount.c`); `vfs_rename()` adds a second because it holds two
paths at once.  Nothing in VFS declares an automatic object large enough
to matter on the 256-byte gate stack (work/M9-REFERENCE.md gate-stack
discipline).

## 5. Lifecycle and boot integration

```text
kernel_main()
  irq_init()
  device_registry_init()
  module_loader_init()
  vfs_init()                       VFS_PHASE_BOOTSTRAP -> VFS_PHASE_MODULES
      register built-ins (romfs)
      find the unique VFS_MOUNT_SYSTEM entry (panic if not exactly one)
      provider lookup by name, ops->mount(source = "recovery")
  kernel_load_vfs_module("sio" | "ioc" | "hid" | "v9958")
      vfs_open("/SYSTEM/<name>.zmod", ZFS_O_READ)
      vfs_file_source() -> module_load_source()
  vfs_mount_all()                  VFS_PHASE_MOUNT -> VFS_PHASE_READY
  console_bind_defaults()
  process_exec("/SYSTEM/monitor.zex")
```

Boot order, module failure semantics and every console line are
unchanged from M8.  A failed system mount prints the existing
`romfs_diagnose()` lines and panics with
`romfs: corrupt recovery image`; the M5 corrupt-image transcript is
reproduced byte for byte.

Non-system mounts follow architecture 32.32.4: a failed optional mount
prints the pinned line and boot continues, a failed required mount prints
the same line and then panics (`PANIC: vfs: mount failed`).  Production
and the M8 images configure no non-system mount, so the whole pass is a
no-op there and prints nothing.

Both outcomes are proven from real linked bytes: `tests/m9/mounts-optional-fail.json`
in the `m9-vfs-selftest` image, and `tests/m9/mounts-required-fail.json`
in the `m9-required-mount-selftest` image (which keeps the production
32-granule gate, because the required failure happens before the monitor
launcher and no test hook is linked).

`ZOS_ROMFS_BOOT=0` images (pre-M5 acceptance) keep the embedded-catalog
path and link no VFS code at all: the object group is dropped from the
link (`M9_VFS_OBJS`), `kernel/process/exec.c` compiles its M6 ROMFS
body instead of the VFS body, `kernel/romfs/romfs.c` compiles the
M9-only accessors out, and `tools/mapcheck.py` rejects any VFS symbol in
those maps (`docs/validation/2026-10-04-m9-vfs-bootstrap.md` section 9).

## 6. Kernel file and directory API

`vfs_open()`/`vfs_read()`/`vfs_write()`/`vfs_seek()`/`vfs_sync()`/
`vfs_close()`/`vfs_stat()` operate on caller-provided `struct vfs_file`
storage; package 27 adds the process-visible pool (`vfs_fd_open`, section
12) on top of them.  Every one of them resolves the path, selects the
mount and calls the provider with the provider-relative path.  Reads are
clamped to EOF by VFS, so a provider only ever sees an in-range request.

`vfs_open()` rejects unknown flag bits, `TRUNC`/`CREATE`/`EXCL` without
`WRITE`, and `EXCL` without `CREATE` with `ZOS_EINVAL`; opening a
directory is `ZOS_EISDIR`, `opendir` on a file is `ZOS_ENOTDIR`, and a
create of an existing name with `CREATE|EXCL` is `ZOS_EEXIST`.  The
position is 0 on open.

`vfs_read()`/`vfs_write()` act at the stored position.  A write advances
the position and grows the cached size only after the provider reports
success (architecture section 32.28); a failed confirmed write leaves both
unchanged.  `vfs_seek()` takes a signed 32-bit `offset` with
`ZFS_SEEK_SET`/`CUR`/`END`, rejects a negative result with `ZOS_EINVAL`
and permits seeking past EOF.  `vfs_close()` flushes through the
provider's `sync` (when present) and then releases the handle.

`stat("/")` and `stat(path)` for a mounted mountpoint are answered by VFS
itself as directories, without consulting a provider.  There is no
`statfs` in M9.

`VFS_O_EXEC` (0x8000) is a kernel-only open hint, not a native flag: the
executable loader sets it, and a provider that distinguishes executable
media (ROMFS) refuses a non-`ZEX` entry opened with it.  That preserves
the M6 `ZEX_EXEC_ERR_TYPE` rejection now that the native type set is
only `FILE`/`DIR`.

`readdir("/")` is a VFS operation: it first yields every mounted
root-level mountpoint (exactly one component) as a `DIR` entry, then the
entries of the filesystem attached at `/`, skipping any whose name equals
a mountpoint (case-insensitive) so the mountpoint shadows it
(architecture section 32.7).  With no filesystem at `/` — the production
M9 namespace — the root lists only the mountpoints and still enumerates
successfully.  The merge cursor is a small VFS-owned field of the
`struct vfs_dir` handle; VFS keeps no extra table.

Mutation entry points (`vfs_mkdir`, `vfs_rmdir`, `vfs_unlink`,
`vfs_rename`) live in `kernel/vfs/mutate.c` and are the kernel half of the
`SYS_FS_*` family that package 27 wires up.  A provider that leaves a
mutation pointer null gets `ZOS_EROFS` from VFS, and a cross-mount rename
is refused with `ZOS_EXDEV` rather than turned into a copy.  Mutating a
mounted mountpoint (or the VFS root) is answered by VFS before any
provider is consulted: `ZOS_EBUSY` for `rmdir`/`unlink`/`rename`,
`ZOS_EEXIST` for `mkdir`; `rename` is the only operation that needs two
pathnames and owns the second 128-byte scratch buffer.

`vfs_file_source()` bounds a `zmod_source` to an open file so the M4
loader and ZEX validation read through the kernel file API without
knowing where the bytes live.  A file larger than 64 KiB is rejected
with `ZOS_ENOSPC` because the source interface is 16-bit.

## 7. ROMFS provider

`fs/romfs/romfs_provider.c` exposes the unchanged M5 reader
(`kernel/romfs/`) through the path-based `zfs_ops`; the on-ROM format, the
ROM-access primitive and the module-facing `romfs_resource_*` imports are
untouched.

```text
mount      source must be "recovery", else ZOS_EINVAL; calls romfs_init()
           (a corrupt image returns the reader's error unchanged) and
           resets the handle pool, so the instance never depends on the
           C runtime having zeroed _DATA
tree       `/` is the only directory; `/<name>` is a file
lookup     one ASCII case-folded name match (romfs_lookup_ci)
stat       ZFS_TYPE_FILE, ZFS_ATTR_READONLY, ZFS_TIME_UNKNOWN; `/` is a
           read-only directory
open       resolves `/<name>`; write flags are ZOS_EROFS before the path
           is even resolved, `/` is ZOS_EISDIR, a missing name is
           ZOS_ENOENT, VFS_O_EXEC on a non-ZEX entry is ZOS_EINVAL
deeper     `/<file>/x` is ZOS_ENOTDIR; `/<unknown>/x` is ZOS_ENOENT
readdir    entries in image order through the reader's indexed access;
           2 static handle slots, ZOS_EMFILE when exhausted
mutations  all null: VFS answers ZOS_EROFS
```

Everything the provider defines is either the exported `romfs_provider`
record or a `romfs_pv_*` private, so a build can be checked for "no ROMFS
provider symbol" with one prefix (`tools/mapcheck.py`).

The provider holds no on-ROM layout knowledge beyond what the reader
exposes, and no Zephyr-80 mount policy.  It contains no `object_id`, no
node handle and no `statfs`.

## 8. Static mount table

Mount policy lives in exactly one place: the JSON manifest selected by
the `ZOS_MOUNTS` Makefile variable.  VFS source contains no paths,
provider names or source strings.

```text
tools/mountgen.py <manifest> --builtin <names> \
    --out build/generated/vfs_mounts.c \
    --report build/generated/vfs_mounts.json
```

The generator validates the section 32.5.1 rules and the extra
rejections of the M9 reference (more than 4 entries; a mount path that
is not upper-case ASCII or has a trailing slash; a `system` entry whose
filesystem is not linked in; `required` on the system entry), emits
entries in manifest order with `VFS_MOUNT_SYSTEM 0x01`,
`VFS_MOUNT_REQUIRED 0x02`, `VFS_MOUNT_OPTIONAL 0x00`, and is
deterministic (no timestamps, no host paths) so `make check`
reproducibility holds.  The output lives under `build/` and is never
committed.

```c
const struct vfs_mount_config vfs_mount_table[] = {
    { "/SYSTEM", 7u, "romfs", "recovery", VFS_MOUNT_SYSTEM },
};
const uint8_t vfs_mount_count = 1u;
```

The table links into the ROM-loaded bank-7 kernel image as `const`
data; VFS keeps the parallel `_DATA` array `vfs_mount_state[4]` (state,
provider, `fs_state`, last error) and never writes the configuration.
There is no generation counter: VFS holds no object identity and no cache
key (architecture section 32.12).  `ZOS_VFS_BUILTIN` names the providers linked into the
kernel and must match the kernel's built-in registration table; a host
test keeps the two in sync.

`tools/romfspack.py` additionally rejects names containing `/` and
names that collide case-insensitively, because ROMFS lookup is now
case-insensitive.

## 9. Budget, per-object sizes and geometry

Bank-7 residency is reported against the architecture section 32.39
ledger.  Production after correction-02:

```text
bank7: code 23490 end 7BC5h | arena 34 granules (21 boot, 13 free)
       | data 1294 at C000h | isr 256 | slack 14362
```

These are the package-25 numbers.  Package 25b then added the
size-optimized compile and the derived data base and moved the runtime
data above `C000h`; package 26 added driver ABI v3, the provider
registration transaction, the completed kernel file API (seek, root
merge, mountpoint errors) for `+1837` `_CODE` and `+192` `_DATA` bytes.
`docs/memory.md` section 2.2.1 records the current ledger
(`code 23526 end 7BE6h | arena 46 granules (20 boot, 26 free) | data 1501
at D800h | slack 291`) with the per-object package-26 deltas.

The arena floor is the unchanged 32-granule production gate; correction-02
raised production from exactly 32 granules to 34 by removing the
superseded machinery.

### 9.1 Per-object bank-7 sizes

Measured by linking each object alone (SDCC emits the whole `_CODE` area
for the object, so the figures are exact, not spans):

| Object | `_CODE` | `_DATA` |
| --- | --- | --- |
| `kernel/vfs/path.c` | 826 | 0 |
| `kernel/vfs/mount.c` | 1975 | 170 |
| `kernel/vfs/file.c` | 1549 | 0 |
| `kernel/vfs/dir.c` | 991 | 0 |
| `kernel/vfs/mutate.c` | 525 | 128 |
| `fs/romfs/romfs_provider.c` | 1644 | 50 |
| `kernel/romfs/romfs.c` | 3119 | 7 |
| generated mount table | 32 | 0 |
| **total** | **10661** | **355** |

### 9.2 What correction-02 removed and added

Removed outright (the numbers are the correction contract's own
measurements of the superseded functions):

```text
vfs_lookup_rel            544   per-component node walk
vfs_release_node          512   node lifetime
vfs_resolve_node          115   node-producing resolver
vfs_statfs                284   statfs entry point
romfs_lookup              312   the case-sensitive duplicate lookup
romfs_lookup_ci (old)     395   the folded lookup with an index out-param
                         ----
                         2162   deleted
```

Added or enlarged by the required rework (measured above):

```text
vfs_write / vfs_sync                      264   write paths (EROFS/consumer)
vfs_mkdir/rmdir/unlink/rename             ~390  the null-mutation EROFS rule
vfs_mount_is_root_level/is_root/
  is_mountpoint/first_component_equal    ~245  flat selection
vfs_resolve_path + vfs_mount_resolve     ~1013 path resolution + selection
ROMFS provider path handling,
  handle pool and deeper-path rule       ~1644 (whole object, incl. ops table)
romfs_lookup_ci (single, folded)          312  one lookup instead of two
```

Net effect on the linked image: `_CODE` 24291 -> 23490 bytes (**-801**)
and `_DATA` 1210 -> 1294 (+84: the provider's 26-byte handle pool, the
64-byte component buffer and `vfs_rename_target`).  That is below the
2-3 KB the correction projected, because the rework also *added* the
flat-selection helpers and the mutation entry points that the superseded
design either already had in another form or did not have at all.

### 9.3 Arena geometries (package-25 snapshot)

The geometries below predate package 25b's size-optimized compile and
derived data base; the current production row is recorded in
`docs/memory.md` section 2.2.1.

| Image | Kernel end | Arena | Granules |
| --- | --- | --- | --- |
| production | `7BC5h` | `7C00h-C000h` | 34 |
| m8-selftest | `8871h` | `8A00h-C000h` | 27 |
| m9-vfs-selftest | `86E2h` | `8800h-C000h` | 28 |
| m8-v9958-selftest | `86CBh` | `8800h-C000h` | 28 |
| m8-console-selftest | `849Dh` | `8600h-C000h` | 29 |
| m8-hid-selftest | `83C4h` | `8400h-C000h` | 30 |
| m8-fallback variants | `82A7h` | `8400h-C000h` | 30 |
| m9-required-mount-selftest | `7BEEh` | `7C00h-C000h` | 34 |

Acceptance and variant images that link a target test hook check 16
granules as their *minimum* (`ZOS_ARENA_MIN_GRANULES`), because a test
hook cannot hold 32 granules on top of a 21-granule boot set.  Their
safety net is unchanged: `tools/zmodpack.py plan` must fit the whole boot
set into the derived arena and `tools/pack_image.py` reconciles the
manifest against it, so an image whose modules do not fit still fails the
build.  Production keeps the 32-granule gate and now clears it by two
granules.

### 9.4 Embedded-catalog images (`ZOS_ROMFS_BOOT=0`)

These images carry no VFS object at all (correction-01), so their `_CODE`
end is set purely by the M6/M5 code they always had.  Measured against
the pre-package-25 baseline `3dedd09`:

| Image | `_CODE` end | Baseline | Delta | Fixed arena |
| --- | --- | --- | --- | --- |
| m1 (`build-selftest`) | `5BE0h` | `5CA4h` | -196 | legacy |
| irq (`build-irq-selftest`) | `60F6h` | `61BAh` | -196 | legacy |
| m3-selftest | `6AC3h` | `6B87h` | -196 | `A000h-BFFFh` |
| m4-loader-selftest | `6E9Bh` | `6F5Fh` | -196 | `A000h-BFFFh` |
| m4-selftest | `701Dh` | `70E1h` | -196 | `A000h-BFFFh` |
| m4-placement-selftest | `6E9Bh` | `6F5Fh` | -196 | `9E00h-BDFFh` |
| m4-failure | `5B82h` | `5C46h` | -196 | `A000h-BFFFh` |

(The measurements are from correction-01; package 25a retires these
images.)

## 10. Acceptance evidence

| Property | Evidence |
| --- | --- |
| ABI sizes and layouts | `tests/test_m9_vfs.py` (`_Static_assert` mirrors, header boundary) |
| the provider ABI is exactly the 14 path-based operations of section 32.12, in order, with no node/`object_id`/`statfs`/`unmount` remnant | `tests/test_m9_vfs.py` (`TestProviderAbiShape`: operation list, signatures, absence checks, `ZFS_OPS_COUNT`) |
| generator rules, determinism, every rejection | `tests/test_m9_vfs.py` (`test_mountgen.py` cases), all `tests/m9` manifests |
| built-in list == `ZOS_VFS_BUILTIN` | `tests/test_m9_vfs.py` |
| `/SYSTEM` mounted before any module loads; every boot ZMOD and the monitor loaded by VFS path | `tests/test_m9_vfs_sim.py` (real linked bytes, boot transcript + monitor handoff) |
| flat mount selection: `/SYSTEMX` must not match `/SYSTEM` | `tests/test_m9_vfs_sim.py` (`test_flat_mount_selection_in_the_linked_kernel`, direct kernel-API call on linked bytes) + the target selftest's FLAT case |
| normalization, case-insensitive resolution, per-entry stat/read byte-compared with the packed files, `/SYSTEM` directory enumeration, `stat("/")`, `stat("/SYSTEM")`, ENOENT, ENOTDIR below a file, deeper-path ENOENT, `VFS_O_EXEC` gate | `tests/test_m9_vfs_sim.py` + `tests/target/m9/m9_vfs_test.c` (`m9-vfs-selftest`) |
| EROFS from the null mutation operations (write flags at open, `vfs_mkdir`/`rmdir`/`unlink`/`rename`, and the provider's null pointers) | target selftest EROFS/MUTATE cases; `kernel/vfs/{file,mutate}.c` |
| one ROMFS lookup, ASCII case-folded, serving the provider and the resource imports | `tests/test_romfs_sim.py` (case-folded spellings), `tools/mapcheck.py` `ROMFS_BANK7_ROUTINES` |
| optional-mount failure line `vfs: /DATA unavailable 0x03` and tolerant boot | `tests/test_m9_vfs_sim.py` (`tests/m9/mounts-optional-fail.json`, `m9-vfs-selftest`) |
| required non-system mount failure: `vfs: required mount /DATA failed 0x03` + `PANIC:` of the same line before the monitor, system entry still `MOUNTED`, failed entry records `-ZOS_ENODEV` | `tests/test_m9_vfs_sim.py` (`tests/m9/mounts-required-fail.json`, `make m9-required-mount-selftest`) |
| ZEX/ZMOD loading through VFS, M8 transcripts unchanged | `tests/test_m3_sim.py` (production boot replay), `tests/test_m8*.py`, `tests/test_romfs*.py` |
| ROMFS name validation | `tests/test_romfs.py` (slash and case-collision negatives) |
| production arena gate, `ZOS_ARENA_MIN_GRANULES` | `tools/mapcheck.py`, `tools/pack_image.py`, `make check` |
| embedded-catalog images link no VFS byte | `tools/mapcheck.py` (`vfs_image` classification + `VFS_FORBIDDEN_PREFIXES`), `tests/test_romfs.py` (`TestRomfsMapcheck.test_embedded_catalog_image_rejects_vfs_symbols`), `tests/test_m4.py` (per-image symbol sweep), `tests/test_m9_vfs.py` |
| physical boot | `docs/validation/2026-10-04-m9-vfs-bootstrap.md` (**PENDING**) |

## 11. Writable-provider acceptance (package 26)

Driver ABI v3 adds `fs_register(provider)` to the `zdriver_api` block
(`include/zephyros/driver.h`), so a loadable module can register a
path-based `struct zfs_provider`.  The loader wraps it: `fs_register`
succeeds only during the module-loading phase, and the VFS provider
registry takes a checkpoint before every module init and rolls back the
providers registered after it when the init fails, exactly like the
device and IRQ checkpoints.  The registry has capacity 4; registration
rejects a null/malformed provider (`ZOS_EINVAL`), a duplicate name
compared case-insensitively (`ZOS_EEXIST`) and a full registry
(`ZOS_ENOSPC`).

The test-only module `tests/modules/ramfs.c` (packed as `ramfs.zmod`,
driver ABI v3) registers the `ramfs` provider.  It is path-based and
hierarchical, matches ASCII case-insensitively, and is bounded by 16
nodes and 2048 bytes of file data per instance over two instances.  Its
documented reserved control names are:

```text
/FAILWRITE   the next write fails with ZOS_EIO and commits nothing
/STALE       creating it invalidates every open handle (ZOS_ESTALE)
/STATS       virtual read-only file exposing the call counters and the
             open-handle count
source "refuse"  makes the instance's mount() fail with ZOS_EIO
```

The acceptance image (`make m9-provider-selftest`) boots SIO-only so the
derived arena holds the bounded RAM provider and the registration
fixtures `m9.probe`, `m9.dup` (duplicate rejection) and `m9.fail`
(rollback).  `tests/m9/mounts-ramfs.json` mounts ramfs at `/` (required)
and `/R2` (optional); `mounts-ramfs-optional-fail.json` and
`mounts-ramfs-required-fail.json` are the failure variants.  The
mount-table generator accepts `/` alongside a root-level mountpoint
because the M9 model treats them as siblings, not as nested mounts.

Evidence: `tests/test_m9_provider_sim.py` (boot transcript, result byte,
mount state), `tests/target/m9/m9_provider_test.c` (registration/
rollback, mount state, mutations and errors, root merge and shadowing,
the section-33 size set, 32-bit seek past 64 KiB, stale handles, a failed
confirmed write, EXDEV, counters) and `tests/test_m9_vfs.py` (generator
acceptance of the ramfs manifests).

### Physical acceptance

The production image (`build/zephyros.bin`, sha256
`03f776a9...95cec`) boots to the monitor with HID and V9958 bound; the
run, transcript and photograph are recorded in
`docs/validation/2026-10-05-m9-provider-abi-hardware.md`.  Note that
`tools/serial_smoke.py --mode normal` requires the console acceptance
line and monitor prompt on the serial transcript, so it reports a
timeout whenever the console-capable module is resident (architecture
30.9 binding policy); that mode is only applicable to an SIO-fallback
bench.  The M8 serial smoke modes were re-run on the ABI v3 module bytes
and pass.

## 12. Native filesystem syscalls (package 27)

Package 27 makes the VFS reachable from native programs through the pinned
`FS_*` family (`10h-1Fh`; the register ABI and every block are documented
in `docs/abi.md` section 10 and declared in `include/zephyros/fs.h`).
Nothing in provider semantics changed; the new layers are:

```text
kernel/vfs/process.c   global owner-tagged descriptor pool + per-process CWD
kernel/syscall/fs.c    the FS_* service bodies (argument + window validation)
user/runtime/zex_fs_runtime.asm + zos_fs.c   native gate stub + C wrappers
tests/target/m9/fstest/  fstest.zex (suite) and fstest_l.zex (second-exec)
```

**Descriptor pool.** One global `_DATA` array of 8 `struct
vfs_descriptor` entries; a file and a directory iterator share it.  Each
entry holds the owner pid, `FREE`/`FILE`/`DIRECTORY`, and the mount,
provider handle, size, offset and access flags of the object.  The fd an
application names is the pool index; `vfs_fd_check()` verifies range, type
and owner, and any mismatch is `ZOS_EINVAL`.  Allocation takes the lowest
free entry and a full pool is `ZOS_EMFILE`.  Provider handles never leave
the kernel.  Descriptors are not `stdin`/`stdout`, are not inherited, and
have no dup.

**CWD.** A static pool of `ZOS_EXEC_MAX_DEPTH` (4) canonical 128-byte
slots, allocated by `vfs_process_start(pid, parent_pid)` (which gives the
child the parent's CWD, or `/` under the launcher) and released by
`vfs_process_exit(pid)`, which is idempotent and also closes every
descriptor owned by the pid with `close` semantics.  Exit runs on every
path: `SYS_EXIT`, the raw-return `ZOS_EFAULT` trap, and the failed-launch
rollback (through `process_slot_release`).  Relative paths resolve against
`vfs_cwd_for(pid)`, so `chdir` crosses mounts transparently.

**Services.** Argument blocks live in the caller window `0100h-1FFFh`;
paths are copied into the shared VFS workspace and normalized against the
CWD; `buffer`/`out` pointers are validated against `0100h-DFFFh` and move
only through the checked bank-copy gates.  File data is chunked through the
workspace staging view, bridging to the caller bank through the 64-byte
common gate.  Every service validates its complete block before any state
change, and every `FS_*` call is refused with `ZOS_EBUSY` before the VFS
READY phase.

**Workspace (correction-01).** All pathname and transfer scratch is one
512-byte bank-7 workspace, `vfs_workspace`, owned by `kernel/vfs/` and
declared kernel-only in `kernel/vfs/vfs.h`.  It has two mutually exclusive
views: the staging view (all 512 bytes) and four 128-byte path slots
(copy-in A, normalized A, copy-in B, normalized B; the B pair is used only
by `rename`).  `OPEN`, `OPENDIR`, `STAT`, `MKDIR`, `RMDIR`, `UNLINK`,
`CHDIR` use the path view; `RENAME` uses both path pairs; `READ`, `WRITE`,
`READDIR` use the staging view; `GETCWD` uses the path view and then the
staging view.  Boot-time kernel callers (the loader and exec, through the
kernel file API) use the path view only.  Single-tasking execution, the
non-re-entrant gate and the absence of VFS use in an ISR mean the two views
are never live at once; `kernel/vfs/workspace.c` asserts that invariant at
run time when `ZOS_VFS_WS_GUARD=1` (the acceptance/simulator builds) and
compiles it out of production.  Every claim site brackets its use of the
workspace with `vfs_ws_enter()`/`vfs_ws_leave()`, including the module-phase
boot loader in `kernel/main.c`, so a guarded image panics
(`vfs: workspace view overlap`) if a live path pointer is ever overwritten
by staging data.  A host test pins the guard wiring: it is off in
production, on for the `m9-fs-selftest` image whose replay asserts "no
PANIC", and every claim is balanced.

**Acceptance.** `make m9-fs-selftest` builds the SIO-only image with ROMFS
at `/SYSTEM` and the test-only `ramfs` at `/` and `/R2`, and runs two
native programs: `fstest.zex` (namespace, ROMFS read/stat/enumeration/
EROFS, ramfs mutation/TRUNC/sizes, error paths, pool exhaustion, stale
handles, a failed confirmed write, `EXDEV` and argument-window faults) and
`fstest_l.zex`, exec'd a second time, which proves the 8-entry pool and the
CWD were released.  `tests/test_syscall_sim.py` replays the linked image,
checks the transcript and the released pool, and asserts the worst-case
kernel gate-stack depth stays inside 256 bytes.  The physical gate is
`make smoke-hardware-m9-fs SERIAL_PORT=/dev/ttyUSB0`, which observed
`fstest: PASS 9/9`, `fstest: launcher OK` and `m9 fs selftest: OK`
on hardware (2026-10-05; `docs/validation/2026-10-05-m9-fs-syscalls.md`).
Package 28 extends the suite with the `exec` case, so the current image
prints `fstest: PASS 10/10`; that was confirmed on hardware the same day
(`docs/validation/2026-10-05-m9-nested-exec.md`).

**Budget (section 32.39).** The production ledger moved from package 26
(`code 23526 end 7BE6h`, `arena 46 granules`, `data 1501`) to package 27
(`code 27636 end 8BF4h`, `arena 35 granules (20 boot, 15 free)`,
`data 3049 at D200h`, `isr 256`, `slack 279`): `+4110` `_CODE` and
`+1548` `_DATA` (the 512-byte staging buffer, the 4x128-byte CWD pool, the
descriptor pool and the syscall path scratch).  The production arena clears
the 32-granule floor and holds the 20-granule production boot set; the slack
target is re-checked by the supervisor after package 28.

Correction-01 then consolidated the six pathname buffers and the staging
buffer into the single workspace, taking the production ledger to
`code 27759 end 8C6Fh | arena 36 granules (20 boot, 16 free) | data 2113 at
D600h | isr 256 | slack 191`: `_DATA` drops by 936 bytes (`+56` `_CODE` for
the non-normalizing resolve/stat split and the piecewise panic line) and the
arena gains a granule.  The worst-case kernel gate-stack depth improved from
151 to 147 of 256 bytes, because the `READ`/`WRITE` and `READDIR` handlers
release the workspace view at a single `out:` exit instead of through a
bracketing wrapper call.  The corrected production image boots on hardware
to the V9958 monitor prompt with the same `scratch=0F19` figure as before
the correction (2026-10-05; `docs/validation/2026-10-05-m9-fs-syscalls.md`).

Package 28 (`SYS_EXEC`) then takes the production ledger to
`code 28159 end 8DFFh | arena 36 granules (20 boot, 16 free) | data 2113 at
D600h | isr 256 | slack 191`: `+400` `_CODE`, `+0` bank-7 `_DATA`, arena
unchanged, because the whole continuation mechanism (per-depth records,
three 48-byte frame saves, the gate-state shadows and the `SYS_EXEC` path
scratch) is common-resident because the resume gate restores it across the
mode change, while the `SYS_EXEC` path buffer is bank-7 `_DATA`
(`docs/validation/2026-10-05-m9-nested-exec.md`).  The `fstest` suite runs
one more case (`exec`, `PASS 10/10`, confirmed on hardware) and the nested
`exec` acceptance is replayed by `tests/test_m9_exec_sim.py`.
