# ZMOD v1 Module Format and Loader Lifecycle

**Status:** current through M9, driver ABI v2 (package 25).  The SIO,
IOC, HID, and V9958 drivers ship as packed ZMOD modules and are loaded
during boot through the generic M4 loader; since M5 the container bytes
come from the ROMFS recovery store and since M9 they are opened as
`/SYSTEM/<name>.zmod` through the kernel VFS and bound with
`vfs_file_source()` (the on-disk ZMOD format and the bounded read-source
adapter are unchanged; see `docs/vfs.md`).  Since M8 the header `driver_abi`
must equal `ZDRIVER_ABI_VERSION` (2) and production derives the driver
arena from the aligned linked kernel end in a two-pass link.  The
production kernel and every ROMFS-boot image (M5 onward) link no embedded
boot blob at all (`tools/mapcheck.py --production` rejects embedded boot
blob/catalog symbols).  The stage-07/stage-08 records above are retained
as historical acceptance evidence (`docs/validation/`); the description
below reflects the current boot path.  The stage-07 loader was
hardware-validated on 2026-10-02 (`m4 loader selftest: OK`; see
`docs/validation/2026-10-02-m4-loader-hardware.md`); stage 08 added the
dynamic-console transcript, the IRQ self-test module variant, a
shifted-base placement image using identical module bytes, and an
init-failure image that proves exact rollback.  The stage-08 acceptance
image was hardware-validated on 2026-10-02 (`console: dynamic sio0.b OK`,
`m4 loader selftest: OK`, `m4 selftest: sio at 0xA000`, `m4 selftest:
OK`) after fixing the CTC0 start/claim race.  The placement image then
passed with the same module bytes relocated to the shifted base
(`m4 selftest: sio at 0x9E00`) and the failure image passed with exact
rollback (`console: dynamic driver failed`, `m4 failure selftest: OK`);
all runs are recorded in `docs/validation/2026-10-02-m4-acceptance.md`.
M5 host evidence is recorded in `docs/validation/2026-10-03-m5-romfs.md`.

The embedded test catalog and the M1-M7 acceptance images that used it
were retired under the Mn-1 retention rule (work package 25a).  Every
image now boots its module set from the recovery store through the VFS,
and the loader mechanism coverage (validation, relocation, failed-init
rollback, arena exhaustion) runs on that path in the subsystem acceptance
image (`tests/target/loader/`, exercised by
`tests/test_subsys_sim.py`).

This document is the authoritative description of the ZephyrOS module
container (`ZMOD v1`), the bank-7 driver arena, the host packer, and the
transactional kernel loader.  The architectural contract is in
`docs/architecture.md` section 22; this file records the concrete format
that is now implemented and pinned by `include/zephyros/module.h` and
`tools/zmodpack.py`.

---

## 1. Driver arena

The arena is a linker-reserved region of bank 7.  Every image derives its
bounds from the aligned linked kernel end and the derived runtime-data
base in a two-pass link:

```text
arena = [align_up(__kernel_code_end, 512), data_base)   minimum 16 KiB
data_base = align_down(DF00h - (l__DATA + l__INITIALIZED), 512)
```

There is exactly one geometry; the probe link places no arena and the
fixed `A000h-BFFFh` rectangle is retired (work packages 25a/25b).  The
derived arena of the current production image is `7600h-D7FFh` (49
granules, 20 used by the boot set) -- see `docs/memory.md` section 2.2
and `docs/architecture.md` section 30.2.
The loader and the generated `arena_geometry.inc` consume the linked
symbols; nothing below depends on the numeric base.*

* Declared by the generated `memory.inc` include
  (`ZOS_DRIVER_ARENA_BASE/END`, `ZOS_DATA_BASE/SIZE`,
  `ZOS_DRIVER_GRANULE_SIZE`, `ZOS_DRIVER_GRANULES`) and exported as the
  link symbols `__driver_arena_start` / `__driver_arena_end` by
  `kernel/module/arena.asm`.  The loader uses the symbols, never the
  numeric values; module code uses neither.
* Loadable kernel sections (`_CODE`, `_HOME`, `_GSINIT`, `_GSFINAL`,
  `_INITIALIZER`) must end below the arena start
  (`__driver_arena_start`, the aligned linked-kernel end), so the two
  passes must agree.  `tools/mapcheck.py` rejects any area overlapping the
  arena and `tools/pack_image.py` rejects ROM payload bytes inside it; the
  512 KiB boot image carries no arena content.
* `_DATA`/`_INITIALIZED` start at the derived data base and must stay
  below the M2 ISR stack at `DF00h`.
* Only the loader allocates granules, at boot, from its byte source:
  the ROMFS recovery store reached as `/SYSTEM` through the VFS (since
  M9), in production and every acceptance image.

The build reports arena geometry, free granules and the single bank-7
budget ledger: `mapcheck.py` and `pack_image.py` both print the summary,
`zephyros.manifest.json` carries an `arena` object
(`start`, `end`, `size`, `granule_size`, `granules`, `free_granules`,
`modules`) plus a `bank7` ledger object, and `arena.plan.json` carries the
same ledger.

## 2. Container layout

A ZMOD v1 container is a packed little-endian byte stream:

```text
offset 0                28-byte header
offset reloc_offset     4 * reloc_count relocation records
offset payload_offset   linked-at-zero image (image_size bytes)
total_size = payload_offset + image_size
```

`total_size` must equal the exact byte length handed to the loader.
Relocation records and payload may not overlap the header or each other;
the packer emits the table directly after the header and the payload
directly after the table.

### 2.1 Header (28 bytes)

| Offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| `00h` | 4 | `magic[4]` | `"ZMOD"` |
| `04h` | 1 | `format_version` | `1` |
| `05h` | 1 | `module_type` | `1` = `DRIVER` (only supported type) |
| `06h` | 1 | `driver_abi` | must equal `ZDRIVER_ABI_VERSION` (2) |
| `07h` | 1 | `flags` | `0` (reserved) |
| `08h` | 2 | `image_size` | payload bytes loaded at `load_base` |
| `0Ah` | 2 | `bss_size` | zero-initialized bytes after the image |
| `0Ch` | 2 | `required_granules` | `ceil((image+bss)/512)`, 1..80 (format maximum), further bounded by the linked arena's live granule count |
| `0Eh` | 2 | `init_offset` | image-relative init entry (required) |
| `10h` | 2 | `fini_offset` | image-relative cleanup entry, `FFFFh` = none |
| `12h` | 2 | `reloc_offset` | offset of the relocation table |
| `14h` | 2 | `reloc_count` | number of 4-byte records |
| `16h` | 2 | `payload_offset` | offset of the image bytes |
| `18h` | 2 | `total_size` | total container bytes |
| `1Ah` | 2 | `reserved` | `0` |

`struct zmod_header` in `include/zephyros/module.h` overlays this layout;
compile-time assertions pin `sizeof` and every field offset.

### 2.2 Relocation records

```text
type:u8 (1 = ZREL_ABS16)   reserved:u8 (=0)   patch_offset:u16
```

`patch_offset` is relative to the loaded image and must satisfy
`patch_offset + 2 <= image_size`.  At load time each record is applied as:

```text
word_at(load_base + patch_offset) += load_base
```

The addition is checked against 16-bit overflow before it is stored; no
record may touch a byte outside the image.  Although the container could
express it, `ZREL_ABS16` is the only relocation type M4 supports.

## 3. Module image conventions

* Modules are linked **at base zero** by `tools/zmodpack.py`.  Every
  private pointer in the image is a module-relative offset that the
  loader corrects through the relocation table.  The packer rejects any
  relocation it cannot express as `ZREL_ABS16`.
* A module may reach the kernel only through the `const struct
  zdriver_api *` passed to its init entry.  Direct references to kernel
  symbols are unresolved externals and are rejected at pack time.
* Kernel imports are limited to the driver ABI v2 services:
  `device_register`, `irq_claim`, `irq_release`, `device_find`,
  `device_interface`, `resource_size`, `resource_read`.  The callback
  order in `struct zdriver_api` is part of the ABI (pinned by
  compile-time assertions and host tests).
* `device_find(class_id, required_caps, after)` selects by class plus
  required capabilities (`ZDEV_CLASS_ANY` = any class; zero
  `required_caps` is invalid); `device_interface(handle, class, &ops,
  &state)` verifies the class and a non-null class interface.  Both are
  read-only; the registry exposes no mutation path.
* `resource_size(name, &size)` and `resource_read(name, offset, dst,
  length)` resolve only ROMFS entries carrying `ROMFS_FLAG_DATA` and
  bound every read to the resource; `romfs_file_t` and the mutable
  filesystem state stay kernel-private.
* SDCC emits byte-fragmented address stores (relocation modes `09h`/`89h`)
  for stack-built structures whose pointer fields are written with
  `ld (ix+d), #<sym` / `# >sym`.  These cannot be relocated by a 16-bit
  word patch, so the packer rejects them; module code must use static
  (`const`) descriptor initializers instead.
* Modules must provide their own SDCC indirect-call trampolines
  (`___sdcc_call_iy`, `___sdcc_call_hl`) because the kernel's copies are
  private; `modules/runtime/sdcc_indirect_calls.asm` is the single shared
  source, compiled separately into every ZMOD that needs it so each
  container remains self-contained.
* Uninitialized module state belongs to BSS areas (`_DATA` in SDCC
  output); the loader zeroes it before init and stores private state
  inside the module allocation only.

### 3.1 Module manifest

`tools/zmodpack.py pack` takes a small JSON manifest (see
`tests/modules/*.json`):

```json
{
  "name": "m4.probe",
  "type": "driver",
  "abi": 2,
  "flags": 0,
  "objects": ["m4_probe.rel", "sdcc_indirect_calls.rel"],
  "image_areas": ["_CODE"],
  "bss_areas": ["_DATA"],
  "init": "m4_probe_init",
  "fini": null
}
```

`objects` are resolved against `--objdir` (default: the manifest's
directory).  `image_areas` are concatenated in the listed order starting
at offset zero; `bss_areas` follow the image.  Every non-empty area in the
objects must be listed; empty SDCC areas are ignored.  `init`/`fini` are C
identifiers (the packer also accepts the `_`-prefixed assembler spelling).

### 3.2 Packer output and guarantees

For each output the packer reports:

```text
build/modules/m4_probe.zmod
  format:        ZMOD v1 driver, ABI 2
  image:         182 bytes
  bss:           2 bytes
  relocations:   16 (ZREL_ABS16)
  init:          +0x0037
  fini:          none
  total:         266 bytes
  allocation:    1 x 512-byte granules
```

The tool rejects, deterministically: bad radix/escape encodings,
unsupported relocation modes, unresolved externals, byte-fragmented
address stores, out-of-range entries, partial relocation-site overwrites,
area overruns, manifest/area mismatches, granule overflows, and container
overlap/truncation.  `validate` re-checks a container against the same
rules the kernel loader enforces; `dump` prints the canonical report plus
every relocation offset.

## 4. Loader lifecycle (kernel/module/loader.c, kernel/module/relocate.c)

```text
module_load(name, blob, size)        module_load_source(name, source)
  │ memory-blob source wrapper         │ VFS-bound file source
  │                                    │ (vfs_file_source over an open
  │                                    │  /SYSTEM/<name>.zmod)
  └──────────────┬─────────────────────┘
                 ▼
  zmod_validate_source()               header, sizes, offsets, granule math,
                                       and every relocation record, read
                                       through the bounded source and
                                       validated before the arena is touched
  ├─ first-fit granule allocation      bitmap over the architectural
  │                                    maximum (80 granules); live count
  │                                    derived from the linked arena
  │                                    bounds at module_loader_init();
  │                                    required_granules cross-checked
  ├─ descriptor ← LOADING              owner token for registrations/claims
  ├─ stream image copy, zero BSS,      image and relocations are read in
  │  apply relocations                 bounded chunks (128 bytes);
  │                                    ZREL_ABS16: word += load_base
  ├─ init(load_base + init_offset)     driver ABI v2, under device and IRQ
  │                                    registry checkpoints
  │     ├─ success → RESIDENT          never freed; no unload operation
  │     └─ failure → fini (optional)   device/IRQ rollback, allocation
  │                                    zeroed and released, EMPTY again
```

* **Byte sources.**  `struct zmod_source` (`read(ctx, offset, dst, length)`,
  `ctx`, `size`) is the only way container bytes enter the loader.  The
  historical `module_load()` wraps the memory blob; since M9 the boot
  modules are opened with `vfs_open("/SYSTEM/<name>.zmod")` and wrapped by
  `vfs_file_source()` (the ROMFS bytes underneath are read by the M5
  reader through the M9 ROMFS provider; `romfs_load_zmod()` no longer
  exists).  Both share the same validation, copy, relocation, and
  transaction code — there is no second loader.
* **Allocator.**  First-fit over a 512-byte granule bitmap sized to the
  architectural maximum (80 granules), with the live granule count
  derived from the linked arena bounds at loader init.  Allocation
  rounds `image_size + bss_size` up to 512 bytes, rejects zero, 16-bit
  overflow, a `required_granules` mismatch, and exhaustion (`ZMOD_ENOSPC`).
  Slack inside an allocation belongs to the module.  No public unload
  exists; freeing is internal to a failed load.
* **Validation-before-mutation.**  Header and relocation records are
  validated completely — through the bounded source for source-backed
  containers — before any arena byte is written.  Tests plant canaries at
  the arena edges and prove rejection leaves them untouched; corrupted
  ROMFS containers are rejected with the exact loader error codes before
  allocation.
* **Transactions.**  `device_registry_set_owner`/`irq_registry_set_owner`
  record the loading module in every registration and claim made during
  init.  On failure `device_registry_rollback` and `irq_registry_rollback`
  remove exactly that module's changes; earlier modules are untouched.
  `fini_offset` runs only as init cleanup and only when present; its
  result is ignored.
* **States.**  `ZMOD_STATE_EMPTY` → `LOADING` → `RESIDENT`; a failed load
  returns the descriptor to `EMPTY` and increments `module_failed_count`.
* **Loader errors** (negated `ZOS_*` values, see `module.h`):
  `ZMOD_EINVAL` (bad field/argument), `ZMOD_EFORMAT` (structure,
  truncation, overlap, range), `ZMOD_ENOTSUP` (version/type/ABI/reloc),
  `ZMOD_ENOSPC` (arena/descriptor exhaustion), `ZMOD_ENOENT` (no such
  source entry),
  `ZMOD_EINIT` (module init returned failure).

### 4.1 IRQ handler validation

`irq_claim` accepts a handler address when it lies

* inside linked bank-7 kernel code (`2000h` .. `irq_kernel_code_end`), or
* inside the image of the currently initializing (LOADING) module, or
* inside a RESIDENT module allocation.

Failed/freed allocations, LOADING modules that are not the active load,
and any other address are rejected with `IRQ_EINVAL`.  Once a module is
RESIDENT its registered handler cannot become dangling because M4 has no
unload.

### 4.2 Boot-module source

Every image -- production and acceptance -- uses the one container
source: the ROMFS recovery store reached as `/SYSTEM` through the VFS,
opened with `vfs_open("/SYSTEM/<name>.zmod")` and wrapped by
`vfs_file_source()`.  The embedded boot catalog (`kernel/module/embedded.c`,
`module_catalog_load_all`, the `zmod_catalog` table and
`struct zmod_catalog_entry`) was deleted under the Mn-1 retention rule
(work package 25a); `tools/mapcheck.py --production` still rejects its
symbols (`_z4_module_boot`, `_zmod_catalog`, `_module_catalog_load_all`)
so they cannot creep back in.

## 5. Synthetic fixtures and self-test

| Fixture | Purpose | Image | BSS | Granules |
| --- | --- | ---: | ---: | ---: |
| `m4.probe` | picks device `m4.probe.a`/`m4.probe.b` from bit 9 of its relocated state pointer; the even-granule instance additionally claims `IRQ_CTC0` with an in-image handler | 182 | 2 | 1 |
| `m4.fail` | registers `m4.fail` and claims `IRQ_CTC1`, then returns failure so rollback is observable | 130 | 2 | 1 |
| `m4.filler` | verifies its 600-byte BSS pad is zero and registers nothing; used to exhaust the arena | 48 | 602 | 2 |
| `m8.api` | driver ABI v2 discovery/resource-import fixture: class+capability `device_find` iteration, `device_interface` success/mismatch/stale errors, and resource imports before ROMFS is published | 1129 | 6 | 3 |

The fixtures are built with the real toolchain and packed under
`build/modules/`; `make zmod-test` validates every container.  The loader
mechanism coverage runs on the current boot path: the loader group of the
subsystem acceptance image (`tests/target/loader/loader_test.c`, replayed
by `tests/test_subsys_sim.py`) opens each fixture as `/SYSTEM/<name>.zmod`
through the same `vfs_open` + `vfs_file_source` + `module_load_source`
path the kernel boot uses, so it exercises the production loader and the
production filesystem stack together.  The boot module is RESIDENT and the
console is bound before the fixtures load, the baseline for every count:

1. malformed containers (bad magic, truncated header) are rejected by
   `zmod_validate_source` with their exact error (`ZMOD_EINVAL`,
   `ZMOD_EFORMAT`) before the arena, the descriptor table or the registry
   is touched, and the rejection is not counted as a failed init,
2. `m4.fail` registers a device, claims `IRQ_CTC1` and returns failure;
   the loader rolls the registration, the claim, the granules and the
   descriptor back exactly, and the identical load is repeatable,
3. `m4.probe` loads from the recovery store; its registered device, ops,
   name and state pointers all lie inside its own allocation with state
   at `load_base + image_size`, and its init-time `IRQ_CTC0` claim is
   owned by the module,
4. IRQ handler addresses are validated against RESIDENT module images
   only (a pointer inside the allocation is accepted; a stale or
   out-of-arena address is refused with `IRQ_EINVAL`),
5. `m4.filler` verifies its 600-byte BSS pad is zero before init,
6. repeated fillers exhaust the derived arena with `ZMOD_ENOSPC` while
   the earlier residents and the boot module stay intact, and the bound
   console keeps working.

`m8.api` remains a host-validated ABI v2 discovery/resource-import
container (`tests/test_zmod.py`); it is not part of the subsystem
group.

Host coverage:

* `tests/test_zmod.py` — packer/validator/container tests; the fixture
  test links the same objects with `sdldz80` at the packer's layout and
  compares every image byte (proving the mini-linker agrees with the real
  linker), and relocation is checked at two bases
  (`test_two_bases_shift_patched_words_by_the_delta`); bad version/ABI
  and every relocation-rejection class are covered here.
* `tests/test_subsys_sim.py` — replays the linked subsystem image in the
  Z80 simulator, including the loader group above and its arena
  exhaustion.
* `m8-fb-arena` (`tests/test_m8_fallback.py`) — the current on-target
  arena-exhaustion image.

## 6. SIO module build pipeline

The production SIO driver is no longer linked.  `drivers/sio/sio.c`,
`drivers/sio/sio_io.asm`, and the production-only entry
`drivers/sio/sio_module.c` are built under `build/modules/` (shared by
every image, so the packed bytes are identical everywhere),
together with the shared `modules/runtime/sdcc_indirect_calls.asm` glue,
and packed through three manifests — the production manifest under
`modules/manifests/`, the test-variant manifests under `tests/modules/`:

| Module | Manifest | Module entry | Image | BSS | Granules |
| --- | --- | --- | ---: | ---: | ---: |
| `sio.zmod` | `modules/manifests/sio.json` | `sio_module_init` | 313 | 4 | 1 |
| `sio-irq.zmod` | `tests/modules/sio-irq.json` | `sio_irq_module_init` | 586 | 8 | 2 |
| `sio-fail.zmod` | `tests/modules/sio-fail.json` | `sio_fail_module_init` | 376 | 4 | 1 |

The IRQ variant (`tests/modules/sio_irq_module.c`) claims `IRQ_CTC0`
inside its relocated init, verifies a second claim returns `IRQ_EBUSY`,
runs a bounded EI spin until three deliveries are counted, checks its
relocated state pointer, and releases the source before registering the
device.  The failure variant (`tests/modules/sio_fail_module.c`)
registers its device and claims CTC1, then returns `ZDRIVER_FAIL` so the
loader's rollback can be verified exactly.  Both remain packed test
containers validated by `make zmod-test`; the IRQ mechanisms that ran
through the pre-M8 images are now covered on the current boot path by the
IRQ group of the subsystem acceptance image (`tests/target/irq/`,
`tests/test_subsys_sim.py`) with its test-owned CTC0 helper
(`tests/target/support/ctc_test.c`).  The production entry contains no
test branches.

Module sources use `static const` descriptors and link the shared
`modules/runtime/sdcc_indirect_calls.asm` source (compiled to
`build/modules/sdcc_indirect_calls.rel`) for
`___sdcc_call_iy`/`___sdcc_call_hl`, because the packer rejects the
byte-fragmented relocations SDCC emits for stack-built descriptors and
indirect-call libcalls, and a module may not import the kernel's private
copy.  Every variant packs without warnings and the packer reports
per-variant relocation counts and init offsets.

`tools/zmodpack.py pack --report` writes `*.report.json` with the symbol
offsets the host replays use (`emit-asm` can still emit a container as an
assembly blob plus header, host-tested by `tests/test_zmod.py`).  No image
links such a blob: the Makefile selects the boot module source per image:

| Image | Boot module / source |
| --- | --- |
| `build/` (production) | `sio.zmod` from ROMFS through the VFS (`/SYSTEM`) |
| `build-m9-vfs-selftest/` (`make m9-vfs-selftest`) | ROMFS at `/SYSTEM` through VFS: `sio.zmod`, `ioc.zmod`, `hid.zmod`, `v9958.zmod` + font + monitor, optional-mount failure diagnosed |
| `build-subsys-selftest/` (`make subsystem-selftest`) | the current boot path with the SIO-only boot set, plus the loader/ROMFS/process fixtures |
| `build-romfs-corrupt/` (`make romfs-corrupt-selftest`) | corrupted ROMFS metadata: panic before loading |
| `build-m8-selftest/` (`make m8-selftest`) | ROMFS: `sio.zmod`, `ioc.zmod`, `hid.zmod`, `v9958.zmod` + font + monitor |
| `build-m8-fb-*/` (`make m8-fallback-selftests`) | ROMFS failure variants (missing/substituted entries), same load order |

`kernel/main.c` runs `module_loader_init()` and then loads the boot
module from the one source: `vfs_init()` (which mounts ROMFS at
`/SYSTEM`) followed by `vfs_open("/SYSTEM/<name>.zmod")` +
`vfs_file_source()`.  The embedded-catalog alternative was deleted under
the Mn-1 retention rule (work package 25a).  The boot path continues with the
deterministic `ioc.zmod`, `hid.zmod`, `v9958.zmod` sequence (each load
gated by `ZOS_IOC_BOOT` / `ZOS_HID_BOOT` / `ZOS_V9958_BOOT`), and a
failed optional module is rolled back by the loader and reported as
`<name>: unavailable 0x<ZOS_E*>` on the direct early SIO; every earlier
resident stays registered.  The console is then bound with
`console_bind_defaults()` (capability-based, name-agnostic) and the
monitor launcher runs.  Module failures of the required SIO fallback
print `console: dynamic driver failed`; success prints
`console: dynamic sio0.b OK` through the bound module.  A corrupt
recovery image panics with `PANIC: romfs: corrupt recovery image` before
any module execution.  The loader makes no assumptions about the module
name, size, load address, relocation list, init entry, or where the
container bytes live — all of it comes from the bounded source.

`tools/mapcheck.py` fails the build on any linked `_sio_`, `_sio0b_`, or
`_sio_driver` symbol, so a static SIO copy cannot creep back in;
`mapcheck.py --production` additionally rejects every acceptance symbol
(retired static-driver remnants, test runners/results, fixture catalogs,
embedded boot blob/catalog symbols, test SIO module entries).
