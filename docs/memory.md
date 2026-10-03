# ZephyrOS Memory Model (M3, implemented)

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
| `2000h-9FFFh` | bank 7 in mode 11 | loadable kernel image (code, `_HOME`, `_INITIALIZER`, `_GSINIT`, `_GSFINAL`) |
| `A000h-BFFFh` | bank 7 in mode 11 | M4 driver arena: 16 x 512-byte granules reserved for loaded modules; never part of the ROM payload and never linked by the kernel |
| `C000h-DEFFh` | bank 7 in mode 11 | zero-initialized runtime area (`_DATA`, `_INITIALIZED`), never ROM-loaded |
| `DF00h-DFFFh` | bank 7 in mode 11 | 256-byte M2 ISR stack; initial `SP=E000h` |
| `E000h-FCFFh` | physical bank 0 | ordinary permanent common code/data (`_COMMON`, `_COMMON_DATA`) |
| `FD00h-FDFFh` | physical bank 0 | 256-byte-aligned IM2 vector page (`I=FDh`) |
| `FE00h` | physical bank 0 | vector-`FFh` guard byte |
| `FE01h-FE7Fh` | physical bank 0 | common IRQ transition state |
| `FE80h-FEFFh` | physical bank 0 | 128-byte common IRQ transition stack |
| `FF00h-FFFFh` | physical bank 0 | common boot/runtime stack; `SP = 0000h` (top-exclusive `10000h`), grows down |

Mode `10` maps the selected application bank at `0000h-DFFFh`; mode `11`
keeps the caller bank at `0000h-1FFFh` and maps bank 7 at `2000h-DFFFh`.
Bank 7 is reserved to ZephyrOS and is never an application bank.

## 2. Linker area ownership

The linker script (`linker/zephyr80.lk`) assigns:

```text
_CODE   = 2000h    bank-7 loadable code
_DATA   = C000h    bank-7 zero-initialized runtime area
_COMMON = E000h    permanent common code/data
```

Unbased areas follow their preceding based area in first-appearance order,
fixed by the declarations at the top of `boot/start.asm`:

```text
_CODE (2000h) -> _HOME -> _INITIALIZER -> _GSINIT -> _GSFINAL   loadable
_DATA (C000h) -> _INITIALIZED                                   runtime
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
| `_CODE` | `2000h-9FFFh` | kernel C code, driver/service code, string literals |
| `_HOME` | `2000h-9FFFh` | static helpers |
| `_INITIALIZER` | `2000h-9FFFh` | ROM image of initialized data |
| `_GSINIT`/`_GSFINAL` | `2000h-9FFFh` | static initializers + terminator |
| `_DATA` | `C000h-DEFFh` | zeroed globals/statics, registry table, driver state |
| `_INITIALIZED` | `C000h-DEFFh` | RAM image of initialized data (ops/descriptor tables) |
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

## 2.2 M4 driver-arena placement

The `A000h-BFFFh` driver arena is reserved between the loadable kernel
image and the runtime data area: 16 contiguous 512-byte granules (8192
bytes) that no kernel section, stack, initialized data, or ROM payload
may occupy.  `platform/zephyr80/memory.inc` declares the geometry;
`kernel/module/arena.asm` exports `__driver_arena_start`/`__driver_arena_end`;
`tools/mapcheck.py` rejects any linked area overlapping the arena and
validates the exported bounds, and `tools/pack_image.py` treats the
region as non-ROM-loadable.  `tools/mapcheck.py` additionally enforces
bank-7 placement for the loader/relocator routines and a prefix sweep
over `_module_`, `_zmod_`, `_z4_`, `_m4_`, and `_irq_registry_` symbols.
Loaded modules are placed inside the arena by the M4 loader (see
`docs/module-format.md`).

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
zos_gate_stack        64 bytes   common gate stack used while crossing modes
```

The gate stack size is validated exactly (`zos_gate_stack_top - zos_gate_stack
== 64`) and the whole block must sit inside `E000h-FEFFh`.

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

- `tools/pack_image.py` — byte-level image checks: exact 128 KiB size,
  FF fill, out-of-region rejection, copier mirroring, entry-point regions,
  and the `word_at(FDFFh) == irq_ff_unexpected` guard identity.
- `tools/mapcheck.py` — map-level checks: area ownership per this table,
  common-resident gate/console/panic/IRQ placement, gate-stack and both
  IRQ-stack sizes, the IM2 page/guard/state addresses, `_irq_kernel_code_end`
  at the linked `_CODE` end, page-zero and stack-reservation bounds, and
  the M3 bank-7 device-layer placement rules (`make check`).
- `tests/test_m1.py` — C/assembly constant parity (hardware, syscall
  self-test, and IRQ self-test header pairs), latch formulas, bank-identity
  preservation across `10h`/`18h`, bank-7 rejection, mapcheck unit cases
  (including negative IRQ fixtures), banner regression, and FF-guard
  identity on real artifacts.
- `tests/test_irq_sim.py` — Z80 replay of the real linked IRQ entry/return
  bytes (IM2 acceptance, alternate registers, `I`/IFF2, `RETI`) for both
  modes and the FF guard path, plus the full `irq-selftest` driver round
  trip with injected CTC0 deliveries.
- `tests/test_m3.py` — M3 constant contract, mapcheck bank-7 M3 rules,
  fallback-image artifacts, and static linker-file/object consistency.
- `tests/test_m3_sim.py` — Z80 replay of the real M3 code: registry
  semantics, checkpoint/rollback, exhaustion, ABI rejection, console
  TX/RX through the SIO port model, the CTC0 probe through the M2
  executive, and a production `kernel_main` boot replay.
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
