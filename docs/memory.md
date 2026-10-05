# ZephyrOS Memory Model (M8/M9, implemented)

This is the implemented, linker-enforced memory ownership table for the
mode `10`/`11` Zephyr-80 architecture.  The contract is in
`docs/architecture.md`; this file records what the build actually places
where, and how it is validated.

## 1. Kernel-visible regions

```text
FFFFh +-------------------------------+
      | bank 0 common         8 KiB    |  FF00h-FFFFh  common boot/runtime stack
FF00h +-------------------------------+
      | bank 0 common code/data        |
      |   _COMMON / _COMMON_DATA       |
E000h +-------------------------------+
      | bank 7 OS body       48 KiB    |  mode 11 view
      |   _CODE .. _DATA               |
2000h +-------------------------------+
      | bank 0 (caller)       8 KiB    |
0000h +-------------------------------+
```

| Region | View | Content |
| --- | --- | --- |
| `0000h-00FFh` | caller window | ZephyrOS-owned native page zero: reset copier (`0000h-0026h`) and the three-byte `RST 30h` vector (`JP zos_syscall_entry`) at `0030h-0032h`; the rest is reserved |
| `0100h-1FFFh` | caller window | unused (FF-filled in the image); at M1, native syscall argument blocks must lie here |
| `2000h-arena start` | bank 7 in mode 11 | loadable kernel image (code, `_HOME`, `_INITIALIZER`, `_GSINIT`, `_GSFINAL`); ends at the linked `__kernel_code_end` |
| `arena start-data base` | bank 7 in mode 11 | driver arena, derived since package 25b from the aligned linked-kernel end `__driver_arena_start` to the derived data base, at least 32 x 512-byte granules (16 KiB) in production; test-hook images check 16 as the minimum and rely on the boot-set plan. Never part of the ROM payload and never linked by content |
| `data base-DFFFh` | bank 7 in mode 11 | zero-initialized runtime area (`_DATA`, `_INITIALIZED`), never ROM-loaded; the base is derived per image |
| `DF00h-DFFFh` | bank 7 in mode 11 | 256-byte M2 ISR stack; initial `SP=E000h` |
| `E000h-FCFFh` | physical bank 0 | ordinary permanent common code/data (`_COMMON`, `_COMMON_DATA`), including the M5 ROM-access staging buffer; `EEFEh-EF07h` is the fixed ROM-access transition block (trampoline + mirrored stub) |
| `FD00h-FDFFh` | physical bank 0 | 256-byte-aligned IM2 vector page (`I=FDh`) |
| `FE00h` | physical bank 0 | vector-`FFh` guard byte |
| `FE01h-FE7Fh` | physical bank 0 | common IRQ transition state |
| `FE80h-FEFFh` | physical bank 0 | 128-byte common IRQ transition stack |
| `FF00h-FFFFh` | physical bank 0 | common boot/runtime stack; `SP = 0000h` (top-exclusive `10000h`), grows down |

Mode `10` maps the selected application bank at `0000h-DFFFh`; mode `11`
keeps the caller bank at `0000h-1FFFh` and maps bank 7 at `2000h-DFFFh`.
Bank 7 is reserved to ZephyrOS and is never an application bank.

### 1.1 ROM side: the 512 KiB programmed image

The programmed ROM is eight 64-KiB pages.  Pages 0/1 are the boot pages
(see `docs/boot.md`); pages 2-7 hold the read-only ROMFS recovery store.
ROM is only ever selected in mode 00, which answers *every* read
(including instruction fetch) from the selected ROM page while every
write targets the selected SRAM bank — so the ROM-access transition is a
stackless mirrored stub (`EEFEh-EF07h`: trampoline `OUT` plus `LDIR` and
the latch-restoring `OUT`), byte-identical in common SRAM and in every
recovery page.  Each recovery page therefore contributes a 48-KiB
logical window (`ZOS_ROMFS_WINDOW_SIZE`) at `0000h-BFFFh`; the
`C000h-FFFFh` half carries only the stub mirror and is never file data.
Six windows total (`ZOS_ROMFS_CAPACITY` = 288 KiB).  The geometry is
declared in `platform/zephyr80/memory.h/.inc`, mirrored into the image by
`tools/pack_image.py`, and recorded in `docs/romfs-format.md`.

## 2. Linker area ownership

The linker control file (`linker/zephyr80.lk` template; the build
substitutes the derived data base into `build/zephyr80.lk`) assigns:

```text
_CODE   = 2000h            bank-7 loadable code
_DATA   = <derived base>   bank-7 zero-initialized runtime area
_COMMON = E000h            permanent common code/data
```

The `_DATA` base is derived, not fixed: the two-pass build computes
`data_base = align_down(DF00h - (l__DATA + l__INITIALIZED), 512)` and
passes it as the `_DATA` base, so runtime data sits directly below the
`DF00h` ISR stack and the arena fills everything between the aligned
kernel end and that base.

Unbased areas follow their preceding based area in first-appearance order,
fixed by the declarations at the top of `boot/start.asm`:

```text
_CODE (2000h) -> _HOME -> _INITIALIZER -> _GSINIT -> _GSFINAL   loadable
_DATA (derived base) -> _INITIALIZED                            runtime
_COMMON (E000h) -> _COMMON_DATA                                 common
```

Every owned region exports `s__<area>`/`l__<area>` start/size symbols
(linker-synthesized); the startup initializer/BSS operations consume
`s__INITIALIZER`/`l__INITIALIZER`, `s__INITIALIZED`, and
`s__DATA`/`l__DATA`.

| Area | Region | Holds |
| --- | --- | --- |
| `_RESET` | `0000h-0026h` | stackless reset copier (`.org 0`, ABS) |
| `_PAGE_ZERO` | `0030h-0032h` | `RST 30h` vector: `JP zos_syscall_entry` (ABS) |
| `_CODE` | `2000h-arena start` | kernel C code, driver/service code, string literals |
| `_HOME` | `2000h-arena start` | static helpers |
| `_INITIALIZER` | `2000h-arena start` | ROM image of initialized data |
| `_GSINIT`/`_GSFINAL` | `2000h-arena start` | static initializers + terminator |
| `_DATA` | `derived base-DFFFh` | zeroed globals/statics, registry table, driver state |
| `_INITIALIZED` | `derived base-DFFFh` | RAM image of initialized data (ops/descriptor tables) |
| `_COMMON` | `E000h-FCFFh` | boot entry, gates, IRQ stubs, console, panic |
| `_COMMON_DATA` | `E000h-FCFFh` | transition-critical state |
| `_IRQ_ISR_STACK` | `DF00h-DFFFh` | bank-7 handler stack |
| `_IRQ_VECTOR` | `FD00h-FE00h` | IM2 page plus guard |
| `_IRQ_COMMON_STATE` | `FE01h-FEFFh` | IRQ state plus transition stack |
| (reserved) | `FF00h-FFFFh` | common boot/runtime stack; nothing may link here |

## 2.1 M3 device-layer placement

The M3 device registry, console service, static driver descriptors, ops
tables, and driver-private state are all bank-7 residents: code in
`_CODE`, state in `_DATA`/`_INITIALIZED`, below the `DF00h` ISR stack.
Nothing from the device layer enters `_COMMON`/`_COMMON_DATA`; only the
M2 source stubs, the common IRQ crossing machinery, and the early
console/panic path are common-resident.  `tools/mapcheck.py` enforces
this both by explicit symbol list and by a prefix sweep over
`_device_`, `_console_`, `_sio0b_`, `_sio_`, `_driver_`,
`_boot_drivers`, and `_m3_` symbols.  See `docs/devices.md`.

## 2.2 Bank-7 geometry: derived arena and data base (M9, package 25b)

There is one bank-7 geometry.  A deterministic two-pass build derives it:

1. **Probe link.**  The same object list links with a bootstrap
   `arena.asm` object that places no arena at all (`ZOS_ARENA_PROBE = 1`);
   nothing can constrain kernel growth.
2. **Derive.**  `tools/mapcheck.py geometry derive` measures the exclusive
   end of every loadable bank-7 area (`_CODE`, `_HOME`, `_INITIALIZER`,
   `_GSINIT`, `_GSFINAL`) and the runtime-data size (`l__DATA +
   l__INITIALIZED`) in the probe map, then writes:

   ```text
   __kernel_code_end     = max end of the loadable areas
   __driver_arena_start  = align_up(__kernel_code_end, 512)
   data_base             = align_down(DF00h - data_size, 512)
   __driver_arena_end    = data_base
   ```

`kernel/module/arena.asm` consumes the generated
`build/geometry/arena_geometry.inc`; `build/geometry/memory.inc` carries
the same values plus the granule count and data size for the layout tools.
No linked content may occupy the arena, the runtime data (`_DATA`,
`_INITIALIZED`) sits directly at `data_base` below the `DF00h` ISR stack,
and the loader computes its granule count from the linked bounds (bounded
by the architectural maximum `(DF00h-2000h)/512 = 95`).

The build fails when fewer than 32 granules (16 KiB) remain in production.
Acceptance and variant images that carry a target test hook relax only the
*minimum* check: `ZOS_ARENA_MIN_GRANULES := 16` when `TARGET_TEST_OBJS` is
non-empty (`tools/mapcheck.py --min-granules`).  Their safety net is
unchanged and independent of the floor: `tools/zmodpack.py plan` must fit
the whole boot set into the derived arena and `tools/pack_image.py`
reconciles the image manifest against that plan, so an image whose modules
do not fit still fails the build.  Production always keeps the 32-granule
gate.  See `docs/vfs.md` section 10 for the per-image measurements.

`tools/mapcheck.py` rejects any linked area overlapping the arena, pins
`__driver_arena_start == align_up(__kernel_code_end, 512)`, enforces the
minimum, and checks that `_DATA` starts at the derived base and that
runtime data stays below `DF00h`.  `tools/pack_image.py` treats the region
as non-ROM-loadable and reconciles the boot-set plan (per-module
image+BSS/granules, used/free totals; see `docs/module-format.md`).
`tests/test_m8.py` pins the whole reconciliation: the image manifest's
arena block must equal the plan, every per-module size/granule count must
match the packed container the plan names, the four production modules
must fit with a non-negative remainder, and the arena must end exactly at
the derived data base.  The loader/relocator routines are bank-7 code
enforced by an explicit list and a prefix sweep over `_module_`, `_zmod_`,
`_z4_`, `_m4_`, and `_irq_registry_` symbols.  Loaded modules are placed
inside the arena by the loader.

### 2.2.1 The bank-7 budget ledger

`tools/pack_image.py` and `tools/zmodpack.py plan` print the same single
ledger line and emit it as matching JSON (`manifest["bank7"]` and
`arena.plan.json["bank7"]`); `tests/test_m8.py` checks the line against
the linked map:

```text
bank7: code <bytes> end <addr> | arena <n> granules (<used> boot, <free> free) | data <bytes> at <addr> | isr 256 | slack <bytes>
```

Package 25b numbers (size-optimized kernel and modules, derived base):

```text
package-25b baseline (fixed C000h data base):
bank7: code 23521 bytes end 7BE1h | arena 34 granules (21 boot, 13 free) | data 1294 bytes at C000h | isr 256 | slack 6642 bytes

package 25b result:
bank7: code 21689 bytes end 74B9h | arena 49 granules (20 boot, 29 free) | data 1309 bytes at D800h | isr 256 | slack 483 bytes
```

Per-measure effect:

* size-optimized compile (`--opt-code-size --max-allocs-per-node 50000`)
  alone: kernel code `7BE1h -> 74B9h` (1832 bytes smaller), ROMFS recovery
  store `17890 -> 17548` bytes (the boot-set modules `ioc -231`, `hid -9`,
  `v9958 -89`, `sio +11`); with the fixed data base the arena would be
  `7600h-C000h`, 37 granules.
* correction-01 (`drivers/v9958/`: 8-glyph font chunks, 24-byte atlas
  segments): the V9958 module trades 30 image bytes for 264 staging BSS
  bytes (`4008/724 -> 4038/460`, `4732 -> 4498` total), so it drops from
  10 to 9 granules and the boot set from 21 to 20; the recovery store
  becomes `17578` bytes and the ledger's used/free split becomes
  `20 boot, 29 free`.  VRAM atlas contents are unchanged.
* derived data base: runtime data moves from `C000h` to `D800h`, so the
  arena end becomes `D800h` and the arena grows to 49 granules
  (+12 granules), leaving the runtime data `1309` bytes just below the
  `DF00h` ISR stack.

## 2.3 ROMFS placement

The ROMFS reader (`_romfs_init`, `_romfs_lookup_ci`, `_romfs_read`,
`_romfs_entry_at`) and the module-facing resource imports
(`_romfs_resource_size`, `_romfs_resource_read`) are bank-7 code;
mapcheck requires the `_romfs_` prefix sweep to stay below the ISR
stack.  The M9 kernel VFS and its statically linked ROMFS provider
(`_vfs_*` plus `_romfs_provider`) are bank-7 code and data too: the
generated `const` mount table is ROM-loaded bank-7 data
(`_vfs_mount_table`), `vfs_mount_state[]` is bank-7 runtime data
(`_DATA`, derived base-DFFFh), and nothing in the VFS is common-resident --
the boot, module-loader and (later) syscall paths all reach it from
bank 7.  VFS path, component and directory scratch is static bank-7
`_DATA` (`kernel/vfs/*.c`, `fs/romfs/romfs_provider.c`); no VFS
function declares an automatic object larger than 32 bytes, so no path
ever needs the 256-byte common gate stack.  The ROM-access primitive
(`_rom_access_copy`) is common code, and its staging buffer and saved
latch/IFF/request state are common data: they must all sit in
`E000h-FEFFh` and the staging size must equal `ZOS_ROMFS_STAGING_SIZE`
(128).  The fixed transition block is pinned too: `rom_access_trampoline`
at `ZOS_ROM_ACCESS_BASE - 2` (`EEFEh`), `rom_access_sync` at
`ZOS_ROM_ACCESS_BASE` (`EF00h`), and `rom_access_sync_end`
`ZOS_ROM_ACCESS_SIZE` (8) bytes later; the `_ROM_ACCESS_STUB` area must
match that extent and nothing else may overlap it.  The busy flag is
initialized by a `.db 0` byte because `.ds` reservations are `FFh` in
the ROM image.  The embedded boot catalog is retired (work package 25a):
every image boots its modules from the recovery store through the VFS, and
`--production` still rejects the catalog symbols outright so a
re-appearance cannot hide in a production map.

## 3. Common-resident routines

Every routine that changes the memory mapping, or that must run immediately
after a mapping change, is linked into `_COMMON`:

```text
_boot_common_entry     mode 10 -> 11 boot entry
__zos_crt0_init        C runtime zero/copy/GSINIT
__zos_mode_fault       latch assertion failure path
zos_enter_os_mode      mode 10 -> 11 gate (common continuation)
zos_leave_os_mode      mode 11 -> 10 gate
zos_os_continuation    post-transition common continuation
zos_syscall_entry      RST 30h syscall gate
irq_ctc0_stub          IM2 source stub (CTC0 vector 00h)
irq_ff_unexpected      IM2 vector FFh / default vector target
irq_common_entry       generic common IRQ gate (full context + mode cross)
_irq_arch_lock         IFF2 snapshot + DI (returns prior state in L)
_irq_arch_unlock       conditional EI restore
_irq_arch_init         IM2 page fill, I=FDh, IM2 select, EI
_platform_memctl_*     latch read/write primitives
_platform_bank_get/set selected-bank primitives
_early_console_*       early serial console
_panic                 fatal path
_rom_access_copy       M5 ROMFS access primitive (mode-00 window)
```

The bank-7 syscall boundary (`zos_syscall_dispatch`) and the C dispatcher
stay in the loadable kernel region; only the three-byte jump stub lives in
page zero.

The early console and panic paths are fully common-resident (code and
strings) so diagnostics survive mode/bank transitions.

## 4. Transition-critical state (`_COMMON_DATA`)

Single-depth by design (interrupts and nesting are out of scope at M1):

```text
zos_gate_saved_sp      2 bytes   caller SP (RST-pushed PC just below it)
zos_gate_saved_latch   1 byte    full caller latch (page/mode/bank)
zos_gate_result_hl     2 bytes   dispatcher result staging
zos_gate_result_a      1 byte    error code staging
zos_gate_result_c      1 byte    carry staging (0/1)
zos_gate_stack       256 bytes   common gate stack used while crossing modes
```

The gate stack carries the complete syscall handler chain.  It must fit
one module class operation that runs a full device transaction: the M8
console read path (`console_getc` -> HID character input -> IOC command
-> SIO transport) reaches roughly 180 bytes below the top, so the size
is held at 256 bytes.

M5 adds the ROM-access block in the same common data area:

```text
_rom_access_staging   128 bytes  bounded copy staging buffer
rom_access_saved_latch 1 byte    exact caller latch byte
rom_access_saved_iff   1 byte    exact IFF2/IFF1 state
rom_access_page/offset/length    validated request
rom_access_busy        1 byte    non-reentrancy flag (.db 0)
```

There is deliberately no transition stack: mode 00 reads ROM at every
address, so a stack could not survive the window.  The transition itself
is the fixed mirrored stub block `EEFEh-EF07h`:

```text
rom_access_trampoline  EEFEh     OUT (00h),A: enter mode 00
rom_access_sync        EF00h     LDIR (ROM -> bank-0 staging); EX AF,AF';
                                 OUT (00h),A: restore the exact latch
rom_access_sync_end    EF08h     stub extent mirrored into pages 2-7
```

The gate stack size is validated exactly (`zos_gate_stack_top - zos_gate_stack
== 256`, from the platform constant) and the whole block must sit inside
`E000h-FEFFh`.

Self-test builds only (`make syscall-selftest`) add a second small block
(`zos_selftest_kernel_sp`, `_zos_selftest_bank`, `_zos_selftest_result`)
in the same common data area.

## 4.1 M2 IRQ state, stacks, and vector page

Fixed absolute reservations (validated by `mapcheck.py` and the packer);
the vector-page, transition-state, and transition-stack bounds are
declared in `platform/zephyr80/irq.inc`:

```text
_IRQ_VECTOR       FD00h-FE00h   IM2 page plus the FFh-vector guard byte;
                                the image word at FDFFh/FE00h must equal the
                                linked irq_ff_unexpected address
_IRQ_COMMON_STATE FE01h-FE7Fh   irq_saved_latch, irq_saved_sp,
                                irq_common_sp, irq_active_source, and the
                                128-byte common transition stack FE80h-FEFFh
_IRQ_ISR_STACK    DF00h-DFFFh   bank-7 handler stack; initial SP=E000h
```

The IM2 page is initialized at runtime by `irq_arch_init()` (every vector
targets `irq_ff_unexpected`, vector `00h` targets `irq_ctc0_stub`); the
linked image carries the guard identity for build-time verification.
`_DATA`/`_INITIALIZED` must end below `DF00h` and
`_COMMON`/`_COMMON_DATA` below `FD00h`.

## 5. Banking primitives

Implemented in `kernel/memory/banking.asm` (assembly, common-resident):

```c
uint8_t platform_memctl_read(void);          /* raw latch value            */
void    platform_memctl_write(uint8_t value);/* raw latch write (gates only) */
uint8_t platform_bank_get(void);             /* D2..D0 bank field          */
void    platform_bank_set(uint8_t bank);     /* keep D7..D3; reject bank 7 */
```

`platform_bank_set()` preserves ROM-page and mode bits and rejects bank 7
(application banks are `0-6`).  Mode transitions are assembly gates with
explicit common-memory continuations, not C functions: a C return address
in `2000h-DFFFh` would disappear when the mode changes.

## 6. Validation

- `tools/pack_image.py` — byte-level image checks: exact 512 KiB size,
  FF fill, out-of-region rejection, copier mirroring, the ROM-access
  stub pattern/trampoline, stub mirror identity in pages 2-7, entry-point
  regions, and the `word_at(FDFFh) == irq_ff_unexpected` guard identity.
- `tools/mapcheck.py` — map-level checks: area ownership per this table,
  common-resident gate/console/panic/IRQ placement, gate-stack and both
  IRQ-stack sizes, ROM-access staging size plus trampoline/sync placement
  and stub-area extent, the IM2 page/guard/state addresses,
  `_irq_kernel_code_end` at the linked `_CODE` end, page-zero and
  stack-reservation bounds, and the M3 bank-7 device-layer placement
  rules (`make check`).
- `tests/maptools.py` — the shared map/constant helpers used by the
  layout and validation suites.  `tests/test_source_layout.py` — constant
  parity (hardware, syscall-gate and IRQ-executive header pairs), the
  single test seam, the owner layout, and the Mn-1 retention window.
- `tests/test_subsys_sim.py` — boot of the subsystem acceptance image on
  the current path: the syscall gate and interrupt-executive groups (with
  injected CTC0 deliveries), the ROMFS reader, the process/ZEX lifecycle
  and the loader groups, every target status word, and the monitor
  session with its relaunch.
- `tests/test_romfs.py` / `tests/test_romfs_sim.py` — ROMFS tooling,
  constant/placement contracts, corrupted-metadata rejection, the
  ROM-access transition replay (stackless mode-00 window pinned to the
  mirrored stub, exact latch/IFF restoration, bounds and reentry
  rejection), and the source-backed module load (`build-subsys-selftest`).
- `tests/test_m9_vfs.py` / `tests/test_m9_vfs_sim.py` — M9 VFS ABI sizes
  and header boundary, mount-table generation and validation, and a
  replay of the real linked VFS bootstrap (`/SYSTEM` mounted before any
  module loads, modules and the monitor opened by path, arena figures).
  `tools/mapcheck.py` additionally enforces the `_vfs_`/`_romfs_provider`
  bank-7 placement list and the derived-arena minimum, and
  `tools/pack_image.py` reconciles arena use against the boot-set plan.
- Runtime: boot asserts mode `11` over bank 0 (assembly latch read-back and
  a C re-check in `kernel_main()`), then exercises the bank primitives; any
  failure prints a fault through the early console and halts.

## 7. Constants are single-sourced

Every memory boundary and latch bit is defined once for C
(`platform/zephyr80/hardware.h`, `memory.h`, `irq.h`) and once for assembly
(`hardware.inc`, `memory.inc`, `irq.inc`), with explicit parity checks in
`tests/test_m1.py` so the representations cannot silently drift.  Hardware
ports and register constants stay in `hardware.h/.inc`; general region
geometry (including the bank-7 ISR stack) stays in `memory.h/.inc`; IM2
vector-page and IRQ transition wiring stays in `irq.h/.inc`.  The Zephyr-80
build include paths and the declarative file names live in
`platform/zephyr80/config.mk`; `tests/test_source_layout.py` enforces that
the platform tree contains no executable code.
