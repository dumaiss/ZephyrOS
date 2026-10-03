# ZephyrOS Architecture

**Status:** M0-M6 complete; M7-M8 architecture defined (M4 dynamic-driver acceptance hardware-validated 2026-10-02; M5/M6 physically accepted 2026-10-03)  
**Scope:** Milestones M0 through M8  
**Target:** Zephyr-80  
**Language/toolchain:** SDCC + sdasz80/sdldz80, GNU Make, Python host tools

---

## 1. Purpose

ZephyrOS is the native operating system for the Zephyr-80.

The initial milestones intentionally build the native operating system from the machine outward. CP/M compatibility is not part of native bring-up; it is a later compatibility personality layered over a complete and useful native ZephyrOS environment.

The milestones covered by this document are:

- **M0 — Kernel bring-up:** boot ZephyrOS, initialize the early serial console, print a banner, and halt. **Complete.**
- **M1 — Kernel foundation:** establish the kernel memory model, page-zero ownership, common kernel entry points, and the initial native ABI scaffolding. **Complete.**
- **M2 — Interrupt executive:** establish kernel-owned IM2, safe mode-crossing IRQ entry/return, source ownership, and the first hardware interrupt. **Complete.**
- **M3 — Device registry and static drivers:** establish the driver/device model and prove it with statically linked hardware drivers. **Complete.**
- **M4 — Dynamic driver/module loading:** load relocatable driver modules into a managed bank-7 driver arena and register their devices at boot. **Complete.**
- **M5 — ROM recovery filesystem:** provide a tiny read-only ROM-backed recovery store for boot modules and native recovery programs. **Complete.**
- **M6 — Native process and ZEX execution:** establish single-tasking native process lifecycle and execute a single-bank native program from ROMFS. **Complete.**
- **M7 — Native monitor / diagnostic foundation:** port the Zephyr monitor as the first useful native ZEX and establish the foundation for the later diagnostic suite.
- **M8 — Native device stack completion:** provide the normal V9958/HID console and IOC transport through class-aware dynamically loaded drivers.

The native FAT filesystem, shell and full userland, complete diagnostic suite, and extended/far-memory services remain later native ZephyrOS milestones. CP/M work begins only after those native facilities are complete.

---

## 2. Architectural principles

Even at M0/M1, several rules are considered architectural invariants.

### 2.1 ZephyrOS owns the machine

Once control enters ZephyrOS, the kernel owns:

- CPU operating state
- memory banking state
- page zero
- stack placement
- interrupt mode and interrupt ownership
- kernel-reserved memory
- entry points into kernel services

Applications and drivers added later will use kernel-defined interfaces rather than relying directly on kernel layout.

### 2.2 Platform is description, not implementation

Kernel and driver code should not scatter Zephyr-80 I/O port values, memory addresses, wiring facts, or board-specific constants throughout the source tree.

Machine-description constants belong under:

```text
platform/zephyr80/
```

This directory is **declarative only**. It describes what the Zephyr-80 machine is:

- I/O port numbers;
- register-bit constants;
- memory-mode constants;
- physical bank ownership;
- fixed memory-layout constants;
- IRQ/vector wiring facts;
- board-specific channel mappings;
- build-time configuration values.

It must not contain executable implementation.

In particular, `platform/zephyr80/` must contain **no `.c` or `.asm` translation units**, and its include files must not define instruction-emitting routines or executable macros.

Active code belongs to the subsystem that owns the behavior:

```text
boot/       reset, ROM-to-RAM copy, initial kernel handoff
kernel/     banking, syscall crossing, IRQ executive, registries, loaders
drivers/    SIO, CTC, V9958, IOC, HID, storage and other device behavior
user/       native user programs and diagnostics
```

Those implementations consume constants from `platform/zephyr80/`.

The rule is:

> **Platform describes the machine; boot, kernel, drivers, and user programs operate the machine.**

### 2.3 Early debug output is special

The early serial console is not the future ZephyrOS console subsystem.

It is a permanently available low-level diagnostic path intended for:

- early boot messages
- fatal kernel errors
- panic output
- debugging failures in the future device or console infrastructure

It may access the SIO hardware directly.

### 2.4 Native ABI is separate from compiler ABI

SDCC calling conventions are an implementation detail.

Any stable ZephyrOS ABI — including later system calls, driver interfaces, executable entry conventions, or module interfaces — must be explicitly defined by ZephyrOS and accessed through assembly glue where necessary.

### 2.5 No compatibility constraints in the native kernel

CP/M compatibility will later be implemented as a compatibility personality.

Native ZephyrOS does not reserve kernel architecture around CP/M concepts such as BDOS, FCBs, 128-byte records, USER areas, or `CALL 0005h`.

The native system must be complete and useful before the CP/M layer is introduced.

---

# 3. Source tree through M8

The tree should grow only as each milestone requires it.

```text
ZephyrOS/
├── README.md
├── Makefile
│
├── docs/
│   ├── architecture.md
│   └── boot.md
│
├── include/
│   └── zephyros/
│       ├── types.h
│       ├── platform.h
│       ├── irq.h
│       ├── device.h
│       ├── driver.h
│       ├── module.h
│       ├── console.h
│       ├── video.h
│       ├── hid.h
│       ├── ioc.h
│       ├── romfs.h
│       ├── process.h
│       ├── zex.h
│       └── syscall.h
│
├── boot/
│   ├── start.asm
│   └── shadow_copy.asm
│
├── kernel/
│   ├── main.c
│   ├── panic.c
│   ├── early_console.c
│   ├── early_console_io.asm
│   │
│   ├── memory/
│   │   └── banking.asm
│   │
│   ├── syscall/
│   │   ├── entry.asm
│   │   └── dispatch.c
│   │
│   ├── irq/
│   │   ├── irq.c
│   │   ├── entry.asm
│   │   └── vectors.asm
│   │
│   ├── device/
│   │   ├── registry.c
│   │   └── console.c
│   │
│   ├── module/
│   │   ├── loader.c
│   │   ├── relocate.c
│   │   ├── arena.asm
│   │   └── embedded.c
│   │
│   ├── romfs/
│   │   ├── romfs.c
│   │   └── rom_access.asm
│   │
│   └── process/
│       ├── process.c
│       ├── exec.c
│       └── launch.asm
│
├── drivers/
│   ├── sio/
│   │   └── ...
│   ├── ioc/
│   │   └── ...
│   ├── hid/
│   │   └── ...
│   ├── v9958/
│   │   └── ...
│   └── ctc/
│       └── ctc.c
│
├── modules/
│   ├── manifests/
│   │   ├── sio.json
│   │   ├── ioc.json
│   │   ├── hid.json
│   │   └── v9958.json
│   └── runtime/
│       └── sdcc_indirect_calls.asm
│
├── recovery/
│   ├── manifest.json
│   └── files/
│       ├── sio.zmod
│       ├── ioc.zmod
│       ├── hid.zmod
│       ├── v9958.zmod
│       └── monitor.zex
│
├── user/
│   ├── runtime/
│   └── monitor/
│       └── ...
│
├── tests/
│   ├── modules/
│   │   └── ...
│   └── target/
│       ├── m1/ .. m8/
│       ├── include/
│       └── support/
│
├── platform/
│   └── zephyr80/
│       ├── hardware.h
│       ├── hardware.inc
│       ├── memory.h
│       ├── memory.inc
│       ├── irq.h
│       ├── irq.inc
│       └── config.mk
│
├── linker/
│   └── zephyr80.lk
│
└── tools/
    ├── mapcheck.py
    ├── zmodpack.py
    ├── romfspack.py
    └── zexpack.py
```

Directories for the native FAT filesystem, shell, CP/M compatibility, and later userland remain deferred.

## 3.1 Directory ownership rules

The source tree is an architectural boundary, not merely an organizational preference.

### `platform/zephyr80/`

**Configuration and constants only.**

Allowed content includes:

```text
#define / enum constants
.equ constants
port numbers
register masks
memory addresses and region bounds
memory-mode values
IRQ source/vector/channel mappings
board wiring facts
Makefile configuration variables
```

Disallowed content includes:

```text
C functions
assembly routines
ISR bodies
bank-switch routines
reset code
device initialization sequences
polling loops
driver operations
instruction-emitting assembly macros
```

A useful test is:

> If the file emits executable bytes or contains a function/routine that runs on the Z80, it does not belong in `platform/zephyr80/`.

### `boot/`

Owns executable code that runs before the normal kernel environment exists:

- reset/start entry;
- ROM-to-RAM installation;
- initial memory-mode transition;
- handoff to the kernel.

It may include platform constants, but the implementation remains under `boot/`.

### `kernel/`

Owns operating-system mechanisms:

- mode/bank transition routines;
- syscall entry/return;
- IRQ entry/return and dispatch;
- device registry;
- service binding;
- module loading;
- ROM recovery-store access;
- process lifecycle and native executable loading;
- panic and early diagnostic policy.

A mechanism remains kernel code even when its implementation is Zephyr-specific.

For example, the fact that bank switching uses I/O port `00h` is platform configuration; the routine that safely changes modes and preserves caller state is kernel implementation.

### `drivers/`

Owns active device behavior:

- register programming;
- initialization sequences;
- polling;
- interrupt service logic after kernel IRQ dispatch;
- device operations;
- driver-private state.

For example, SIO ports `22h/23h` are declared by the platform description; the code that writes WR3/WR4/WR5 or polls RR0 belongs to the SIO driver, except for the deliberately separate early-console implementation in `kernel/`.

### `user/`

Owns native ZephyrOS programs. User programs consume the native syscall/service ABI and do not reach into private kernel symbols or driver internals.

The M7 monitor is the first substantial program in this tree.

### `linker/`

Owns link placement and section layout.

The linker may consume values derived from the platform description, but linker scripts are not executable target code.

### `tools/`

Owns host-side build, validation, packing, and inspection tools.

These rules apply equally to M0-M8 and to later milestones.

---

# 4. Toolchain

The initial kernel uses the SDCC Z80 toolchain directly.

```text
C source
   │
   ▼
  SDCC
   │
   ▼
 .rel
   │
   ├──────────────┐
   │              │
ASM source        │
   │              │
sdasz80           │
   │              │
   └──── .rel ────┤
                  ▼
               sdldz80
                  │
                  ▼
             zephyros.bin
```

Initial tools:

- **SDCC** — C compiler
- **sdasz80** — Z80 assembler
- **sdldz80** — linker
- **GNU Make** — build orchestration
- **Python** — host-side image, map, validation, module, ROMFS, and executable tooling

The kernel is freestanding. It must not depend on the hosted SDCC C runtime or standard I/O facilities.

A tiny private kernel runtime may later provide functions such as `memcpy`, `memset`, and `strlen` as required.

---

# 5. Milestone M0 — Kernel bring-up

## 5.1 Objective

M0 is complete when the physical Zephyr-80 and the emulator can:

1. enter ZephyrOS from reset/boot,
2. establish a valid stack,
3. initialize the early serial console,
4. call C code successfully,
5. print a ZephyrOS banner,
6. enter a controlled halt loop.

Example output:

```text
ZephyrOS 0.0
platform: Zephyr-80
kernel entry: OK
serial: OK
HALT
```

No other functionality is required.

---

## 5.2 M0 boot flow

M0 assumes the current Zephyr-80 memory hardware and memory-decoder revision that implements modes `00`, `01`, `10`, and `11`.

### 5.2.1 Hardware required by M0/M1

Only two hardware blocks are required to complete M0 and M1:

1. the memory/banking latch and decoder;
2. Z80 SIO channel B for the early serial console.

No CTC, V9958, IOC, SD-card, HID, sound, or other device initialization is required for M0/M1.

The minimum I/O map is:

| Port | Device | Use in M0/M1 |
| ---: | --- | --- |
| `00h` | memory/banking latch | bank selection, ROM page selection, memory mode |
| `20h` | SIO A data | defined for completeness; unused |
| `21h` | SIO A control | defined for completeness; unused |
| `22h` | SIO B data | early console TX |
| `23h` | SIO B control | early console init/status |

The banking latch is decoded by hardware across `00h-0Fh`; ZephyrOS software must use canonical port `00h`.

### 5.2.2 Reset and memory-latch state

At hardware reset the banking latch is expected to be `00h`:

```text
D7..D5 = ROM page 0
D4     = 0
D3     = 0
D2..D0 = SRAM bank 0
```

The latch layout is:

| Bits | Meaning |
| --- | --- |
| `D0-D2` | selected SRAM bank, `0-7` |
| `D3` | `MEM_MODE0`; memory-mode bit 0 |
| `D4` | `MEM_MODE1`; memory-mode bit 1 |
| `D5-D7` | selected ROM page, `0-7` |

For ROM page `p`, SRAM bank `b`, and mode value `m`:

```text
latch = ((p & 7) << 5) | m | (b & 7)
```

The four hardware modes select complete memory mappings; no single bit is a feature flag:

```text
D4 D3   value   meaning
 0  0    00h    mode 00: ROM access / RAM destination
                (reads: selected ROM page everywhere;
                 writes: selected SRAM bank everywhere)
 0  1    08h    mode 01: flat selected SRAM bank
                (no ROM, no forced bank, no common region)
 1  0    10h    mode 10: application + common
 1  1    18h    mode 11: application low + bank-7 OS + common
```

Only mode 00 selects ROM, and it selects ROM for every read; a stack cannot survive it, because a `PUSH` writes SRAM while the matching `POP` would read ROM. `08h` does not expose ROM.

Runtime mode values for application bank `N` are therefore:

```text
mode 10: 10h | N
mode 11: 18h | N
```

Reads from port `00h` return the current latch value and may be used by the kernel banking implementation when preserving or validating the active bank. The port number itself comes from the platform description.

### 5.2.3 Initial ROM-to-RAM handoff

M0 reuses the proven Zephyr-80 ROM-to-RAM boot mechanism.

```text
RESET
  │ latch = 00h
  ▼
ROM page 0 bootstrap
  │
  ├── DI
  ├── copy ROM page 0 -> SRAM bank 0
  ├── select ROM page 1 / SRAM bank 7 in mode 00
  ├── copy ROM page 1 -> SRAM bank 7
  ├── enter mode 10 over bank 0
  ▼
common transition code E000h-FFFFh
  │
  ├── enter mode 11
  ▼
bank 7 visible at 2000h-DFFFh
  │
  ▼
kernel entry
```

Mode 00 copy loops are stackless and only change the latch where the executing ROM bytes are mirrored identically.

For M0/M1:

```text
bank 7:
    2000h-BFFFh   loadable kernel image
    C000h-DFFFh   zero-initialized runtime area / stacks / scratch

bank 0:
    E000h-FFFFh   permanent common code/state
```

The transition from mode 10 to mode 11 must execute from permanent common memory.

### 5.2.4 M0 execution path

After the image is resident in SRAM:

```text
mode 11
  │
  ▼
kernel entry in bank 7
  │
  ├── establish kernel stack
  ├── call early_console_init()
  ├── call kernel_main()
  ▼
controlled HALT loop
```

The boot path must not depend on state left by CP/M, ZSDOS, ZCPR, or the existing BIOS.

## 5.3 Kernel entry contract

`start.asm` owns the transition from firmware/boot state into the C kernel.

Before calling `kernel_main()` it must guarantee interrupts are disabled, the documented mapping and stack are established, required C runtime state is initialized, and no previous-firmware register contents are assumed.

```c
void kernel_main(void);
```

It does not return.

## 5.4 Early serial console

The M0 serial implementation directly uses Z80 SIO channel B:

```text
SIO channel B data       22h
SIO channel B control    23h
SIO clock                1.8432 MHz
mode                     asynchronous x16
line format              115200 baud, 8 data bits, 1 stop bit, no parity
flow control             none required
interrupts               disabled
I/O method               polling
```

The known-good initialization sequence is:

```text
OUT (23h),18h
OUT (23h),04h
OUT (23h),44h
OUT (23h),03h
OUT (23h),C1h
OUT (23h),05h
OUT (23h),EAh
OUT (23h),01h
OUT (23h),00h
```

RR0 bit `01h` means RX available and `04h` means TX buffer empty.

```c
void early_console_init(void);
void early_console_putc(char ch);
void early_console_puts(const char *s);
```

The early console remains permanently separate from the registered console service and is retained for boot/panic diagnostics.

## 5.5 M0 failure behavior

Fatal errors print through the early console, disable interrupts, and halt.

---

# 6. Milestone M1 — Kernel foundation

M1 establishes the kernel memory map, page-zero ownership, permanent common region, stacks, native syscall gateway, banking primitives, and layout validation.

# 7. M1 memory model

## 7.1 Physical banks versus kernel abstractions

The memory-control latch is port `00h`. Direct access is owned by kernel banking code.

```text
MEMCTL_PORT       = 00h
BANK_MASK         = 07h
MEM_MODE0         = 08h
MEM_MODE1         = 10h
ROM_PAGE_MASK     = E0h
MODE_ROM          = 00h
MODE_FLAT_RAM     = 08h
MODE_APPLICATION  = 10h
MODE_OS           = 18h
```

Application banks are `0-6`; bank `7` is reserved to ZephyrOS.

```c
uint8_t memctl_read(void);
void memctl_write(uint8_t value);
uint8_t bank_get(void);
void enter_application_mode(uint8_t bank);
void enter_os_mode(uint8_t bank);
```

Mode-changing primitives execute from common memory and preserve caller-bank identity.

## 7.2 Kernel-visible regions

### Mode 10 — application execution

```text
0000h-DFFFh  selected application bank N
E000h-FFFFh  physical SRAM bank 0 common
```

### Mode 11 — OS execution

```text
0000h-1FFFh  selected application bank N
2000h-DFFFh  physical SRAM bank 7
E000h-FFFFh  physical SRAM bank 0 common
```

The bank selector remains the caller/process identity while mode 11 is active.

---

# 8. Page-zero ownership

For native execution ZephyrOS owns `0000h-00FFh` and reserves `RST 30h` at `0030h` as the native syscall entry:

```asm
.org 0x0030
    jp zos_syscall_gate
```

The actual gate is in common memory.

# 9. Native syscall ABI scaffold

```text
Entry:   RST 30h
Input:   A = syscall number, HL = argument-block pointer
Return:  Carry clear success / Carry set error
         HL = primary result, A = errno on error
```

CP/M later receives its own page-zero personality and does not define the native ABI.

# 10. Common kernel entry

The common syscall gate saves caller SP, switches to a common gate stack before changing mapping, enters mode 11, dispatches bank-7 kernel code, then restores mode 10 and the caller stack before `RET`.

# 11. Kernel stacks

The normal kernel stack is in bank-7 runtime memory (`C000h-DFFFh`). A small common gate stack is in physical bank-0 common memory and is used only while crossing mappings.

# 12. Linker and memory-map validation

`tools/mapcheck.py` and image tooling turn placement violations into build errors. They validate page zero, common code/stacks, bank-7 loadable/runtime separation, entry-point placement, and platform-source purity.

# 13. Assembly/C boundary

Assembly owns machine-state-sensitive paths such as startup, port I/O, banking, syscall/IRQ crossing and later far calls. C owns policy and higher-level kernel logic. Stable interfaces must not expose SDCC calling convention directly.

# 14. Interrupt state at the M1 boundary

M0/M1 finish with interrupts disabled. M2 owns IM2 and interrupt policy.

# 15. Driver state at the M1 boundary

M0 serial is an early console, not a registered driver. M3 introduces the driver/device/capability/service model.

# 16. Relationship to future CP/M compatibility

CP/M `.COM` execution later constructs a CP/M-compatible page zero (`0000h`, `0005h`, FCBs, command tail, `0100h`) without changing the native ZephyrOS ABI.

# 17. M0/M1 acceptance criteria

M0/M1 are complete and hardware-validated. The authoritative target remains physical Zephyr-80 hardware; simulators and host-side map/image checks supplement but do not replace hardware validation.

---

# 18. Milestone M2 — Interrupt executive

M2 establishes kernel-owned IM2, common-memory IRQ crossing, source ownership, bank-7 handler dispatch, safe `FFh` handling, and hardware CTC0 proof. It does not introduce nesting, scheduling, preemption, or application-owned vectors.

## 18.1 Ownership and entry

Drivers may claim logical IRQ sources, but the kernel owns `I`, the IM2 table, entry/exit, stacks, mapping transitions, and final `RETI`.

An interrupt accepted in mode 10 saves the interrupted SP, switches to a common transition stack before mapping changes, saves full context, enters mode 11, switches to the bank-7 ISR stack, dispatches the handler, and restores the exact prior latch/context before `RETI`.

## 18.2 IM2 page and FFh invariant

```text
FD00h-FDFFh  IM2 vector page
FE00h        FFh-vector guard byte
I = FDh
```

The build validates that vector `FFh` resolves safely through `FDFFh/FE00h`. This path is required for sources such as the V9958 that may present `FFh` on the data bus.

## 18.3 Logical sources

```text
IRQ_CTC0
IRQ_CTC1
IRQ_CTC2
IRQ_CTC3
IRQ_SIO0
IRQ_SIO1
IRQ_VDP_FF
```

Platform wiring/vector constants remain declarative; active dispatch belongs to `kernel/irq/`.

## 18.4 Handler contract

Handlers execute in bank 7, service/clear their source, remain bounded, do not own `RETI`, and do not install directly into the IM2 page.

---

# 19. M2 acceptance criteria

M2 is complete when hardware proves repeated CTC0 interrupt delivery through the executive, exact context/mapping restoration from modes 10 and 11, safe unexpected/FFh handling, and continued early-console operation.

---

# 20. Milestone M3 — Device registry and static drivers

## 20.1 Objective

M3 introduces the driver/device/capability/service model with a statically linked SIO0/B proof driver. This static table is historical since M4; production uses ZMOD modules.

## 20.2 Concepts

```text
driver       hardware-family implementation
device       registered instance
capability   operations the instance provides
service      OS binding selecting devices for a role
```

Console input and output are independent bindings.

## 20.3 Device classes and capabilities

M3 initially reserved:

```text
ZDEV_SERIAL
ZDEV_VIDEO
ZDEV_STORAGE
ZDEV_INPUT
ZDEV_TRANSPORT
ZDEV_OTHER
```

and introduced:

```text
ZCAP_CHAR_IN
ZCAP_CHAR_OUT
ZCAP_CONSOLE_IN
ZCAP_CONSOLE_OUT
```

M8 turns the reserved video/input/transport concepts into real class-specific interfaces. `ZDEV_INPUT` becomes `ZDEV_HID` and `ZDEV_TRANSPORT` becomes `ZDEV_IOC` while retaining their numeric slots in driver ABI v2; this avoids gratuitous renumbering while making the ownership explicit.

## 20.4 Device registry

The fixed-size bank-7 registry issues boot-local handles, rejects duplicates/overflow, stores driver-owned ops/state pointers, and supports capability lookup. Handle 0 is invalid.

## 20.5-20.7 Historical static-driver path

The M3 `struct zdriver`, `boot_drivers[]`, and `driver_init_static()` mechanism is retired. Production images since M4 load ZMOD drivers. Static M3 details remain milestone history only.

## 20.8 SIO0/B driver

The first driver wraps SIO0/B (`22h/23h`, 115200 8N1) and provides character/console input/output. The early console remains a separate direct panic path.

## 20.9 Console service

The console service stores independent input and output handles and exposes `putc/write/poll/getc`; panic bypasses it.

---

# 21. M3 acceptance criteria

M3 is complete: registry/capability semantics, static SIO proof, console service, error handling and bank-7 placement were validated before M4 retired the production static-driver path.

---

# 22. Milestone M4 — Dynamic driver/module loading

## 22.1 Objective

M4 changes drivers from statically linked code to boot-loadable resident ZMOD modules while preserving the M3 registry/service model. Modules remain resident until reboot; runtime unload/hot-plug is not required.

## 22.2 Fixed-granule driver arena

M4 introduced a bank-7 driver arena using 512-byte allocation granules. A module receives a contiguous whole number of granules; its image, BSS/private state, alignment and reserved slack all belong to that allocation.

The loader consumes linker symbols rather than hard-coded module addresses:

```text
__driver_arena_start
__driver_arena_end
```

### M4 historical geometry

The M4 acceptance implementation reserves:

```text
A000h-BFFFh   8192 bytes = 16 x 512-byte granules
```

between the loadable kernel and the `C000h-DFFFh` bank-7 runtime region. This geometry was sufficient to prove allocation, relocation, placement independence, BSS, rollback and dynamic SIO loading. It is retained as M4 history and in M4 acceptance fixtures.

### M8 production geometry revision

Eight KiB is not a viable production budget once the native V9958, HID and IOC stacks are resident; the current V9958 implementation alone is on the order of 4 KiB. M8 therefore removes `A000h` as a production architectural boundary.

Production geometry becomes:

```text
2000h
    linked static kernel code
    ...
__kernel_code_end
    alignment to 512-byte granule
__driver_arena_start
    dynamically loaded resident ZMOD modules
    ...
__driver_arena_end = C000h

C000h-DFFFh
    bank-7 runtime/system workspace
    kernel state, stacks, storage/cache/deblocking workspace, scratch
```

Rules:

- `__driver_arena_start = align_up(__kernel_code_end, 512)` conceptually; the concrete linker/build mechanism may use generated symbols but must express this relationship rather than a fixed `A000h` base;
- `__driver_arena_end` remains fixed at `C000h`;
- the production build must fail if the resulting driver arena is less than **16 KiB** (`4000h`);
- if the linked kernel is smaller, drivers automatically receive the additional space;
- `C000h-DFFFh` is never absorbed into the module arena: it remains bank-7 runtime/system workspace, including the storage/cache/deblocking needs introduced by M9;
- the build reports total arena size, used granules, free granules, and per-module allocations;
- no ZMOD binary or relocation rule changes because module placement was already loader-selected.

This revision is one of the intended benefits of M4 relocation: moving the linked kernel boundary may move every loaded driver without rebuilding the ZMODs.

## 22.3 Module format

ZMOD v1 is a generic relocatable container; M4 uses module type `DRIVER`. The header describes image size, BSS, allocation requirement, init/fini offsets, ABI version and relocation table.

## 22.4 Relocation model

The initial runtime relocation is `ZREL_ABS16`:

```text
word_at(module_base + patch_offset) += module_base
```

The host tool resolves the module at virtual base `0000h`, uses SDCC/ASxxxx relocation records to identify patch locations, resolves symbols, and emits only the compact target-side patch table. The Z80 loader never parses object files or symbol tables.

## 22.5 Kernel imports

Dynamic drivers receive a versioned `struct zdriver_api *` and do not link directly against arbitrary private kernel symbols.

## 22.6 BSS/private state

Module allocation covers loaded image plus zeroed BSS/private state and alignment. Registry entries point into driver-owned state but do not own it.

## 22.7 Load sequence

```text
source -> validate -> allocate granules -> copy -> zero BSS
       -> relocate -> init(driver_api) -> register devices/IRQs -> RESIDENT
```

Failed initialization rolls back allocation and registry/IRQ changes transactionally.

## 22.8 Initial source

M4 used an embedded module catalog. M5 replaces production module sourcing with ROMFS without changing loader semantics.

## 22.9 Dynamic SIO proof

The SIO driver was removed from the static table, packed as ZMOD, loaded at allocator-selected address, relocated, initialized and used as the normal console while the direct early console remained available.

## 22.10 IRQ handlers

Loaded modules may claim M2 logical sources; handlers live in their relocated bank-7 allocation and are dispatched only after the IRQ executive maps mode 11.

## 22.11 Lifetime

Successful modules remain resident until reboot. No hot unplug or live unload is required.

## 22.12 Host tooling

`tools/zmodpack.py` consumes compiler/assembler outputs and relocation data, resolves module symbols, emits/validates ZMOD and reports image/BSS/relocation/allocation sizes.

## 22.13 Implementation notes

M4 is implemented and hardware-validated. `docs/module-format.md`, `docs/devices.md` and the M4 validation record are authoritative for the concrete stage-07/stage-08 implementation and acceptance images.

---

# 23. M4 acceptance criteria

M4 is complete. Acceptance proved allocator exhaustion/error handling, ZMOD validation, placement-independent relocation, BSS zeroing, device registration, dynamic console output, shifted-arena execution of byte-identical modules, relocated IRQ handler execution, failed-init rollback, and early-console fallback.

---

# 24. Milestone M5 — ROM recovery filesystem

## 24.1 Objective

M5 adds a small read-only filesystem backed by ROM. It is the boot/recovery store available even when IOC, SD, FAT or later userland is unavailable and provides boot ZMODs, native recovery programs and immutable recovery assets.

## 24.2 ROM access

Runtime ROM reads use mode `00`, the only mapping that selects ROM. Because mode 00 selects ROM for every read while writes still target the selected SRAM bank, the transition uses a fixed stackless stub mirrored byte-identically in common SRAM and every recovery ROM page.

Only `0000h-BFFFh` of each recovery ROM page is logical ROMFS payload. `C000h-FFFFh` is excluded and carries the mirrored access stub/FF fill.

```text
ROMFS_PAGE_BYTES = C000h = 48 KiB
```

## 24.3 Access primitive

The common path validates the request, locks interrupts, preserves the exact latch/IFF state, arms registers, switches to mode 00, performs stackless `LDIR` from ROM to bank-0 staging memory, restores the exact latch, then returns through ordinary common code.

## 24.4 Format

ROMFS v1 is a deterministic flat read-only namespace with header, file table and payloads. Entries contain native names, logical offsets, sizes and type/flags. Directories/writes/timestamps are unnecessary for recovery.

## 24.5 Kernel interface

```c
int romfs_init(void);
int romfs_lookup(const char *name, romfs_file_t *out);
int romfs_read(const romfs_file_t *file, uint32_t offset,
               void *dst, uint16_t length);
```

This private recovery interface does not define the later general FAT filesystem API.

## 24.6 Host construction

`tools/romfspack.py` builds/validates the deterministic recovery image and `pack_image.py` places it in configured ROM pages while preserving boot pages and mirrored access stubs.

## 24.7 Module-source transition

Production boot changes from embedded `sio.zmod` bytes to ROMFS lookup/source-backed loading while the M4 ZMOD binary and loader remain unchanged.

## 24.8 Implementation notes

M5 was implemented and physically accepted 2026-10-03. Six recovery pages (2-7) provide six 48-KiB windows (288 KiB); `docs/romfs-format.md` and `docs/validation/2026-10-03-m5-romfs.md` record the concrete format, corrected mode-00 access mechanism and validation evidence.

---

# 25. M5 acceptance criteria

M5 is complete. Acceptance includes deterministic ROMFS generation, bounded/cross-page reads, exact mapping restoration, corrupt-image failure through early diagnostics, source-backed production loading of `sio.zmod`, and physical-console proof.

---

# 26. Milestone M6 — Native process and ZEX execution

## 26.1 Objective

M6 introduces single-tasking native process lifecycle:

```text
parent -> exec child -> parent suspended -> child -> exit -> parent resumes
```

No scheduler or preemption is required.

## 26.2 Process descriptor

Bank-7 descriptors track slot/state, application bank `0-6`, parent, saved context/status and flags. Bank 7 is never a process bank; only one process runs at a time.

## 26.3 Native page zero

Every process gets a freshly constructed native page zero containing the `RST 30h` gateway. It contains no CP/M structures.

## 26.4 ZEX v1

M6 ZEX is deliberately single-bank: one code/data image, BSS, entry and stack, linked at logical `0100h`. Choosing a different physical application bank does not change logical addresses and therefore does not require module-style base relocation.

## 26.5 Header

ZEX v1 carries magic/version/ABI/flags plus image size, BSS size, entry offset, stack size and header size. The loader rejects overlap, overflow and invalid entry geometry.

## 26.6 Process memory

```text
0000h-00FFh   native page zero
0100h-...     ZEX image
...           BSS/free memory
...-DFFFh     downward process stack
E000h-FFFFh   common memory (not owned by process)
```

## 26.7 Load/launch

ROMFS lookup -> validate -> allocate bank -> initialize page zero -> copy image -> zero BSS -> descriptor/stack -> common launch gate -> mode 10 -> entry.

## 26.8 Minimal native syscalls

```text
SYS_EXIT
SYS_CONSOLE_WRITE
SYS_CONSOLE_READ
SYS_CONSOLE_POLL
SYS_GETINFO
```

Console calls use the normal console service, never direct SIO.

## 26.9 Exit

`SYS_EXIT` releases the child's bank/resources, records status, restores the parent/kernel-launcher context and resumes after `exec`.

## 26.10 Non-goals

No scheduler, signals, pipes, dynamic linking, writable FS, far allocations, CP/M COM or shell.

## 26.11 Implementation notes

M6 was implemented and physically accepted 2026-10-03. The concrete process allocator, bank-copy gate, syscall implementation, ZEX runtime/packer, acceptance fixtures and simulator/hardware evidence are recorded in `docs/zex-format.md` and `docs/validation/2026-10-03-m6-zex.md`.

---

# 27. M6 acceptance criteria

M6 is complete. Acceptance proved bank allocation/release, deterministic page zero, ROMFS ZEX loading at `0100h`, BSS/stack geometry, native console syscalls, exit/resume, the same ZEX in multiple physical banks, repeat-cycle stability, malformed-image rejection and bank-7 exclusion.

---

# 28. Milestone M7 — Native monitor and diagnostic foundation

## 28.1 Objective

M7 ports the existing Zephyr-80 monitor concept into an ordinary native `monitor.zex`. It is the first useful native application and the interactive environment before the shell exists.

```text
reset -> kernel -> ROMFS -> required ZMODs -> console -> exec monitor.zex
```

When M10 introduces the shell, the monitor remains a diagnostic/recovery program.

## 28.2 Port, do not transplant

Preserve useful monitor behavior (help, system/register information, memory dump/modify, I/O port read/write, bank/map information, controlled execute diagnostics) while replacing old ROM/CP/M/banking assumptions with native ZephyrOS contracts.

## 28.3 Native console only

`monitor.zex` uses native console syscalls. In M7 they bind to dynamic SIO0/B. M8 must be able to rebind the same monitor binary to HID input plus V9958 output with no monitor rebuild.

## 28.4 Diagnostic boundary

M7 may introduce controlled diagnostic services such as memory read/write, port input/output, bank info and system info. They are explicit ZephyrOS diagnostic interfaces, not private-kernel symbol access.

## 28.5 Diagnostic-suite foundation

Reusable parsing, numeric formatting, line editing, hexdump, error/status reporting and diagnostic wrappers become the foundation for M11 programs covering memory, IRQ/CTC, SIO, VDP, HID, IOC, storage, sound and system diagnostics.

## 28.6 Packaging/launch

`monitor.zex` is stored in ROMFS and launched through the ordinary M6 `exec` path. The kernel pre-shell launcher may re-exec it after exit until M10 replaces that boot policy.

## 28.7 Future role

The M7 monitor over SIO helps bring up M8 devices and M9 storage, then remains available after the shell and full diagnostic suite arrive.

---

# 29. M7 acceptance criteria

M7 is accepted when `monitor.zex` runs as an ordinary ROMFS ZEX, uses native console and diagnostic interfaces, supports its core interactive commands, cleanly exits/resumes, survives repeated exec cycles, and contains no SIO port-number dependency.

---

# 30. Milestone M8 — Native device stack completion

## 30.1 Objective

M8 establishes the normal native ZephyrOS device environment:

```text
normal console input   -> HID keyboard
normal console output  -> V9958
IOC transport          -> command + bulk services for HID and later FAT/storage
serial fallback        -> SIO0/B registered driver
early/panic console    -> direct SIO0/B, permanently independent
```

M8 is the point where the M3/M4 character-device proof grows into a class-aware device architecture. The device registry remains the common discovery mechanism, but video, HID and IOC each gain a real class contract rather than being forced through the character-device operation shape.

All three production drivers are ordinary resident ZMOD modules sourced from ROMFS and relocated by the M4 loader.

---

## 30.2 Production driver-area sizing

M8 activates the production arena revision defined in section 22.2:

```text
__driver_arena_start = align_up(__kernel_code_end, 512)
__driver_arena_end   = C000h
minimum arena size   = 4000h (16 KiB)
allocation granule   = 0200h (512 bytes)
```

The build must fail rather than silently squeeze modules if the linked kernel leaves less than 16 KiB.

The production report must list:

```text
static kernel end
arena start/end/size
module name
module image+BSS size
allocated granules
free granules after boot
```

`C000h-DFFFh` remains outside the module allocator and remains bank-7 runtime/system workspace. M9 may use this region for filesystem state, sector/cache/deblocking buffers and IOC/storage working memory; M8 must not consume it merely to fit drivers.

Large immutable resources used by a driver (fonts, icons, boot graphics and similar assets) should not automatically be resident module contents. They belong in ROMFS or later resource facilities unless they are intrinsically required resident tables.

---

## 30.3 Driver ABI v2 and class-aware interfaces

The M3/M4 ABI uses a 16-bit capability mask and one `putc/write/poll/getc` operation shape. M8 introduces driver ABI v2 so devices can expose both generic character behavior and class-specific operations.

Conceptually:

```c
#define ZDRIVER_ABI_VERSION 2u

typedef uint32_t zdev_caps_t;

struct zchar_ops {
    int (*putc)(void *state, char ch);
    int (*write)(void *state, const char *buf, uint16_t len);
    int (*poll)(void *state);
    int (*getc)(void *state);
};

struct zdevice_desc {
    uint8_t                 class_id;
    zdev_caps_t             capabilities;
    const char             *name;
    const struct zchar_ops *char_ops;     /* optional */
    const void             *class_ops;    /* type determined by class */
    void                   *state;
};
```

The existing generic character capabilities remain meaningful:

```text
ZCAP_CHAR_IN
ZCAP_CHAR_OUT
ZCAP_CONSOLE_IN
ZCAP_CONSOLE_OUT
```

A device may expose character operations, class operations, or both. Capability bits are authoritative; unsupported operations may be absent/NULL and must return `ZDEV_ENOTSUP` through the appropriate wrapper.

The device registry itself remains fixed-size and heap-free unless actual M8 device counts prove the existing capacity insufficient.

---

## 30.4 VIDEO class

M8 makes `ZDEV_VIDEO` a concrete class contract.

The native interface is intentionally modeled on the useful shape of the current ZSDOS `VIDEO_SEND` service: submit a device-oriented video operation plus an optional payload. It does **not** inherit the historical Virtual Drip packet framing or its 16-byte single-frame restriction.

Conceptually:

```c
struct zvideo_ops {
    int (*send)(void *state,
                uint8_t operation,
                const void *payload,
                uint16_t length);
};
```

Initial native operation vocabulary:

```text
ZVIDEO_CTRL_WRITE
ZVIDEO_DATA_WRITE
ZVIDEO_DATA_BLOCK
ZVIDEO_PALETTE_WRITE
ZVIDEO_INDIRECT_WRITE
ZVIDEO_PRESENT
ZVIDEO_RESET
```

Suggested capabilities:

```text
ZCAP_VIDEO_CTRL_WRITE
ZCAP_VIDEO_DATA_WRITE
ZCAP_VIDEO_DATA_BLOCK
ZCAP_VIDEO_PALETTE
ZCAP_VIDEO_INDIRECT
ZCAP_VIDEO_PRESENT
ZCAP_VIDEO_RESET
```

The native operation numbers do not have to equal the frozen ZSDOS/Virtual-Drip `VIDEO_SEND` packet values. When the CP/M personality arrives, it translates the historical ABI to the native video operations. This prevents an old transport constraint from becoming the native ZephyrOS video ABI.

### V9958 device

The V9958 module registers a device such as:

```text
name:   v9958.0
class:  ZDEV_VIDEO
caps:   VIDEO_* as implemented
        CHAR_OUT
        CONSOLE_OUT
```

It therefore exposes two independent views of the same hardware:

```text
native graphics/video clients -> zvideo_ops.send()
normal text console           -> zchar_ops.putc/write()
```

The module owns the V9958 programming, terminal/ANSI-light renderer and driver-private state. The console service does not know V9958 register details.

M8 need not require VDP interrupts for console/video acceptance. If the driver enables V9958 `/INT`, it must claim/use the M2 `IRQ_VDP_FF` path; it may not install a private vector or bypass the common IRQ executive.

---

## 30.5 HID class

M8 refines the M3 placeholder `ZDEV_INPUT` into `ZDEV_HID`, retaining the same numeric class slot under driver ABI v2.

HID is a semantic device layer above the IOC transport. The HID driver understands keyboard/gamepad commands and data; it does **not** know SIO1 port numbers, synchronous-lane framing, or CTS register semantics.

Suggested capabilities:

```text
ZCAP_HID_TEXT
ZCAP_HID_KEY_EVENTS      reserved/optional in M8
ZCAP_HID_GAMEPAD         reserved/optional in M8
```

A keyboard instance also advertises:

```text
ZCAP_CHAR_IN
ZCAP_CONSOLE_IN
```

Conceptual class operations:

```c
struct zhid_ops {
    int (*read_text)(void *state,
                     uint8_t *dst,
                     uint16_t max_len);

    int (*read_gamepad)(void *state,
                        uint8_t index,
                        struct zhid_gamepad_state *out);
};
```

Methods unsupported by a particular instance may be absent; capability bits determine what clients may request.

A module may register multiple devices, for example:

```text
hid.keyboard0   ZDEV_HID   HID_TEXT | CHAR_IN | CONSOLE_IN
hid.gamepad0    ZDEV_HID   HID_GAMEPAD
hid.gamepad1    ZDEV_HID   HID_GAMEPAD
```

M8 acceptance requires the keyboard/text console path. Gamepad/event capabilities may be implemented later without changing the class boundary.

### HID/IOC relationship

The current controller keeps translated keyboard bytes in its own queue and exposes a hardware pending/doorbell indication. In M8:

```text
HID driver
    -> queries IOC status: HID_PENDING
    -> issues the IOC HID-input command only when needed
    -> buffers/presents text through zchar_ops
```

The HID driver never samples `/CTSB` directly. The IOC driver owns the meaning of that pin/status bit.

---

## 30.6 IOC class

M8 refines the M3 placeholder `ZDEV_TRANSPORT` into `ZDEV_IOC`, retaining its numeric slot under driver ABI v2.

The IOC module owns SIO1 and the two-lane synchronous transport:

```text
SIO1/B   command/reply lane
SIO1/A   bulk lane
```

Suggested capabilities:

```text
ZCAP_IOC_COMMAND
ZCAP_IOC_BULK_READ
ZCAP_IOC_BULK_WRITE
ZCAP_IOC_STATUS
```

Conceptual interface:

```c
struct zioc_ops {
    int (*command)(void *state,
                   const struct zioc_frame *request,
                   struct zioc_frame *reply);

    int (*bulk_read)(void *state,
                     void *dst,
                     uint16_t length);

    int (*bulk_write)(void *state,
                      const void *src,
                      uint16_t length);

    uint16_t (*status)(void *state);
};
```

The IOC driver owns:

- SIO1 port/register programming;
- command/reply framing and synchronization;
- bulk-lane framing and synchronization;
- lane serialization/admission state;
- bounded transport timeouts and recovery;
- raw hardware doorbell/status interpretation.

Class-level status exposes semantic bits such as:

```text
ZIOC_STATUS_HID_PENDING
```

rather than exposing the underlying `/CTSB`/RR0 bit to consumers.

`bulk_read()` / `bulk_write()` are valid only in transport state admitted by the corresponding command protocol; the IOC driver owns that state and rejects invalid sequencing. M9 FAT/storage uses the same IOC class rather than duplicating transport code.

---

## 30.7 Driver-to-driver discovery

M8 needs one resident driver to consume another registered device (HID -> IOC). Direct symbol references or lookup by instance name are not permitted.

Driver ABI v2 therefore adds read-only discovery/interface access to `zdriver_api`, semantically equivalent to:

```c
zdev_handle_t (*device_find)(uint8_t class_id,
                             zdev_caps_t required_caps,
                             zdev_handle_t after);

int (*device_interface)(zdev_handle_t handle,
                        uint8_t expected_class,
                        const void **class_ops,
                        void **state);
```

Rules:

- clients select dependencies by class + required capabilities, not module or device names;
- `device_interface()` verifies the expected class before returning operations/state;
- callers treat returned device state as opaque;
- normal clients do not receive arbitrary registry write access;
- dependency acquisition does not create a runtime unload problem because M8 modules remain resident until reboot.

M8 does not require a general dependency solver. ROMFS/boot-module order is explicit and deterministic.

---

## 30.8 Boot order

The normal M8 module order is conceptually:

```text
sio.zmod
    -> fallback character console exists

ioc.zmod
    -> ZDEV_IOC command/bulk/status available

hid.zmod
    -> discovers ZDEV_IOC
    -> registers HID keyboard/text input

v9958.zmod
    -> registers video + character output

console_bind_defaults()
    -> preferred input/output selected

exec monitor.zex
```

V9958 does not depend on IOC. Its relative load order may change as long as all normal-console candidates are registered before final console binding.

A module-init failure does not invalidate the already loaded fallback SIO device.

---

## 30.9 Console binding policy

M8 changes default binding from “first device with the generic capability” to a preference policy based on class + capability.

Preferred normal console:

```text
input:   ZDEV_HID   + ZCAP_CONSOLE_IN
output:  ZDEV_VIDEO + ZCAP_CONSOLE_OUT
```

Fallback selection then uses any registered device providing the required console capability, which normally resolves to SIO0/B.

Expected combinations:

```text
all native devices present:
    HID -> console -> V9958

V9958 failed:
    HID -> console -> SIO

HID failed:
    SIO -> console -> V9958

HID + V9958 failed:
    SIO -> console -> SIO

kernel panic / very early boot:
    direct early SIO path regardless of registry/bindings
```

The M7 `monitor.zex` must run unchanged in all valid normal-console combinations because it uses only the native console syscall ABI.

---

## 30.10 M8 non-goals

M8 does not implement:

- FAT or the native general filesystem;
- SD file semantics or directory/path APIs;
- the native shell;
- the full diagnostic suite;
- module unload/hot-plug;
- far-code/far-data services;
- CP/M compatibility.

It provides the device foundation those later milestones consume.

---

# 31. M8 acceptance criteria

M8 is accepted when:

- production driver-arena start is derived from the aligned linked-kernel end rather than fixed at `A000h`;
- driver-arena end remains `C000h`, allocation remains 512-byte granular, and the build rejects a production arena smaller than 16 KiB;
- the build reports total/used/free arena granules and per-module allocations;
- `C000h-DFFFh` remains outside the module arena and available as bank-7 runtime/system workspace;
- driver ABI v2 supports 32-bit capabilities, optional character operations and class-specific operations;
- SIO remains a dynamically loaded serial/character fallback device;
- the IOC ZMOD registers `ZDEV_IOC` and proves command, bulk-read, bulk-write and semantic status behavior on physical hardware;
- the HID ZMOD discovers IOC through class/capability lookup rather than symbols/names, registers a keyboard/text `ZDEV_HID`, and provides `ZCAP_CHAR_IN | ZCAP_CONSOLE_IN`;
- the V9958 ZMOD registers `ZDEV_VIDEO`, provides the supported `ZVIDEO_*` send operations, and also provides `ZCAP_CHAR_OUT | ZCAP_CONSOLE_OUT` through its text renderer;
- native video block operations are not limited by the historical 16-byte Virtual Drip single-frame ceiling; bounds are defined by the native API/driver implementation;
- normal console binding selects HID input and V9958 output when both are available;
- each HID/V9958 failure combination falls back cleanly as defined in section 30.9 without losing the direct early/panic SIO path;
- the exact same `monitor.zex` used by M7 runs interactively through HID/V9958 without SIO-specific changes;
- malformed/unsupported class operations return bounded errors rather than calling through invalid pointers;
- all resident M8 production modules fit simultaneously while satisfying the 16-KiB minimum-arena build invariant.

Physical Zephyr-80 hardware is authoritative for V9958 timing/wait-state behavior, HID doorbell/IOC transport behavior and complete console interaction.

---

# 32. Deferred architecture after M8

The following remain intentionally deferred until after M8:

- native FAT filesystem on SD with paths, directories, file handles, CWD, and normal byte-oriented I/O;
- native shell and core userland;
- complete bundled native diagnostic suite;
- extended/far-memory objects, far-code and far-data services;
- CP/M compatibility personality;
- `.COM` loader;
- CP/M BDOS/FCB/filesystem compatibility;
- driver unload/hot-plug;
- preemptive multitasking or scheduling.

The first four items above are **native ZephyrOS** work. CP/M compatibility begins only after the native filesystem, shell/userland, diagnostics and extended-memory facilities are complete.

---

# 33. Milestone summary

```text
M0  COMPLETE     kernel bring-up
M1  COMPLETE     kernel foundation / banking / syscall scaffold
M2  COMPLETE     interrupt executive
M3  COMPLETE     registry + static-driver proof
M4  COMPLETE     relocatable dynamic ZMOD drivers
M5  COMPLETE     ROM recovery filesystem
M6  COMPLETE     native process + single-bank ZEX execution
M7               native monitor / diagnostic foundation
M8               native device stack
                 - production-sized relocatable driver arena
                 - class-aware driver ABI v2
                 - IOC command/bulk transport
                 - HID native input
                 - V9958 video + console output
                 - HID/V9958 normal console with SIO fallback
```

At the end of M8, ZephyrOS can boot from ROMFS, load its native device stack dynamically, execute native programs, use the V9958/HID console, communicate with the IOC, and retain the serial recovery path — all without any CP/M layer.

---

# 34. Later milestone roadmap

The intended sequence after M8 is:

```text
M9  Native FAT filesystem on SD
    - IOC-backed FAT filesystem
    - paths and real directories
    - file handles / byte-oriented I/O
    - CWD and normal native filesystem semantics

M10 Native shell and core userland
    - launch ZEX programs
    - cd/pwd/ls/cp/mv/rm/mkdir/cat and related core commands

M11 Native diagnostic suite
    - port/consolidate useful existing Zephyr/ZSDOS diagnostics
    - memory, banking, IRQ/CTC, SIO, V9958, HID, IOC, storage, sound, system

M12 Extended memory / far-code / far-data services
    - auxiliary native allocations
    - ZEX segment/relocation metadata
    - far-call and far-data mechanisms

---------------- native ZephyrOS complete ----------------

M13 CP/M compatibility personality
M14 COM executable support
M15 CP/M BDOS / FCB / filesystem compatibility
```

M12 is part of native ZephyrOS, not a compatibility extension. The CP/M layer begins at M13 and translates CP/M semantics onto native ZephyrOS services that already work independently.

The milestone names after M8 define intended sequencing. Their detailed architecture should be added as each milestone is approached rather than prematurely constraining the implementation.
