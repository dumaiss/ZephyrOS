# ZMOD v1 Module Format and Loader Lifecycle

**Status:** implemented through M4 stage `08-m4-dynamic-driver-acceptance`.
The SIO driver ships as a packed ZMOD module embedded in every image's
boot catalog and is loaded during boot (the kernel links no SIO symbol at
all; `mapcheck.py` enforces that).  The stage-07 loader was
hardware-validated on 2026-10-02 (`m4 loader selftest: OK`; see
`docs/validation/2026-10-02-m4-loader-hardware.md`); stage 08 adds the
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

This document is the authoritative description of the ZephyrOS module
container (`ZMOD v1`), the bank-7 driver arena, the host packer, and the
transactional kernel loader.  The architectural contract is in
`docs/architecture.md` section 22; this file records the concrete format
that is now implemented and pinned by `include/zephyros/module.h` and
`tools/zmodpack.py`.

---

## 1. Driver arena

The arena is a linker-reserved region of bank 7:

```text
A000h-BFFFh   8192 bytes   16 contiguous 512-byte granules
```

* Reserved by `platform/zephyr80/memory.inc`
  (`ZOS_DRIVER_ARENA_BASE/END`, `ZOS_DRIVER_GRANULE_SIZE`,
  `ZOS_DRIVER_GRANULES`) and exported as the link symbols
  `__driver_arena_start` / `__driver_arena_end` by
  `kernel/module/arena.asm`.  The loader uses the symbols, never the
  numeric values; module code uses neither.
* Loadable kernel sections (`_CODE`, `_HOME`, `_GSINIT`, `_GSFINAL`,
  `_INITIALIZER`) must end below `A000h`.  `tools/mapcheck.py` rejects any
  area overlapping the arena and `tools/pack_image.py` rejects ROM payload
  bytes inside it; the 128 KiB boot image carries no arena content.
* `_DATA`/`_INITIALIZED` keep their established `C000h-DFFFh` runtime
  region; the M2 ISR stack keeps `DF00h`.
* Only the loader allocates granules, at boot, from the embedded catalog.

The build reports arena geometry and free granules:
`mapcheck.py` and `pack_image.py` both print the arena summary, and
`zephyros.manifest.json` carries an `arena` object
(`start`, `end`, `size`, `granule_size`, `granules`, `free_granules`,
`modules`).

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
| `06h` | 1 | `driver_abi` | must equal `ZDRIVER_ABI_VERSION` (1) |
| `07h` | 1 | `flags` | `0` (reserved) |
| `08h` | 2 | `image_size` | payload bytes loaded at `load_base` |
| `0Ah` | 2 | `bss_size` | zero-initialized bytes after the image |
| `0Ch` | 2 | `required_granules` | `ceil((image+bss)/512)`, 1..16 |
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
* Kernel imports are limited to the driver ABI v1 services:
  `device_register`, `irq_claim`, `irq_release`.
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
  "abi": 1,
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
  format:        ZMOD v1 driver, ABI 1
  image:         174 bytes
  bss:           2 bytes
  relocations:   16 (ZREL_ABS16)
  init:          +0x0037
  fini:          none
  total:         266 bytes
  allocation:    1 x 512-byte granules (15 free at boot)
```

The tool rejects, deterministically: bad radix/escape encodings,
unsupported relocation modes, unresolved externals, byte-fragmented
address stores, out-of-range entries, partial relocation-site overwrites,
area overruns, manifest/area mismatches, granule overflows, and container
overlap/truncation.  `validate` re-checks a container against the same
rules the kernel loader enforces; `dump` prints the canonical report plus
every relocation offset.

## 4. Loader lifecycle (kernel/module/loader.c)

```text
module_load(name, blob, size)
  │
  ├─ zmod_validate_container()        (relocate.c)  header, sizes,
  │                                   offsets, granule math, every
  │                                   relocation record
  ├─ first-fit granule allocation     bitmap over 16 granules;
  │                                   required_granules cross-checked
  ├─ descriptor ← LOADING             owner token for registrations/claims
  ├─ copy image, zero BSS,            ZREL_ABS16: word += load_base
  │  apply relocations
  ├─ init(load_base + init_offset)    driver API v1, under device and IRQ
  │                                   registry checkpoints
  │     ├─ success → RESIDENT         never freed; no unload operation
  │     └─ failure → fini (optional)  device/IRQ rollback, allocation
  │                                   zeroed and released, EMPTY again
```

* **Allocator.**  Fixed first-fit over `granule_used[16]`.  Allocation
  rounds `image_size + bss_size` up to 512 bytes, rejects zero, 16-bit
  overflow, a `required_granules` mismatch, and exhaustion (`ZMOD_ENOSPC`).
  Slack inside an allocation belongs to the module.  No public unload
  exists; freeing is internal to a failed load.
* **Validation-before-mutation.**  Header and relocation records are
  validated completely before any arena byte is written.  Tests plant
  canaries at the arena edges and prove rejection leaves them untouched.
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
  `ZMOD_ENOSPC` (arena/descriptor exhaustion), `ZMOD_ENOENT` (catalog),
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

### 4.2 Embedded catalog

`kernel/module/embedded.c` exposes a null-terminated
`struct zmod_catalog_entry` table (`name`, `blob`, `size`).  The loader
retrieves bytes from it and has no filesystem assumptions.  Every image's
boot catalog carries the packed SIO boot module selected by the Makefile;
`ZOS_M4_SELFTEST` builds additionally embed the three synthetic fixtures
generated from the packed `.zmod` containers (`build/modules/*.zmod` →
`m4_catalog.asm`/`.inc`).  See section 6 for the per-image selection.

## 5. Synthetic fixtures and self-test

| Fixture | Purpose | Image | BSS | Granules |
| --- | --- | ---: | ---: | ---: |
| `m4.probe` | picks device `m4.probe.a`/`m4.probe.b` from bit 9 of its relocated state pointer; the even-granule instance additionally claims `IRQ_CTC0` with an in-image handler | 174 | 2 | 1 |
| `m4.fail` | registers `m4.fail` and claims `IRQ_CTC1`, then returns failure so rollback is observable | 126 | 2 | 1 |
| `m4.filler` | verifies its 600-byte BSS pad is zero and registers nothing; used to exhaust the arena | 48 | 602 | 2 |

`make m4-loader-selftest` builds the fixture modules with the real
toolchain, packs them, embeds them, and runs `zos_run_m4_selftest` after
the production boot section (the boot catalog's SIO module is already
resident, so all fixture checks are relative to that baseline):

1. boot module presence and baseline counts,
2. fixture loads (`probe` and `filler` load; `fail` rolls back),
3. identical bytes loaded twice initialize at different arena addresses
   with the registered `state` pointer equal to `load_base + image_size`
   (each instance picks its device name from its own relocated state
   parity), plus rollback evidence, BSS zeroing, IRQ ownership, and
   handler-address accept/reject checks,
4. 2-granule fillers exhaust the arena; the next load fails with
   `ZMOD_ENOSPC` and all resident modules remain intact,
5. dynamic SIO acceptance: the bound console device must be the resident
   boot module, its relocated ops/state/name pointers must lie inside the
   allocation, CTC0/CTC1 must be unclaimed after init, and the load
   address is reported (`m4 selftest: sio at 0x....`), then
   `m4 selftest: OK`.

Host coverage:

* `tests/test_zmod.py` — packer/validator/container tests; the fixture
  test links the same objects with `sdldz80` at the packer's layout and
  compares every image byte (proving the mini-linker agrees with the real
  linker).
* `tests/test_m4_sim.py` — replays the linked kernel in the Z80 simulator:
  full self-test, every rejection class, arena canaries, and a direct
  two-address relocation check computed from the packed container.
* `tests/test_m4.py` — constant contract, mapcheck negative cases, and
  artifact/manifest checks.

## 6. SIO module build pipeline

The production SIO driver is no longer linked.  `drivers/sio/sio.c`,
`drivers/sio/sio_io.asm`, and the production-only entry
`drivers/sio/sio_module.c` are built under `build/modules/` (shared by
every image, so the packed bytes are identical across placement builds),
together with the shared `modules/runtime/sdcc_indirect_calls.asm` glue,
and packed through three manifests — the production manifest under
`modules/manifests/`, the test-variant manifests under `tests/modules/`:

| Module | Manifest | Module entry | Image | BSS | Granules |
| --- | --- | --- | ---: | ---: | ---: |
| `sio.zmod` | `modules/manifests/sio.json` | `sio_module_init` | 309 | 4 | 1 |
| `sio-irq.zmod` | `tests/modules/sio-irq.json` | `sio_irq_module_init` | 582 | 8 | 2 |
| `sio-fail.zmod` | `tests/modules/sio-fail.json` | `sio_fail_module_init` | 372 | 4 | 1 |

The IRQ self-test variant (`tests/modules/sio_irq_module.c`) claims
`IRQ_CTC0` inside its relocated init, verifies a second claim returns
`IRQ_EBUSY`, runs a bounded EI spin until three deliveries are counted,
checks its relocated state pointer, and releases the source before
registering the device.  `kernel_main()` starts CTC0 with the M2 lock
held (`irq_arch_lock`) and stops it after the load: a tick that fires
before the module claims the source must stay pending, because the
executive answers an unowned dispatch with `platform_irq_quiesce()` —
which resets CTC0 and would permanently stop the timer (the 2026-10-02
hardware failure).  The pending tick is delivered through the relocated
handler at the module's first `EI`, when it owns the source.  The
failure variant (`tests/modules/sio_fail_module.c`) registers its device
and claims CTC1, then returns `ZDRIVER_FAIL` so the loader's rollback can
be verified exactly.  The production entry contains no test branches.

Module sources use `static const` descriptors and link the shared
`modules/runtime/sdcc_indirect_calls.asm` source (compiled to
`build/modules/sdcc_indirect_calls.rel`) for
`___sdcc_call_iy`/`___sdcc_call_hl`, because the packer rejects the
byte-fragmented relocations SDCC emits for stack-built descriptors and
indirect-call libcalls, and a module may not import the kernel's private
copy.  Every variant packs without warnings and the packer reports
per-variant relocation counts and init offsets.

Embedding: `tools/zmodpack.py emit-asm` writes a generated assembly blob
plus header per image (`--header` also carries the `*_SIZE` constants;
`pack --report` writes `*.report.json` with symbol offsets used by the
host replays).  The Makefile selects the boot module per image:

| Image | Boot module |
| --- | --- |
| `build/` (production) | `sio.zmod` |
| `build-m4-loader-selftest/` (`make m4-loader-selftest`) | `sio.zmod` |
| `build-m4-selftest/` (`make m4-selftest`) | `sio-irq.zmod` |
| `build-m4-placement-selftest/` (`make m4-placement-selftest`) | `sio.zmod`, arena shifted to 9E00h-BE00h |
| `build-m4-failure/` (`make m4-failure-selftest`) | `sio-fail.zmod` |

`kernel/module/embedded.c` exposes the embedded boot blob through
`zmod_catalog`; the stage-07 fixture catalog (`zmod_fixture_catalog`) is
a test artifact defined in `tests/target/m4/m4_fixture_catalog.c` and
linked only into acceptance images.  `kernel/main.c` runs
`module_loader_init()` and `module_catalog_load_all()` and then binds the
console with `console_bind_defaults()` (capability-based, name-agnostic).
Module failures print `console: dynamic driver failed`; success prints
`console: dynamic sio0.b OK` through the bound module.  The loader makes
no assumptions about the module name, size, load address, relocation
list, or init entry — all of it comes from the catalog bytes.

`tools/mapcheck.py` fails the build on any linked `_sio_`, `_sio0b_`, or
`_sio_driver` symbol, so a static SIO copy cannot creep back in;
`mapcheck.py --production` additionally rejects every acceptance symbol
(retired static-driver remnants, test runners/results, fixture catalogs,
test SIO module entries).
