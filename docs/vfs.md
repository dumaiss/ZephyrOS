# ZephyrOS VFS Bootstrap and the ROMFS Provider

**Status:** implemented by work package `25-m9-vfs-bootstrap-romfs`
(2026-10-04), including corrections 01 and 02.  Host-validated and
simulator-accepted; physical acceptance is `PENDING` (see
`docs/validation/2026-10-04-m9-vfs-bootstrap.md`).  Bank-7 residency is
reported against the architecture section 32.39 ledger.

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
| syscalls, per-process descriptors, `SYS_FS_*` | 27 |
| data cache and generation keys (provider-local: IOC-FAT) | 30 |
| current working directory (`chdir`/`getcwd`) | 27 |
| writable providers, IOC-FAT at `/`, block class, ZDEV ABI v3 | 26, 29, 30 |
| `/` filesystem merge into root enumeration | 26 |
| cross-filesystem move/rename semantics | 31 |

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
`/` base until package 27 adds the per-process CWD.

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

`vfs_open()`/`vfs_read()`/`vfs_write()`/`vfs_sync()`/`vfs_close()`/
`vfs_stat()` operate on caller-provided `struct vfs_file` storage: there is
no descriptor table yet (package 27 adds the process-visible pool).  Every
one of them resolves the path, selects the mount and calls the provider
with the provider-relative path.  Reads are clamped to EOF by VFS, so a
provider only ever sees an in-range request.

`stat("/")` and `stat(path)` for a mounted mountpoint are answered by VFS
itself as directories, without consulting a provider.  There is no
`statfs` in M9.

`VFS_O_EXEC` (0x8000) is a kernel-only open hint, not a native flag: the
executable loader sets it, and a provider that distinguishes executable
media (ROMFS) refuses a non-`ZEX` entry opened with it.  That preserves
the M6 `ZEX_EXEC_ERR_TYPE` rejection now that the native type set is
only `FILE`/`DIR`.

`readdir("/")` is a VFS operation: it yields every mounted root-level
mountpoint (exactly one component) as a `DIR` entry.  With no filesystem
at `/` — the production M9 namespace — the root lists only the
mountpoints.  Merging a `/` filesystem's own entries with mountpoint
shadowing (architecture section 32.7) arrives with that filesystem
(package 30).

Mutation entry points (`vfs_mkdir`, `vfs_rmdir`, `vfs_unlink`,
`vfs_rename`) live in `kernel/vfs/mutate.c` and are the kernel half of the
`SYS_FS_*` family that package 27 wires up.  Their purpose in this package
is to give the read-only rule one home: a provider that leaves a mutation
pointer null gets `ZOS_EROFS` from VFS, and a cross-mount rename is refused
with `ZOS_EXDEV` rather than turned into a copy.

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

### 9.3 Arena geometries

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
| required non-system mount failure: diagnostic + `PANIC: vfs: mount failed` before the monitor, system entry still `MOUNTED`, failed entry records `-ZOS_ENODEV` | `tests/test_m9_vfs_sim.py` (`tests/m9/mounts-required-fail.json`, `make m9-required-mount-selftest`) |
| ZEX/ZMOD loading through VFS, M8 transcripts unchanged | `tests/test_m3_sim.py` (production boot replay), `tests/test_m8*.py`, `tests/test_romfs*.py` |
| ROMFS name validation | `tests/test_romfs.py` (slash and case-collision negatives) |
| production arena gate, `ZOS_ARENA_MIN_GRANULES` | `tools/mapcheck.py`, `tools/pack_image.py`, `make check` |
| embedded-catalog images link no VFS byte | `tools/mapcheck.py` (`vfs_image` classification + `VFS_FORBIDDEN_PREFIXES`), `tests/test_romfs.py` (`TestRomfsMapcheck.test_embedded_catalog_image_rejects_vfs_symbols`), `tests/test_m4.py` (per-image symbol sweep), `tests/test_m9_vfs.py` |
| physical boot | `docs/validation/2026-10-04-m9-vfs-bootstrap.md` (**PENDING**) |
