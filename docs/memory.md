# ZephyrOS Memory Model (M1, implemented)

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
| `2000h-BFFFh` | bank 7 in mode 11 | loadable kernel image (code, `_HOME`, `_INITIALIZER`, `_GSINIT`, `_GSFINAL`) |
| `C000h-DFFFh` | bank 7 in mode 11 | zero-initialized runtime area (`_DATA`, `_INITIALIZED`), never ROM-loaded |
| `E000h-FEFFh` | physical bank 0 | permanent common code/data (`_COMMON`, `_COMMON_DATA`) |
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
| `_CODE` | `2000h-BFFFh` | kernel C code and string literals |
| `_HOME` | `2000h-BFFFh` | static helpers |
| `_INITIALIZER` | `2000h-BFFFh` | ROM image of initialized data |
| `_GSINIT`/`_GSFINAL` | `2000h-BFFFh` | static initializers + terminator |
| `_DATA` | `C000h-DFFFh` | zeroed globals/statics |
| `_INITIALIZED` | `C000h-DFFFh` | RAM image of initialized data |
| `_COMMON` | `E000h-FEFFh` | boot entry, gates, console, panic, their strings |
| `_COMMON_DATA` | `E000h-FEFFh` | transition-critical M1 state (below) |
| (reserved) | `FF00h-FFFFh` | common boot/runtime stack; nothing may link here |

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

## 5. Banking primitives

Implemented in `platform/zephyr80/banking.asm` (assembly, common-resident):

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
  FF fill, out-of-region rejection, copier mirroring, entry-point regions.
- `tools/mapcheck.py` — map-level checks: area ownership per this table,
  common-resident gate/console/panic placement, gate-stack size, page-zero
  and stack-reservation bounds (`make check`).
- `tests/test_m1.py` — C/assembly constant parity, latch formulas,
  bank-identity preservation across `10h`/`18h`, bank-7 rejection,
  mapcheck unit cases, and banner regression on real artifacts.
- Runtime: boot asserts mode `11` over bank 0 (assembly latch read-back and
  a C re-check in `kernel_main()`), then exercises the bank primitives; any
  failure prints a fault through the early console and halts.

## 7. Constants are single-sourced

Every memory boundary and latch bit is defined once for C
(`platform/zephyr80/hardware.h`, `memory.inc`) and once for assembly
(`hardware.inc`), with an explicit parity check in `tests/test_m1.py` so the
two representations cannot silently drift.
