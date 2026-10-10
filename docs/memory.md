# ZephyrOS Memory Model (M9/M10, implemented)

This is the implemented, linker-enforced memory ownership table for the
mode `10`/`11` Zephyr-80 architecture.  The contract is in
`docs/architecture.md`; this file records what the build actually places
where, and how it is validated.

Since work package 38 the production nucleus is linked as **two regions**
under decoder revision 13 (architecture 36.5): a 16-KiB **fixed core**
(`2000h-5FFFh`) that every page selection shows unchanged, and a 32-KiB
**cold half** (`6000h-DFFFh`) that is mapped only while the window shows
bank 7 (`P = 0`).  `config/residency.json` is the classification that
decides which object belongs where; `tools/mapcheck.py` fails the build
when an object is missing from it or when a fixed-required object links at
or above `6000h`.

## 1. Kernel-visible regions

```text
FFFFh +-------------------------------+
      | bank 0 common         8 KiB    |  FF00h-FFFFh  common boot/runtime stack
FF00h +-------------------------------+
      | bank 0 common code/data        |
      |   _COMMON / _COMMON_DATA       |
E000h +-------------------------------+
      | bank 7 cold half     32 KiB    |  mode 11 view, mapped only when
      |   _WINDOW/_WINDOW_DATA         |  the window shows bank 7 (P = 0)
6000h +-------------------------------+
      | bank 7 fixed core    16 KiB    |  mode 11 view, always mapped:
      |   _CODE .. _DATA, stacks       |  fixed code, runtime data, the
      |                                |  service stack and the ISR stack
2000h +-------------------------------+
      | bank 0 (caller)       8 KiB    |
0000h +-------------------------------+
```

| Region | View | Content |
| --- | --- | --- |
| `0000h-00FFh` | caller window | ZephyrOS-owned native page zero: reset copier (`0000h-0026h`) and the three-byte `RST 30h` vector (`JP zos_syscall_entry`) at `0030h-0032h`; the rest is reserved |
| `0100h-1FFFh` | caller window | unused (FF-filled in the image); at M1, native syscall argument blocks must lie here |
| `2000h-fixed code end` | bank 7 in mode 11 | **fixed core**: loadable nucleus code (`_CODE`, `_HOME`, `_INITIALIZER`, `_GSINIT`, `_GSFINAL`) that must stay reachable whatever the window shows; ends at the derived fixed code end (`ZOS_FIXED_CODE_END`) |
| `fixed code end-5D00h` | bank 7 in mode 11 | fixed-core zero-initialized runtime area (`_DATA`, `_INITIALIZED`), never ROM-loaded; the base is derived per image (`align_down(ZOS_SERVICE_STACK_BASE - data size, 512)`) so the area ends directly below the stacks |
| `5D00h-5EFFh` | bank 7 in mode 11 | 512-byte **service stack** (architecture 36.9); the IPC substrate switches to it for every service call (stage 3).  It is a fixed assembly reservation, exported as the ABS symbol `zos_service_stack`/`_top` (`kernel/ipc/ipc_asm.asm`), so no ordinary allocation can enter it.  The acceptance replay measures its high-water mark: 256 of 512 bytes in the package-39 correction-01 image.  Grows down from `ZOS_SERVICE_STACK_TOP` |
| `5F00h-5FFFh` | bank 7 in mode 11 | 256-byte **ISR stack** (`_IRQ_ISR_STACK`); the IRQ gate switches to it in `irq_common_entry` |
| `6000h-cold code end` | bank 7 while the window shows bank 7 (P = 0) | **cold half**: nucleus code that only ever runs with the window on bank 7 (the syscall dispatcher and bodies, boot/idle, process management, the VFS core and the resident-service loader); ends at the derived `ZOS_KERNEL_CODE_END` |
| `cold code end-cold data base` | bank 7 while P = 0 | the cold half's slack, straight from the aligned cold code end to the derived cold data base.  The driver arena that used to fill it is deleted (work package 42c): there is no granule accounting and no floor |
| `cold data base-E000h` | bank 7 while P = 0 | cold-half zero-initialized runtime area (`_WINDOW_DATA`), never ROM-loaded; the base is derived per image from the linked cold code end (`data_base = align_up(code_end, 512)`) |
| `E000h-FCFFh` | physical bank 0 | ordinary permanent common code/data (`_COMMON`, `_COMMON_DATA`), including the M5 ROM-access staging buffer, the package-28 exec continuation state and the package-39 correction-01 aperture copy primitive (`ipc_common.asm`: the mode-10 stub, its request block, its saved latch/SP and its 4-byte interrupt-push reservation); `EEFEh-EF07h` is the fixed ROM-access transition block (trampoline + mirrored stub) |
| `FD00h-FDFFh` | physical bank 0 | 256-byte-aligned IM2 vector page (`I=FDh`) |
| `FE00h` | physical bank 0 | vector-`FFh` guard byte |
| `FE01h-FE7Fh` | physical bank 0 | common IRQ transition state |
| `FE80h-FEFFh` | physical bank 0 | 128-byte common IRQ transition stack |
| `FF00h-FFFFh` | physical bank 0 | common boot/runtime stack; `SP = 0000h` (top-exclusive `10000h`), grows down |

Mode `10` maps the selected application bank at `0000h-DFFFh`; mode `11`
keeps the caller bank at `0000h-1FFFh` and maps bank 7 at `2000h-DFFFh`.
Bank 7 is reserved to ZephyrOS and is never an application bank, and from
M10 on bank 6 is the **resident service domain** (architecture 36.6): the
process bank allocator hands out banks 0-5 only
(`PROCESS_BANK_COUNT`).  Bank 6 stays addressable - the mode-11 window
shows it at P = 1 and the diagnostic family reaches it - it is simply
never a process bank.  With the
page field at 0, this is exactly what decoder revision 13 does as well;
section 1.1 records the revision-13 mappings for a non-zero page field, and
sections 2.2/2.2.1 the two-region geometry those mappings allow.

### 1.1 Decoder revision 13 (programmed 2026-10-07)

The banking latch at port `00h` carries a page field in D5-D7 outside ROM
mode:

```text
D0-D2  bank field Q
D3-D4  mode
D5-D7  page field P (the ROM page in mode 00)
```

The board modification for revision 13 routes D5-D7 to three spare
decoder inputs (GAL pins 18/19/20, inputs only: the latch drives them and
the GAL never drives them).  Pin 21 (`!CART_CS`, no equation) is
unchanged.  **Revision 13 is programmed on the board** (2026-10-07); CP/M
and the production ZephyrOS ROM both boot on it unchanged.  The decoder
PLD and its WinCUPL simulation vectors live in the hardware project, not
in the OS tree; the OS keeps only the decoder model, which the simulator
uses as its default, and `tests/test_decoder_rev13.py` pins that model's
behaviour (the P = 0 equivalence with revision 12 across every mode,
region and bank, the mode-10 aperture rule and the mode-11 window).

Revision-13 mappings per mode:

| mode | `0000h-1FFFh` | `2000h-5FFFh` | `6000h-7FFFh` | `8000h-BFFFh` | `C000h-DFFFh` | `E000h-FFFFh` |
|---|---|---|---|---|---|---|
| 00 ROM | reads ROM page P; writes SRAM bank Q | ← | ← | ← | ← | ← |
| 01 flat | bank Q | bank Q | bank Q | bank Q | bank Q | bank Q |
| 10 application, P = 0 | bank Q | bank Q | bank Q | bank Q | bank Q | bank 0 (common) |
| 10 application, P ≠ 0 | bank Q | bank Q | bank Q | **bank 7 − P (far aperture)** | bank Q | bank 0 (common) |
| 11 OS | bank Q (caller window) | bank 7 (fixed nucleus) | **bank 7 − P (window)** | **bank 7 − P** | **bank 7 − P** | bank 0 (common) |

- **P = 0 is revision 12 in every mode.**  CP/M and any image that never
  sets the page field run unchanged; the production image sets no page
  bits outside mode 00, so it is byte-identical under revision 13 and the
  simulator's revision-13 default.
- The mode-11 window is `6000h-DFFFh` and shows bank 7 at P = 0, bank 6 at
  P = 1; the rest of the mode-11 view (`2000h-5FFFh`) is the permanently
  visible fixed nucleus.  The mode-10 far aperture `8000h-BFFFh` opens
  only when P ≠ 0.

The target use of the page field -- the bank-6 service window, map tokens
and the far aperture -- is architecture section 36.2.

### 1.2 ROM side: the 512 KiB programmed image

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
_CODE        = 2000h            fixed-core loadable code
_DATA        = <derived base>   fixed-core zero-initialized runtime area
_WINDOW      = 6000h            cold-half loadable code
_WINDOW_DATA = <derived base>   cold-half zero-initialized runtime area
_COMMON      = E000h            permanent common code/data
```

Both data bases are derived, not fixed: the two-pass build computes
`data_base = align_down(ZOS_SERVICE_STACK_BASE - (l__DATA + l__INITIALIZED),
512)` and `window_data_base = align_up(code_end, 512)` (work package 42c:
the cold base follows the linked cold code, so no reservation sits between
them), so runtime data sits directly below the fixed-core stacks and
directly after the cold half's code.

Unbased areas follow their preceding based area in first-appearance order,
fixed by the declarations at the top of `boot/start.asm`:

```text
_CODE (2000h) -> _HOME -> _INITIALIZER -> _GSINIT -> _GSFINAL   fixed loadable
_DATA (derived base) -> _INITIALIZED                            fixed runtime
_WINDOW (6000h) -> _WINDOW_DATA (derived base)                  cold
_COMMON (E000h) -> _COMMON_DATA                                 common
```

Every owned region exports `s__<area>`/`l__<area>` start/size symbols
(linker-synthesized); the startup initializer/BSS operations consume
`s__INITIALIZER`/`l__INITIALIZER`, `s__INITIALIZED`, `s__DATA`/`l__DATA`,
and `s__WINDOW_DATA`/`l__WINDOW_DATA` (`boot/start.asm`'s
`__zos_crt0_init` zeroes both runtime areas: `_DATA` for the fixed core and
`_WINDOW_DATA` for the cold half).

| Area | Region | Holds |
| --- | --- | --- |
| `_RESET` | `0000h-0026h` | stackless reset copier (`.org 0`, ABS) |
| `_PAGE_ZERO` | `0030h-0032h` | `RST 30h` vector: `JP zos_syscall_entry` (ABS) |
| `_CODE` | `2000h-fixed code end` | fixed-core kernel C code, string literals, the copy gates' dispatch boundary, the IRQ executive and the upcall shim |
| `_HOME` | `2000h-fixed code end` | fixed-core static helpers |
| `_INITIALIZER` | `2000h-fixed code end` | ROM image of initialized data |
| `_GSINIT`/`_GSFINAL` | `2000h-fixed code end` | static initializers + terminator |
| `_DATA` | `derived base-5D00h` | fixed-core zeroed globals/statics (IRQ registry, shim state) |
| `_INITIALIZED` | `derived base-5D00h` | RAM image of initialized data (fixed-core ops/descriptor tables) |
| `_WINDOW` | `6000h-cold code end` | cold-half kernel code: syscall dispatcher and bodies, boot/idle, process/ZEX, loader, device registry, FS front end (`--codeseg WINDOW`) |
| `_WINDOW_DATA` | `cold data base-E000h` | cold-half zeroed globals (`--dataseg WINDOW_DATA`) |
| `_COMMON` | `E000h-FCFFh` | boot entry, gates, IRQ stubs, console, panic, copy primitives |
| `_COMMON_DATA` | `E000h-FCFFh` | transition-critical state |
| `_IRQ_ISR_STACK` | `5F00h-5FFFh` | fixed-core bank-7 handler stack |
| `_IRQ_VECTOR` | `FD00h-FE00h` | IM2 page plus guard |
| `_IRQ_COMMON_STATE` | `FE01h-FEFFh` | IRQ state plus transition stack |
| (reserved) | `FF00h-FFFFh` | common boot/runtime stack; nothing may link here |

## 2.1 M3 device-layer placement

The M3 device registry, console service, static driver descriptors, ops
tables, and driver-private state are all bank-7 residents: code in `_CODE`
or `_WINDOW`, state in `_DATA`/`_INITIALIZED` or `_WINDOW_DATA`, and never
above the fixed-core stacks.
Nothing from the device layer enters `_COMMON`/`_COMMON_DATA`; only the
M2 source stubs, the common IRQ crossing machinery, and the early
console/panic path are common-resident.  `tools/mapcheck.py` enforces
this both by explicit symbol list and by a prefix sweep over
`_device_`, `_console_`, `_sio0b_`, `_sio_`, `_driver_`,
`_boot_drivers`, and `_m3_` symbols.  See `docs/devices.md`.

Cold-half code that has to call a service (the stage 3 loader streaming an
executable object, the process-exit notification) goes through the fixed
**upcall shim** (`kernel/memory/upcall.asm`, architecture 36.5): a request
block (`page`, `entry`, `args`, `result`) is validated (page 0-1, entry at
or above `ZOS_REV13_WINDOW_BASE`), the latch is saved, the requested page
selected, the entry called and the latch restored exactly.  The shim links
in the fixed core (`_CODE`/`_DATA`) and nothing in production calls it yet;
for production images `tools/pack_image.py --production` allows the only
banked latch writes inside the fixed core within the shim's own
`_zos_upcall`..`zos_upcall_end` extent (the self-test caller blob, which
runs in an application bank, is the one further allowance), and
`tests/test_nucleus_sim.py` exercises the shim against a fixed-core test
routine.

## 2.2 Bank-7 geometry: two regions, both derived from the code ends (M10)

There is one bank-7 geometry, and it is the same two-pass mechanism as
package 25b:

1. **Probe link.**  The same object list links with a bootstrap
   `kernel/memory/bank7_end.asm` object that only carries the
   `__kernel_code_end` label; nothing can constrain kernel growth.
2. **Derive.**  `tools/mapcheck.py geometry derive` measures, in the probe
   map:

   - the exclusive end of the fixed loadable areas (`_CODE`, `_HOME`,
     `_INITIALIZER`, `_GSINIT`, `_GSFINAL`) and the fixed runtime-data
     size (`l__DATA + l__INITIALIZED`);
   - the exclusive end of the cold loadable area (`_WINDOW`) and the cold
     runtime-data size (`l__WINDOW_DATA`);

   and writes both regions:

   ```text
   fixed_code_end    = max end of the fixed loadable areas
   data_base         = align_down(ZOS_SERVICE_STACK_BASE - data_size, 512)
   __kernel_code_end = end of _WINDOW (the cold code end)
   window_data_base  = align_up(__kernel_code_end, 512)
   ```

   It also computes the fixed-core ledger figures (required/overflow
   bytes from `config/residency.json` plus the per-object `.rel` sizes,
   the two stack reservations, and the room).

   The cold data base derives **straight from the cold code end** (work
   package 42c): package 38's 24-granule arena reservation between the two
   is deleted, so the cold half is one contiguous region from `6000h` to
   its data base.  `__driver_arena_start`/`__driver_arena_end` and the
   `GEOMETRY_*` arena derivation are gone with it.

`kernel/memory/bank7_end.asm` consumes the generated
`build/geometry/bank7_end.inc`; `build/geometry/memory.inc` carries both
data bases, the ledger fields and the stack reservations for every
assembly unit and layout tool.

**Placement rule (architecture 36.5).**  `config/residency.json`
classifies every production object as `fixed-required` or `cold` (the M9
`moves` stage is retired with the module form).  Fixed-required objects
link in `2000h-5FFFh`; `tools/mapcheck.py` fails the build when one of
them compiles into `_WINDOW`/`_WINDOW_DATA`, when an object is missing
from the classification, or when an object's areas disagree with its
cold-half placement.  The production cold half holds the cold objects
plus the objects that do not fit in the fixed core; those are reported as
the fixed ledger's `overflow`, never as an error.

`tools/mapcheck.py` pins `__kernel_code_end ==` the linked cold code end
and `window_data_base == align_up(__kernel_code_end, 512)`, checks that
`_DATA` and `_WINDOW_DATA` start at their derived bases, that fixed
runtime data stops below the service stack and that the fixed core holds
its two stacks.  The bank-6 service set is not linked into bank 7 at all:
it is packed into the recovery store and placed by the cold nucleus loader
(`docs/module-format.md`), so no bank-7 geometry depends on it.

### 2.2.1 The nucleus budget ledgers

`tools/pack_image.py` and `tools/zmodpack.py services` print the same five
ledger lines (architecture 36.20) and emit them as matching JSON
(`manifest["nucleus"]` and `modules/services.plan.json["nucleus"]`,
sharing one format definition); host tests check every figure against the
linked map:

```text
nucleus fixed: required <bytes> | overflow <bytes> | stacks <bytes> | room <bytes>
nucleus cold: code <bytes> end <addr> | data <bytes> at <addr> | room <bytes>
driver hot: <bytes> of <capacity>
driver boot/bulk: <bytes> of <capacity>
common: used <bytes> of <capacity> bytes (code <n>, data <n>) | ceiling <n>, m10 <n>, limit <n>
```

- **`nucleus fixed:`** the fixed core's 16 KiB.  `required` is the linked
  fixed-area content of the `fixed-required` objects, `overflow` is
  everything else that links there (the objects that do not fit in the cold
  half), `stacks` is the two reservations (512-byte service stack plus
  256-byte ISR stack = 768) and `room` is what is left:
  `16384 - required - overflow - stacks`.
- **`nucleus cold:`** the cold half's 32 KiB.  `code`/`end` are the linked
  cold code, `data` is `_WINDOW_DATA` (`base` is derived from `end`) and
  `room` is what is left (`32768 - code - data bytes`, the alignment gap
  included in the room).
- **`driver hot:`/`driver boot/bulk:`** the bank-6 tiers of architecture
  36.6.  `tools/pack_image.py` prints them from the services plan
  (`services.plan.json`), and the loader's own acceptance line prints the
  same figures (32 KiB hot tier, 24 KiB boot/bulk tier).
- **`common:`** the permanently mapped common window, which the nucleus
  lines cannot see.  `used` is the packed `_COMMON` + `_COMMON_DATA` run
  and `capacity` is `ZOS_COMMON_CODE_END - ZOS_COMMON_BASE` (7424 bytes);
  `code`/`data` are the two area sizes.

The fixed absolute reservations in the window (IM2 vector page, IRQ
transition state and stacks) are declared separately in
`platform/zephyr80/irq.inc` and are not part of the packed run, so growth of
the packed run is what must be watched: it may not reach the lowest
reservation.

The `common:` figures are the pinned common-memory budget (work package
30c, from the 30b audit): `ceiling` is `ZOS_COMMON_CEILING` (3072) and
`m10` is `ZOS_COMMON_M10_RESERVATION` (512), held back for M10's far-call
gate and per-process far state; `limit` is `ceiling - m10` (2560) and
`used` may not exceed it.  The first M10 tenant of that reservation is the
correction-01 aperture copy primitive: 44 bytes of common code and 14
bytes of common data (the request block, the saved latch and SP, and the
4-byte reservation an interrupt taken inside its mode-10 window pushes
onto).  It is common and not fixed-core because mode 10 does not map bank
7: the stub's instructions, every byte it reads or pushes, and the
interrupt entry that may run inside its window all have to be permanently
mapped.  `mapcheck.py` and the packer both fail the
build over the limit, and neither value may be raised to make a package
fit (target architecture section 2.18).  Common memory is the scarcest
region: package 28 added 167 bytes of continuation state and correction-01
moved 128 bytes of path buffer back out to bank 7.

### 2.2.2 M9-era measurements (packages 25b-31)

The figures in this subsection were measured under the M9 single-region
layout and are kept as the historical ledger; package 38 re-derived the
same content as the nucleus lines above, and work package 42c deleted the
arena the later figures still mention (the package-38 validation record
has the M10-era ledgers before that prune).

The tools print the ledger lines and carry the same figures in the JSON.
There is no granule table any more (work package 42c): the packer places
each bank-6 segment and the loader bounds it.  The image manifest's
`kernel` object still gives the two distances the old plan used for the
cold code end and the cold runtime-data size:
`code_above_lower_bytes`/`code_before_higher_bytes` and
`data_above_lower_bytes`/`data_before_higher_bytes`.  The data pair is
expressed as runtime-data size changes - shrinking by the first moves the
data base up one granule, growing by the second moves it down one.  A saving smaller than `above_lower_bytes`
frees no granule; `tests/test_pack_image.py` checks the arithmetic against
the linked map and the packed containers.

Package 25b numbers (size-optimized kernel and modules, derived base; the
figures below predate package 38 and are quoted in the M9 `bank7:` ledger
wording - the region they describe is the single M9 bank-7 region that
package 38 split into the fixed core and the cold half):

```text
package-25b baseline (fixed C000h data base):
bank7: code 23521 bytes end 7BE1h | arena 34 granules (21 boot, 13 free) | data 1294 bytes at C000h | isr 256 | slack 6642 bytes

package 25b result:
bank7: code 21689 bytes end 74B9h | arena 49 granules (20 boot, 29 free) | data 1309 bytes at D800h | isr 256 | slack 483 bytes

package 26 result (driver ABI v3, provider registry transaction, kernel
file API seek/root merge and the mountpoint error rules):
bank7: code 23526 bytes end 7BE6h | arena 46 granules (20 boot, 26 free) | data 1501 bytes at D800h | isr 256 | slack 291 bytes
```

Package 26 effect (measured per object against the package-25b build):

| object | `_CODE` | `_DATA` | delta | note |
| --- | ---: | ---: | ---: | --- |
| `vfs_file.rel` | 1536 -> 2321 | 0 | +785 | file position, `vfs_seek`, write-commit ordering, close-sync |
| `vfs_dir.rel` | 1042 -> 1568 | 0 | +526 | root merge, mountpoint shadowing, `/`-dir handle |
| `vfs_mount.rel` | 1752 -> 2164 | 169 -> 357 | +412/+188 | provider registry transaction, required-mount diagnostic buffer |
| `module_loader.rel` | 1419 -> 1484 | 373 | +65 | `fs_register` wrapper plus the provider checkpoint/rollback |
| `process_exec.rel` | 1549 -> 1598 | 0 | +49 | `vfs_seek` + positioned reads |
| `main.rel` | 1649 | 139 -> 143 | 0/+4 | TEST-ONLY boot-module hook (production compiles it out) |
| `vfs_mutate.rel` | unchanged | unchanged | 0 | mountpoint/root error constants only |

Kernel C lines added (all changed kernel `.c` files): 304 lines, +1837
`_CODE` bytes and +192 `_DATA` bytes, about 6.7 bytes per line; the
largest single addition is `vfs_seek` at 174 bytes.  The arena shrinks
`49 -> 46` granules and slack `483 -> 291` bytes, so the split of the
remaining end-of-M9 slack is tight on the section 32.39 ledger; the
boot-set arena plan still held (package 38 retired the 32-granule floor,
not the plan check).

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

## 2.3 M9 runtime data (packages 27-31)

The M9 VFS, the exec machinery and the resident IOC-FAT provider keep their
state in static storage; nothing in M9 allocates.  The bank-7 runtime-data
block (`_DATA`/`_INITIALIZED`, whose base is derived from its own size per
section 2.2) holds:

| item | bytes | owner |
| --- | ---: | --- |
| global descriptor pool, 8 entries x 17 (`owner`, `type`, file/dir union) | 136 | `kernel/vfs/process.c` |
| CWD pool: 4 chain levels x (1 owner byte + 128-byte canonical path) | 516 | `kernel/vfs/process.c` |
| VFS workspace: two 256-byte path views and the 512-byte transfer staging view, exactly tiling one buffer | 512 | `kernel/vfs/workspace.c` |
| VFS path scratch, kernel file table and the SYS_EXEC path buffer | (see the map) | `kernel/vfs/*.c`, `kernel/main.c` |

The exec continuation state is **common**, not bank-7 data, because it is
touched only while the gate runs in mode 11: `zos_proc_frames` is
`ZOS_EXEC_MAX_DEPTH - 1 = 3` save areas of `ZOS_EXEC_FRAME_SAVE` (48)
bytes, i.e. 144 bytes, plus the single-depth gate shadow bytes
(`include/zephyros/process.h`, `docs/abi.md` section 11).

The IOC-FAT provider is a resident ZMOD, so its state is that module's own
BSS, not kernel data: a 637-byte BSS of which the one 512-byte file-data
cache slot is the largest member (`fs/iocfat/iocfat.c`,
`IOCFAT_CACHE_SLOTS`).

Final ledger (end of M9, packages 30c and 31; production image, M9 wording -
package 38 re-derived this content into the five nucleus lines of section
2.2.1 without changing any byte):

```text
bank7: code 27990 bytes end 8D56h | arena 36 granules (33 boot, 3 free) | data 2241 bytes at D600h | isr 256 | slack 63 bytes
common: used 2104 of 7424 bytes (code 1410, data 694) | ceiling 3072, m10 512, limit 2560

boot set: sio x1, ioc x8, hid x2, v9958 x9, iocfat x13 = 33 granules
total slack: 3 x 512 + 63 = 1599 bytes (32.39 target: >= 1 KiB)
```

No M9 package after 30 changes a kernel or module byte except the 30c
reclaim, which moved the arena start one granule down (`9000h -> 8E00h`)
with the data base and the data slack unchanged.  The `m9-selftest`
integration image compiles HID/V9958 out and adds the acceptance object, so
its own ledger (`code 29855 bytes end 949Fh | arena 30 granules (22 boot, 8
free) | data 3306 bytes at D200h | slack 22`) is not a production figure.

## 2.4 ROMFS placement
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
_zos_process_launch    process entry gate (depth-indexed continuation)
_zos_process_resume    parent resume gate (never returns to its caller)
_zos_process_continuation_size  gate-stack bytes in use (SYS_EXEC preflight)
zos_process_return_trap  raw entry RET -> ZOS_EFAULT teardown
```

The bank-7 syscall boundary (`zos_syscall_dispatch`) and the C dispatcher
stay in the loadable kernel region; only the three-byte jump stub lives in
page zero.

The early console and panic paths are fully common-resident (code and
strings) so diagnostics survive mode/bank transitions.

## 4. Transition-critical state (`_COMMON_DATA`)

Single-depth by design (interrupts and nesting were out of scope at M1; a
package-28 exec shadows the process side per depth, below):

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

Package 28 makes the *process* side of that state depth-indexed, because
a nested `SYS_EXEC` runs its child on the same single gate stack:

```text
zos_proc_cont          8 bytes   launcher SP per depth (2 bytes x 4)
zos_proc_ix            8 bytes   launcher IX (SDCC frame pointer) per depth
zos_proc_latch         4 bytes   launcher latch per depth
zos_proc_iff           4 bytes   launcher IFF2 per depth
zos_proc_gate_sp       8 bytes   shadowed gate caller SP per depth
zos_proc_gate_latch    4 bytes   shadowed gate caller latch per depth
zos_proc_gate_iff      4 bytes   shadowed gate caller IFF per depth
zos_proc_frames      144 bytes   3 x 48-byte saved gate frames
```

The `SYS_EXEC` path buffer is *not* here: it is ordinary bank-7 `_DATA`
(`_zos_process_path_scratch` in `kernel/process/process.c`, ZOS_PATH_MAX
bytes), because nothing touches it across a mode change.

`zos_gate_saved_sp/latch/iff` stay single-depth (they are what the gate
saves and restores for one syscall); the `zos_proc_gate_*` copies are the
per-depth shadow that lets a resumed parent return through the gate with
*its* stack pointer, bank and interrupt state instead of its child's.
`ZOS_EXEC_FRAME_SAVE` is 48 bytes: the measured copied frame is 32 bytes
at every depth in the acceptance image (recorded beside the constant in
`include/zephyros/process.h`), and the preflight rejects a shallower
handler frame above `ZOS_EXEC_FRAME_SAVE - ZOS_EXEC_FRAME_SLACK` before
anything is allocated.

Everything above is common-resident because the resume gate *restores* it
while it is changing the mapping: it runs in mode 11 over the child's
bank with the child's stack still live, and it needs the launcher's saved
SP, latch, IFF, IX and gate frame before it can switch back - none of
which could be reached from bank 7 if the last `LDIR` and the latch `OUT`
had already moved off it.  Common memory is not protected memory: mode 10
maps it exactly as mode 11 does, and every application can read and write
it.  Placement here is a reachability and budget decision, never a
protection one.

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
_IRQ_ISR_STACK    5F00h-5FFFh   fixed-core handler stack; initial SP=6000h
```

The IM2 page is initialized at runtime by `irq_arch_init()` (every vector
targets `irq_ff_unexpected`, vector `00h` targets `irq_ctc0_stub`); the
linked image carries the guard identity for build-time verification.
`_DATA`/`_INITIALIZED` must end below `ZOS_SERVICE_STACK_BASE` (5D00h)
and `_COMMON`/`_COMMON_DATA` below `FD00h`.

## 5. Banking primitives

Implemented in `kernel/memory/banking.asm` (assembly, common-resident):

```c
uint8_t platform_memctl_read(void);          /* raw latch value            */
void    platform_memctl_write(uint8_t value);/* raw latch write (gates only) */
uint8_t platform_bank_get(void);             /* D2..D0 bank field          */
void    platform_bank_set(uint8_t bank);     /* keep D7..D3; reject bank 7 */
```

`platform_bank_set()` preserves ROM-page and mode bits and rejects bank 7
(application banks are `0-6`; `PROCESS_BANK_COUNT` hands the process
allocator banks 0-5, since bank 6 is the resident service domain from M10
on).  Mode transitions are assembly gates with
explicit common-memory continuations, not C functions: a C return address
in `2000h-DFFFh` would disappear when the mode changes.

## 6. Validation

- `tools/pack_image.py` — byte-level image checks: exact 512 KiB size,
  FF fill, out-of-region rejection, copier mirroring, the ROM-access
  stub pattern/trampoline, stub mirror identity in pages 2-7, entry-point
  regions, and the `word_at(FDFFh) == irq_ff_unexpected` guard identity.
- `tools/mapcheck.py` — map-level checks: area ownership per this table,
  both derived bases and the derived geometry, the residency classification
  (`--residency config/residency.json`: every production object classified,
  every `fixed-required` object below `ZOS_REV13_WINDOW_BASE`, the boot set
  fitting the derived arena), common-resident gate/console/panic/IRQ
  placement, gate-stack and both stack sizes, ROM-access staging size plus
  trampoline/sync placement and stub-area extent, the IM2 page/guard/state
  addresses, `_irq_kernel_code_end` at the linked cold code end, page-zero
  and stack-reservation bounds, and the M3 bank-7 device-layer placement
  rules (`make check`).
- `tests/maptools.py` — the shared map/constant helpers used by the
  layout and validation suites.  `tests/test_source_layout.py` — constant
  parity (hardware, syscall-gate and IRQ-executive header pairs), the
  single test seam, the owner layout, and the Mn-1 retention window.
- `tools/mapcheck.py`'s residency model also carries the `test_link` list
  (work package 39, reused by 42a): objects the production linker script
  does not list yet, whose placement is nevertheless enforced exactly as
  production will compile them in the stage that adopts them.  A
  test-linked `cold` object is expected in the cold areas; a test-linked
  `fixed-required` object is held below `ZOS_REV13_WINDOW_BASE` like any
  other.  `cold_half` may not name a `fixed-required` object, because the
  list drives the image's cold codegen flags.  Package 41 linked the IPC
  substrate, so since then the list names the namespace objects
  (`vfs_ns`, `vfs_ns_file`, `object`; work package 42a), which the
  `namespace-selftest` image reaches through its own `--cold-extra` and
  production adopts in 42b - the production ledgers of this package are
  therefore unchanged.
- `tests/test_ipc_selftest.py` / `tests/test_ipc_selftest_sim.py` — the
  IPC substrate acceptance image: the pinned ABI, the residency
  classification and the client's caller-window layout on the host, and a
  replay that measures per-call T-states, the domain latch intervals (every
  one closed on the latch it found, with the caller's latch restored at the
  gate), the two CTC0 deliveries into the service domain, the reserved
  stack high-water marks (service `5E08h`, ISR `5FF4h`, gate `E5FAh`), the
  window LDIRs and the lookup census.
- `tests/test_ns_selftest.py` / `tests/test_ns_selftest_sim.py` — the
  namespace acceptance image (work package 42a): the pinned ABI, the
  packed publication tables, the residency classification and the
  service-manifest wiring on the host, and a replay that compares the
  client's transcript (standard handles, `/dev` binds and calls, a
  `/SYSTEM` FILE read/seek/metadata, table exhaustion, paged `ns_enum`,
  an exec'd child) with the packed store.
- `tests/test_subsys_sim.py` — boot of the subsystem acceptance image on
  the current path: the syscall gate and interrupt-executive groups (with
  injected CTC0 deliveries), the ROMFS reader and the process/ZEX
  lifecycle groups, every target status word, and the monitor session with
  its relaunch.
- `tests/test_romfs.py` / `tests/test_romfs_sim.py` — ROMFS tooling,
  constant/placement contracts, corrupted-metadata rejection, the
  ROM-access transition replay (stackless mode-00 window pinned to the
  mirrored stub, exact latch/IFF restoration, bounds and reentry
  rejection), and the source-backed module load (`build-subsys-selftest`).
- (retired) `tests/test_m9_vfs.py` / `tests/test_m9_vfs_sim.py` — the M9
  VFS ABI sizes, the mount-table generation and a replay of the M9
  bootstrap; deleted with the machinery they covered (M10, work packages
  42b/42c).
- `tools/mapcheck.py` enforces the two-tier residency classification and
  the derived geometry (fixed core, cold code end, cold data base), and
  `tools/pack_image.py` reports the two bank-6 ledgers from
  `services.plan.json` instead of an arena plan (there is no granule
  floor any more: the packer places and the loader bounds).
- Runtime: boot asserts mode `11` over bank 0 (assembly latch read-back and
  a C re-check in `kernel_main()`), then exercises the bank primitives; any
  failure prints a fault through the early console and halts.

## 7. Constants are single-sourced

Every memory boundary and latch bit is defined once for C
(`platform/zephyr80/hardware.h`, `memory.h`, `irq.h`) and once for assembly
(`hardware.inc`, `memory.inc`, `irq.inc`), with explicit parity checks in
`tests/test_m1.py` so the representations cannot silently drift.  Hardware
ports and register constants stay in `hardware.h/.inc`; general region
geometry (including the fixed-core ISR and service stacks) stays in
`memory.h/.inc`; IM2
vector-page and IRQ transition wiring stays in `irq.h/.inc`.  The Zephyr-80
build include paths and the declarative file names live in
`platform/zephyr80/config.mk`; `tests/test_source_layout.py` enforces that
the platform tree contains no executable code.
