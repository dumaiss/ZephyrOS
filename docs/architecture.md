# ZephyrOS Architecture

**Status:** M0-M8 complete and physically accepted; M9 architecture defined for implementation (bootstrap slice implemented by work package 25, see docs/vfs.md and section 32.38.1)
**Scope:** Milestones M0 through M9
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
- **M7 — Native monitor / diagnostic foundation:** port the Zephyr monitor as the first useful native ZEX and establish the foundation for the later diagnostic suite. **Complete.**
- **M8 — Native device stack completion:** provide the normal V9958/HID console and IOC transport through class-aware dynamically loaded drivers. **Complete.**
- **M9 — Native VFS and filesystem framework:** establish the native namespace, filesystem-provider model, bootstrap/system filesystem, IOC-FAT provider (with its own file-data cache), block-device class, and native file/directory syscall ABI.

The native shell and full userland, complete diagnostic suite, and extended/far-memory services remain later native ZephyrOS milestones. Compatibility personalities begin only after those native facilities are complete.

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
drivers/    SIO, CTC, V9958, IOC, storage and other device behavior
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

The native system must be complete and useful before the compatibility layers are introduced.

---

# 3. Source tree through M7

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
│   │   ├── page_zero.asm
│   │   ├── diag.c
│   │   ├── diag.h
│   │   └── diag_io.asm
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
│   │   └── arena.asm
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
│   │   ├── sio.c
│   │   ├── sio.h
│   │   ├── sio_io.asm
│   │   └── sio_module.c
│   └── ctc/
│       └── ctc.c
│
├── modules/
│   ├── manifests/
│   │   └── sio.json
│   └── runtime/
│       └── sdcc_indirect_calls.asm
│
├── recovery/
│   ├── manifest.json
│   └── files/
│       ├── sio.zmod
│       └── monitor.zex
│
├── abi/
│   └── zos/
│       ├── types.h, errno.h, syscall.h
│       └── fs.h, process.h, diag.h
│
├── user/
│   ├── sdk/
│   │   ├── include/ (zos.h, zos/, zlibc/)
│   │   ├── src/ (crt0.asm, runtime.asm, gates, args/fs/exec, zlibc/)
│   │   ├── zex.mk
│   │   └── template/hello.c
│   ├── helloworld/
│   │   ├── hello.c
│   │   ├── Makefile
│   │   └── manifest.json
│   └── monitor/
│       ├── monitor.c/.h
│       ├── monitor_mem.c
│       ├── monitor_io.c
│       └── hexfmt/hexdump/status
│
├── tests/
│   ├── modules/
│   │   └── sio_irq_module.c, sio_fail_module.c, *.json
│   └── target/
│       ├── m1/ .. m7/
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

Directories for later facilities are added only as each milestone requires them.

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

### `abi/zos/`

Owns the public native ABI: the one copy of every header a native program
or module may compile against (`types.h`, `errno.h`, `syscall.h`, `fs.h`,
`process.h`, `diag.h`).

Headers under `include/zephyros/` that used to carry those declarations
now include the `abi/zos/` header and add only kernel-internal detail, so
there is exactly one definition of every packed block, constant and
equate.  The kernel build, the module build and the user build all get
`-I abi`; nothing else is shared.

### `user/`

Owns native ZephyrOS programs: the monitor (`user/monitor/`), the demos
(`user/helloworld/`) and, from M10, the shell (`user/ZShell/`).  User
programs consume the public ABI through the SDK and never reach into
private kernel symbols or driver internals.

### `user/sdk/`

Owns the user SDK: the only supported way to build a native program.

- `user/sdk/include/` - `zos.h` (the umbrella a program includes) and
  the `zos/` and `zlibc/` headers;
- `user/sdk/src/` - crt0, libzos (console/diag/FS/exec gate stubs and
  wrappers) and `zlibc/`;
- `user/sdk/zex.mk` - the generic `zex_program` rule: one line per
  program, manifest generated from the link map, `zexpack` validation;
- `user/sdk/template/` - the copyable minimal program.

A program or sub-project (`user/helloworld/`, `user/ZShell/`) is built
only from this directory plus `abi/`; it never includes a kernel header
and never writes entry assembly (`docs/sdk.md`).

### `linker/`

Owns link placement and section layout.

The linker may consume values derived from the platform description, but linker scripts are not executable target code.

### `tools/`

Owns host-side build, validation, packing, and inspection tools.

These rules apply equally to the early milestones and later native ZephyrOS work.

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
D4     = 0  ROM enabled
D3     = 0  normal/boot mapping
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

The four hardware modes select complete memory mappings; no single bit
is a feature flag ("decoder revision 12" semantics, matching the
Zephyr-80 machine reference "Memory Management":

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

Only mode 00 selects ROM, and it selects ROM for **every** read; a
stack cannot survive it, because a `PUSH` writes SRAM while the matching
`POP` would read ROM.  08h does **not** expose ROM (earlier revisions of
this document called it "shadow/copy" and claimed a ROM window; that was
wrong — see 24.2 for the ROM-access pattern this actually requires).

Runtime mode values for application bank `N` are therefore:

```text
mode 10: 10h | N
mode 11: 18h | N
```

Reads from port `00h` return the current latch value and may be used by the kernel banking implementation when preserving or validating the active bank. The port number itself comes from the platform description.

### 5.2.3 Initial ROM-to-RAM handoff

M0 should reuse the proven Zephyr-80 ROM-to-RAM boot mechanism rather than invent a second bootstrap architecture.

The minimal required sequence is:

```text
RESET
  │ latch = 00h (mode 00: ROM page 0 everywhere on reads)
  ▼
ROM page 0 bootstrap (stackless between latch changes)
  │
  ├── DI
  ├── one full-page LDIR: ROM page 0 -> SRAM bank 0      (latch 00h)
  ├── latch 27h: ROM page 1, mode 00, SRAM bank 7
  ├── one full-page LDIR: ROM page 1 -> SRAM bank 7
  ├── latch 10h: mode 10 over bank 0
  │      (both boot pages carry byte-identical bootstrap bytes at the
  │       switch address, so the fetch after each OUT is identical)
  │
  ▼
common transition code in E000h-FFFFh
  │
  ├── write 18h to port 00h
  │      mode 11 over caller bank 0
  │
  ▼
bank 7 visible at 2000h-DFFFh
  │
  ▼
kernel entry
```

Mode 00 answers every read (including instruction fetch) from the
selected ROM page, so the copy loops must be stackless and may only
change the latch into an address whose ROM bytes are identical to the
running code.  The two boot pages satisfy that by sharing a
byte-identical low bootstrap region.

For M0/M1:

```text
bank 7:
    2000h-BFFFh   loadable kernel image
    C000h-DFFFh   zero-initialized runtime area / stacks / scratch

bank 0:
    E000h-FFFFh   permanent common code/state
```

The exact amount of `C000h-DFFFh` used at M0/M1 is linker-defined; the
region is zero-initialized at boot and holds runtime state, so it must
not depend on ROM-initialized content.

The transition from mode 10 to mode 11 must execute from the permanent common range `E000h-FFFFh`. Code executing in `2000h-DFFFh` would change underneath the CPU when `D3` is changed.

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
  │
  ▼
controlled HALT loop
```

The boot path must be deterministic and must not depend on state left by CP/M, ZSDOS, ZCPR, or the existing BIOS.

## 5.3 Kernel entry contract

`start.asm` owns the transition from firmware/boot state into the C kernel.

Before calling `kernel_main()` it must guarantee:

- interrupts are disabled,
- the CPU is in a documented memory mode,
- the kernel image is visible at its linked addresses,
- the stack pointer is valid,
- any C runtime state required by SDCC has been initialized,
- no assumptions are made about previous firmware register contents.

The kernel entry function is:

```c
void kernel_main(void);
```

It does not return.

Returning from `kernel_main()` is considered a kernel fault and should eventually invoke `panic()`.

---

## 5.4 Early serial console

The M0 serial implementation directly uses Z80 SIO channel B.

Hardware configuration:

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

Channel B is the only console device required by M0/M1.

The known-good initialization sequence is:

```text
OUT (23h),18h            ; WR0: channel reset

OUT (23h),04h            ; select WR4
OUT (23h),44h            ; x16, 1 stop bit, no parity

OUT (23h),03h            ; select WR3
OUT (23h),C1h            ; RX enable, 8-bit RX, auto-enables disabled

OUT (23h),05h            ; select WR5
OUT (23h),EAh            ; DTR, TX 8-bit, TX enable, RTS

OUT (23h),01h            ; select WR1
OUT (23h),00h            ; interrupts disabled
```

Although M0 only requires output, receive is initialized to the current known-good console configuration so M0 and later diagnostic code share one hardware setup.

SIO RR0 status bits required by the early console are:

```text
01h   RX character available
04h   TX buffer empty
```

Transmit operation:

```text
poll IN (23h) until RR0 & 04h != 0
OUT  (22h),character
```

The initial interface is:

```c
void early_console_init(void);
void early_console_putc(char ch);
void early_console_puts(const char *s);
```

No input API is required by M0, although a private polling receive routine may exist for hardware bring-up.

No formatting library is required.

No `printf()` dependency is permitted.

The early-console implementation should live in, or be callable entirely through, the permanent common region so that diagnostic output remains available during mode/bank transitions and kernel failures.

The early console remains available after later console/device subsystems are introduced.

## 5.5 M0 failure behavior

At M0 there is no recovery infrastructure.

A fatal error should:

1. print the clearest available diagnostic through the early console,
2. disable interrupts,
3. enter a halt loop.

Conceptually:

```c
void panic(const char *message);
```

The initial implementation may be extremely small.

---

## 5.6 M0 non-goals

M0 explicitly does **not** include:

- interrupt handling
- IM2 setup
- syscall implementation
- device registry
- dynamic drivers
- process management
- memory allocation
- filesystem support
- IOC services
- CP/M compatibility
- `.COM` loading
- `.ZEX` loading
- shell or userland
- keyboard input

M0 should stay small enough that a failure can be debugged from the generated map file, disassembly, and serial output alone.

For automated validation before hardware execution, the build may use host-side checks such as:

- linker/map validation;
- binary-layout checks;
- disassembly inspection;
- unit tests for host-side tooling;
- a CPU-level Z80 simulator for isolated code where useful.

These checks are supplementary. They do not replace hardware validation of the memory-mode transitions or SIO console.

---

# 6. Milestone M1 — Kernel foundation

## 6.1 Objective

M1 establishes the execution environment that future kernel subsystems can rely on.

M1 is complete when ZephyrOS has:

- a documented kernel memory map,
- a deterministic bank/memory mode at kernel entry,
- explicit ownership of page zero,
- a permanent kernel/common region,
- defined stack regions,
- a reserved native syscall gateway,
- basic platform banking primitives,
- build-time validation that kernel regions do not overlap.

M1 still does not need a useful syscall implementation.

---

# 7. M1 memory model

## 7.1 Physical banks versus kernel abstractions

The hardware bank/mode register is I/O port `00h`.

The M1 kernel banking implementation owns all direct access to this latch. Other kernel subsystems must not issue raw `IN` or `OUT` instructions to port `00h`; they call the banking mechanism instead. The latch address and bit definitions come from `platform/zephyr80/`.

Required constants:

```text
MEMCTL_PORT       = 00h

BANK_MASK         = 07h
RAM_SHADOW        = 08h
ROM_DIS           = 10h
ROM_PAGE_MASK     = E0h

MODE_BOOT         = 00h
MODE_SHADOW       = 08h
MODE_APPLICATION  = 10h
MODE_OS           = 18h
```

For runtime application bank `N`, where `N = 0..6`:

```text
application mapping = MODE_APPLICATION | N
OS mapping          = MODE_OS          | N
```

Bank `7` is reserved to ZephyrOS and must not be exposed as an ordinary application bank.

The minimum platform API for M1 should provide semantics equivalent to:

```c
uint8_t memctl_read(void);
void memctl_write(uint8_t value);

uint8_t bank_get(void);
void enter_application_mode(uint8_t bank);
void enter_os_mode(uint8_t bank);
```

The exact C/assembly signatures are not yet ABI and may be adjusted during implementation.

`enter_os_mode()` and `enter_application_mode()` must preserve the selected application-bank identity in `D0-D2`. Entering or leaving the OS changes the mode bit, not the identity of the suspended caller.

For example:

```text
application bank 3     13h
OS over bank 3         1Bh
application bank 3     13h
```

Mode-changing primitives must execute from an address that remains mapped before and after the transition. For M1 this means the permanent common range `E000h-FFFFh`.

M1 does **not** yet define the later application memory allocator or far-memory API.

## 7.2 Kernel-visible regions

ZephyrOS carries forward the proven **mode 10 / mode 11** execution model from the current Zephyr-80 OS.

The model reserves:

- SRAM banks `0-6` for application/process banks,
- SRAM bank `7` for the OS body,
- the top 8 KiB of physical SRAM bank `0` as permanently visible common memory.

### Mode 10 — application execution

Mode `10` is the normal application execution view.

```text
FFFFh  ┌────────────────────────────────────┐
       │ bank 0 common             8 KiB    │
E000h  ├────────────────────────────────────┤
       │                                    │
       │ selected application bank N        │
       │                                    │
0000h  └────────────────────────────────────┘
```

Mapping:

```text
0000h-DFFFh  selected application bank N
E000h-FFFFh  physical SRAM bank 0
```

The selected bank number remains the identity of the running application/process.

This mode gives the application a contiguous 56 KiB private view while keeping the permanent ZephyrOS common region visible.

### Mode 11 — OS execution

Mode `11` is entered when the kernel needs access to the main OS body.

```text
FFFFh  ┌────────────────────────────────────┐
       │ bank 0 common             8 KiB    │
E000h  ├────────────────────────────────────┤
       │                                    │
       │ bank 7 OS body            48 KiB   │
       │                                    │
2000h  ├────────────────────────────────────┤
       │ caller bank N              8 KiB   │
0000h  └────────────────────────────────────┘
```

Mapping:

```text
0000h-1FFFh  selected application bank N
2000h-DFFFh  physical SRAM bank 7
E000h-FFFFh  physical SRAM bank 0
```

Mode `11` therefore exposes the kernel while preserving the low 8 KiB of the caller's address space.

### Bank-7 layout inside mode 11

The 48 KiB `2000h-DFFFh` window is not fixed internally.  The build
derives the boundary between the loadable kernel, the driver arena and
the runtime data (package 25b):

```text
2000h                     kernel code, then __kernel_code_end
__driver_arena_start      loaded ZMOD modules (derived, >= 32 granules)
data_base = arena end     _DATA / _INITIALIZED (derived base)
DF00h                     fixed 256-byte ISR stack
E000h                     end of bank 7 in mode 11
```

`data_base = align_down(DF00h - (l__DATA + l__INITIALIZED), 512)` and the
arena runs from the aligned linked-kernel end up to that base, so kernel
and module growth consumes arena space rather than a fixed `C000h`
reservation.  `pack_image.py` and the arena plan report the single
bank-7 budget ledger (section 32.39).

The bank selector continues to identify the suspended/running application bank while mode `11` is active; switching to OS mode must not destroy that identity.

### Architectural consequences

This memory model is a deliberate ZephyrOS invariant.

The three important regions are:

```text
0000h-1FFFh  caller-visible low-memory window
2000h-DFFFh  application memory in mode 10 / OS bank 7 in mode 11
E000h-FFFFh  permanent common region in physical bank 0
```

The low 8 KiB window allows the kernel to inspect and operate on process-local low memory without copying or remapping it separately. This is particularly useful for page zero, syscall arguments, command/process state, and later CP/M compatibility structures.

The permanent `E000h-FFFFh` region contains only code and state that must remain reachable across both modes, such as:

- mode/bank transition code,
- kernel entry and return gates,
- critical runtime state,
- future interrupt-entry infrastructure,
- fatal diagnostic paths.

The bulk of the OS should reside in bank 7 and become visible only in mode `11`.

This avoids consuming a large permanent common-memory area while still allowing the kernel to enter and leave an application's context safely.

Two clarifications, both learned by having to fix them:

- "Permanent" is about *reachability across a mapping change*, not about
  isolation.  Mode 10 maps this region as readily as mode 11 does; there
  is no protection anywhere in the machine, and an application can read
  and write it.  Anything whose whole lifetime is mode 11 (for example the
  `SYS_EXEC` path buffer, package 28 correction-01) belongs in bank 7.
- Because it is the scarcest region, its use is measured.  The budget
  ledger carries a `common: used <n> of <n> bytes (code <n>, data <n>) |
  ceiling <n>, m10 <n>, limit <n>` line beside the `bank7:` line
  (docs/memory.md section 2.2.1).  The architectural ceiling on the packed
  `_COMMON` + `_COMMON_DATA` run is `ZOS_COMMON_CEILING` (3072 bytes), with
  `ZOS_COMMON_M10_RESERVATION` (512 bytes) held back for M10's far-call
  gate and per-process far state (target architecture 2.13-2.15); M9
  enforces `used <= ceiling - reservation` (2560 bytes).  `mapcheck.py`
  fails the build when common use exceeds it.  Neither value may be raised
  to make a package fit.

M1 should preserve this mapping rather than introduce a new generic kernel memory model.

---

# 8. Page-zero ownership

Page zero is part of the execution environment selected by ZephyrOS.

For native ZephyrOS execution, ZephyrOS owns `0000h-00FFh`.

M1 reserves a native syscall vector, tentatively:

```text
RST 30h
```

at address:

```text
0030h
```

The page-zero entry remains tiny:

```asm
.org 0x0030
    jp zos_syscall_gate
```

`zos_syscall_gate` must be located in permanent common memory (`E000h-FFFFh`), not directly in the bank-7 kernel body.

This is required because an `RST` occurs while mode `10` is active. The common gate is responsible for safely transitioning into mode `11` before dispatching into bank 7.

The choice of `RST 30h` is provisional until debugger/monitor and other low-page requirements have been reviewed.

The actual syscall dispatcher must not live in page zero.

# 9. Native syscall ABI scaffold

M1 reserves the mechanism but does not require useful services.

Provisional native ABI:

```text
Entry:
    RST 30h

Input:
    A  = syscall number
    HL = pointer to syscall-specific argument block

Return:
    Carry clear = success
    Carry set   = error

    HL = primary result
    A  = error code when Carry is set
```

The initial M1 dispatcher may simply report an unimplemented syscall through the early console and return an error or panic.

The syscall mechanism is only for native ZephyrOS execution.

Future CP/M `.COM` processes will receive a separate CP/M-compatible page-zero environment, including `CALL 0005h`, rather than being required to use the native `RST` ABI.

---

# 10. Common kernel entry

The native syscall vector enters a permanent common-memory gate.

The critical constraint is that an application stack may live anywhere in `0000h-DFFFh`. If it lies in `2000h-DFFFh`, that memory becomes bank 7 as soon as mode `11` is entered.

Therefore the syscall gate must not continue using the caller's stack after changing memory mode.

Required entry sequence, conceptually:

```text
application, mode 10
    │
    │ RST 30h
    │ CPU pushes return PC on caller stack
    ▼
0030h: JP zos_syscall_gate
    │
    ▼
common memory, still mode 10
    │
    ├── save caller SP in common state
    ├── switch SP to the common gate stack
    ├── save registers required by the ABI
    ├── preserve caller bank N
    ├── write 18h|N to port 00h
    │
    ▼
mode 11
    │
    ├── bank 7 visible at 2000h-DFFFh
    └── dispatch to bank-7 kernel code
```

Return sequence:

```text
bank-7 kernel returns to common gate
    │
    ├── restore common-saved state as required
    ├── write 10h|N to port 00h
    │
    ▼
mode 10
    │
    ├── caller memory is visible again
    ├── restore caller SP
    └── RET
```

The return address pushed by `RST` may remain on the caller's original stack while the kernel runs. The kernel need not access it while in mode `11`; it becomes visible again before the caller SP is restored.

M1 does not yet require a complete syscall dispatcher. It does require this transition path to be demonstrably safe.

# 11. Kernel stacks

M0/M1 require two conceptually distinct stacks.

### Kernel stack

The normal kernel stack is private to bank 7 and is used only after mode `11` has been established.

It should be allocated from zero-initialized/runtime memory in bank 7, preferably within:

```text
C000h-DFFFh
```

This region is not required to be loaded from ROM.

### Common gate stack

M1 also reserves a small stack in physical bank 0 common memory:

```text
E000h-FFFFh
```

This stack exists specifically for operations that cross between mode `10` and mode `11`.

The common gate stack must remain mapped in both modes.

The syscall/mode-transition gate must switch from the caller stack to this stack **before** changing the memory mode.

Exact stack addresses and sizes are linker-defined in M1 and must be exported in the generated map.

Later milestones may introduce separate interrupt, process, or driver stacks. Those are not part of M1.

# 12. Linker and memory-map validation

The linker configuration is part of the architecture.

The M1 build must generate a map file.

A host-side validation tool should fail the build if:

- `platform/zephyr80/` contains target executable `.c` or `.asm` translation units;
- platform include files contain instruction-emitting routines rather than constants/configuration;
- page-zero content exceeds `0000h-00FFh`,
- the native RST stub is not at its required page-zero vector,
- common transition code is placed outside `E000h-FFFFh`,
- the early serial/panic path cannot execute entirely from permanently mapped memory,
- the common gate stack overlaps common code or state,
- the bank-7 loadable image extends to or beyond `C000h`,
- bank-7 runtime stacks/state overlap loaded code/data,
- kernel entry is outside the bank-7 OS-visible region,
- banked sections exceed their assigned region,
- entry points fall outside their expected regions.

Initial host tool:

```text
tools/mapcheck.py
```

Implemented (M1, fixed): `tools/mapcheck.py` is the authoritative post-link
layout check.  It derives every boundary from the declarations in
`platform/zephyr80/memory.inc` (never a second copy of the map), is invoked
automatically by the default build after linking and before the ROM is
considered complete, and fails with precise symbol/region diagnostics for
each condition above.  `tools/pack_image.py` independently enforces the
byte-level image contract (exact size, FF fill, reset-copier mirroring,
page ownership, the three-byte `RST 30h` stub, no mapping-changing code in
bank 7, no conflicting/ambiguous IHX input) and emits
`build/zephyros.manifest.json` with sizes, hashes, fill counts, and entry
addresses.  `make check` additionally proves a clean rebuild reproduces
the identical binary and runs the whole host validation stack;
`tools/serial_smoke.py` (`make smoke-hardware`) captures and verifies the
physical serial output.  The acceptance record lives in
`docs/validation/`.

The purpose is to turn memory-layout violations into build errors rather than hardware debugging sessions.

---

# 13. Assembly/C boundary

Assembly is expected for code that directly depends on Z80 machine state:

- reset/startup
- port I/O primitives
- banking primitives
- page-zero gates
- future syscall entry/exit
- future interrupt entry/exit
- future far-call support

C is preferred for policy and higher-level kernel logic.

M0/M1 C code includes:

- `kernel_main()`
- panic handling
- simple initialization logic
- later syscall dispatch policy

Stable ZephyrOS interfaces must not expose SDCC's internal calling convention.

Assembly wrappers should isolate compiler conventions from ZephyrOS-defined ABIs.

---

# 14. Interrupt state at the M1 boundary

M0 and M1 complete with maskable interrupts disabled.

M2 takes ownership of interrupt mode, the IM2 vector page, IRQ entry/return, source registration, and ISR stack policy.

No M0/M1 component establishes its own permanent interrupt environment.

---

# 15. Driver state at the M1 boundary

M0's serial support is an **early console**, not a registered driver.

M1 has no device registry and no driver ABI.

M3 introduces:

```text
hardware
   │
driver
   │
device instance
   │
capabilities / service binding
```

The early serial console remains outside that mechanism permanently as a panic and bring-up path.

---

# 16. Relationship to future CP/M compatibility

M0 and M1 are native ZephyrOS only.

Later, launching a CP/M `.COM` executable will construct a CP/M-compatible execution environment with its own page zero, including entries such as:

```text
0000h   warm-boot/process-exit gate
0005h   BDOS compatibility gate
005Ch   default FCB
006Ch   second default FCB
0080h   command tail / default DMA area
0100h   COM program
```

That compatibility environment must not determine the native ZephyrOS ABI.

---

# 16.1 Emulator validation

ZephyrOS must not assume that a complete Zephyr-80 emulator exists.

M0/M1 correctness depends on platform-specific behavior, especially:

- the port-`00h` banking latch;
- the mode `10` / mode `11` memory mapping;
- bank-7 substitution over `2000h-DFFFh`;
- permanent bank-0 common memory at `E000h-FFFFh`;
- SIO channel-B register and polling behavior.

An emulator is only a valid milestone test target when those behaviors are implemented accurately enough to exercise the same boot and transition paths as hardware.

Until then:

```text
authoritative target     physical Zephyr-80
required static checks   build, link map, binary layout
optional dynamic checks  CPU simulator / partial emulator where applicable
future integration test  MAME once the Zephyr machine model is complete
```

MAME support is therefore a useful later integration target, not a prerequisite for M0 or M1 completion.

---

# 17. M0/M1 acceptance criteria

## M0

M0 is accepted when:

- the kernel links reproducibly,
- the ROM/bootstrap reaches the ZephyrOS bank-7 image using the documented port-`00h` memory-control sequence,
- the kernel executes in mode `11`,
- the generated image boots on physical Zephyr-80 hardware,
- SIOB is initialized on ports `22h/23h` as 115200 8N1 polling I/O,
- the expected serial banner appears without requiring interrupts,
- the kernel enters a controlled halt loop,
- no external runtime library is required.

MAME is **not** an M0 acceptance dependency. Emulator validation may be used when the Zephyr-80 MAME machine/device implementation models the memory decoder, banking latch, and SIO behavior required by this milestone closely enough to be meaningful.

## M1

M1 is accepted when:

- the memory map is documented,
- the linker enforces the intended memory placement,
- the kernel banking implementation can read/write the memory latch at the platform-declared canonical port `00h`,
- application banks `0-6` can enter mode `10` as `10h|N`,
- mode `11` can be entered as `18h|N` without losing caller-bank identity,
- a mapping test demonstrates that:
  - `0000h-1FFFh` remains the selected caller bank in both modes,
  - `2000h-DFFFh` changes between caller bank and bank 7,
  - `E000h-FFFFh` remains physical bank 0,
- bank 7 cannot be selected as a normal application bank,
- page zero is under ZephyrOS control,
- the native syscall vector is installed,
- syscall entry moves to the common gate stack before entering mode `11`,
- the syscall gate can enter bank 7 and return safely to the original caller stack in mode `10`,
- invalid/unimplemented syscalls fail predictably,
- the early serial console remains usable before, during, and after mode transitions and from kernel fault paths,
- map validation catches region overlap and invalid-placement errors during the build.

---

# 17.1 M0/M1 hardware reference

This table is the implementation reference for hardware constants required before M2.

```text
Memory control
--------------
Port 00h       banking/memory latch (canonical address; hardware decodes 00h-0Fh)

D0-D2          SRAM bank
D3             memory-mode bit 0 (MEM_MODE0)
D4             memory-mode bit 1 (MEM_MODE1)
D5-D7          ROM page

00h            mode 00: ROM reads everywhere / writes to the selected
               SRAM bank (stackless-only; used by the reset copier and
               the ROM-access stub)
08h            mode 01: flat selected SRAM bank (no ROM)
10h|N          application mode, bank N
18h|N          OS mode over caller bank N

Valid application N: 0..6
Bank 7:        reserved to ZephyrOS


Early serial console
--------------------
20h            SIO A data       (unused M0/M1)
21h            SIO A control    (unused M0/M1)
22h            SIO B data
23h            SIO B control

Clock           1.8432 MHz
Async mode      x16
Format          115200 8N1
RR0 bit 0       RX available
RR0 bit 2       TX buffer empty

Init:
23h <- 18h
23h <- 04h, 44h
23h <- 03h, C1h
23h <- 05h, EAh
23h <- 01h, 00h
```

No other hardware interface is required for M0/M1 completion.

---

# 18. Milestone M2 — Interrupt executive

## 18.1 Objective

M2 establishes a single interrupt architecture owned by ZephyrOS.

M2 is complete when:

- ZephyrOS owns the Z80 `I` register and interrupt mode;
- a common-memory IM2 table is installed;
- an interrupt can arrive while either mode `10` or mode `11` is active;
- the IRQ entry path preserves the interrupted context;
- the executive can enter bank 7 safely, invoke a registered handler, restore the previous mapping and stack, and return with `RETI`;
- a real CTC0 interrupt is demonstrated on physical hardware.

M2 does not introduce nested interrupts, preemption, scheduling, or application-owned vector tables.

---

## 18.2 Interrupt ownership

The interrupt architecture belongs to the kernel.

Drivers may own an **interrupt source**, but they do not own:

- the `I` register;
- the IM2 vector page;
- the global interrupt mode;
- the common IRQ entry path;
- the ISR transition stack;
- the final `RETI`.

A driver requests a source through the IRQ executive and provides a bank-7 handler.

Conceptually:

```c
typedef uint8_t irq_source_t;

int irq_claim(
    irq_source_t source,
    void (*handler)(void *context),
    void *context
);

int irq_release(irq_source_t source);
```

These signatures are internal kernel interfaces, not a frozen public ABI.

Only one owner may claim a source at a time.

Registry mutation occurs with maskable interrupts disabled.

---

## 18.3 IM2 vector page

M2 uses Z80 IM2.

The vector page must:

- live in permanent common memory;
- be 256-byte aligned;
- reserve **257 bytes**, because vector `FFh` reads one byte beyond the nominal 256-byte page;
- remain visible in both mode `10` and mode `11`.

The initial ZephyrOS placement is:

```text
FD00h-FDFFh   IM2 vector page
FE00h         FFh-vector guard byte

I = FDh
```

The linker and `mapcheck.py` must treat the page plus guard as one constrained object.

Every vector used by ZephyrOS must resolve to common-memory IRQ entry code.

### FFh vector invariant

The Zephyr expansion hardware includes sources that may present `FFh` on the data bus when interrupting. The V9958 path is the important example.

For IM2 vector `FFh`, the CPU fetches the handler pointer from:

```text
FDFFh   low byte of target pointer
FE00h   high byte of target pointer
```

The build must therefore verify the actual linked image identity:

```text
word_at(FDFFh) == address_of(irq_ff_unexpected)
```

The guard byte must be derived from the linked target, not maintained as an unrelated literal.

This invariant is a build-time error if it is violated.

M2 need not enable the V9958 interrupt yet; it must make the `FFh` path safe before interrupts are globally enabled.

---

## 18.4 Platform IRQ sources

The executive uses logical source IDs. Hardware vector values, port numbers, and board channel mappings are declared as constants in `platform/zephyr80/irq.h` / `irq.inc`. Active mapping and dispatch logic remains in `kernel/irq/`.

The initial source namespace should reserve at least:

```text
IRQ_CTC0
IRQ_CTC1
IRQ_CTC2
IRQ_CTC3
IRQ_SIO0
IRQ_SIO1
IRQ_VDP_FF
```

Not all sources need to be enabled in M2.

The source ID is not required to equal an IM2 vector byte.

This distinction is important because physical port numbering and logical channel numbering are not guaranteed to be identical on the Zephyr I/O board.

---

## 18.5 CTC bring-up source

M2 uses **CTC physical channel 0 only** as its first real interrupt source.

Known hardware:

```text
CTC channel 0 port       40h
Z80 CPU/system clock     10.000 MHz
CTC daisy-chain order    CTC -> SIO0 -> SIO1
```

The other CTC channel ports must not be inferred as `40h + logical_channel` until the board mapping has been explicitly validated. The current board wiring may exchange physical channels 1 and 2 at ports `41h` and `42h`.

For M2 acceptance, this ambiguity is avoided by using port `40h`, which is physical CTC channel 0.

A simple test configuration may use:

```text
vector base     00h
control word    A7h
time constant   00h   ; interpreted by the CTC as 256
```

At 10 MHz in timer mode with prescaler 256:

```text
interrupt_rate = 10,000,000 / (256 * 256)
               ≈ 152.59 Hz
```

The acceptance test need only prove repeated, stable interrupts and correct context restoration. It does not establish a permanent system tick rate.

Writing `03h` to the CTC channel resets it with interrupts disabled and is suitable for test teardown.

---

## 18.6 Common IRQ entry

An interrupt may occur while an application is executing in mode `10`.

At that instant the CPU pushes the interrupted PC onto the application's current stack. That stack may be in `2000h-DFFFh`, which will cease to be application memory when mode `11` is entered.

Therefore the IRQ entry path follows the same fundamental crossing rule as the syscall gate:

> The caller stack must be abandoned before changing the memory mapping.

Conceptual entry:

```text
interrupt accepted
    │
    │ CPU pushes PC on interrupted stack
    ▼
IM2 common source stub
    │
    ├── capture source identity without losing caller context
    ├── save interrupted SP in common state
    ├── switch SP to common IRQ transition stack
    ├── save full required CPU context
    ├── read and save port-00h latch value
    │
    ├── if saved mode is 10:
    │       write MODE_OS | saved_bank
    │
    ▼
mode 11 active
    │
    ├── switch to bank-7 ISR stack
    ├── dispatch registered source handler
    └── return to common IRQ gate
```

Return:

```text
common IRQ gate
    │
    ├── return to common transition stack
    ├── restore the exact saved port-00h mapping
    ├── restore CPU context
    ├── restore interrupted SP
    └── RETI
```

If the interrupt arrived while mode `11` was already active, the saved mapping is still restored exactly; the gate does not blindly force mode `10`.

---

## 18.7 Context preservation

M2 IRQ delivery is transparent to interrupted code.

The common IRQ wrapper must preserve all programmer-visible register state that a normal foreground program may depend upon, including:

```text
AF BC DE HL
IX IY
alternate AF'
alternate BC' DE' HL'
```

The `I` register is kernel-owned and remains set to the ZephyrOS IM2 page.

The `R` register is not treated as application state.

Individual bank-7 handlers follow a normal kernel-internal calling convention because the assembly wrapper owns full-context preservation.

---

## 18.8 IRQ stacks

M2 introduces two IRQ stack roles.

### Common IRQ transition stack

A small stack in physical bank 0 common memory.

It is used only while:

- leaving the interrupted address space;
- saving/restoring the architectural register frame;
- changing the memory mapping;
- transferring to/from the bank-7 ISR stack.

It must remain mapped in both modes.

### Bank-7 ISR stack

A larger private stack in bank 7.

Device handlers execute on this stack after mode `11` is active.

This is deliberate: driver handlers added by M3/M4 may be ordinary bank-7 code and should not consume scarce common memory.

M2 does not allow nested maskable interrupts. Handlers run with maskable interrupts disabled until the executive executes `RETI`.

---

## 18.9 Source stubs and handler location

The finite platform IRQ sources have small source-identifying stubs in common memory.

These stubs are part of the **interrupt executive**, not part of individual drivers.

They transfer control to the generic common IRQ gate with a logical `irq_source_t`.

The registered handler itself normally resides in bank 7:

```text
IM2 vector
    │
common source stub
    │
common IRQ gate
    │ mode 11
    ▼
bank-7 registered handler
```

This rule prevents a driver's ISR-reachable implementation from being accidentally executed out of an application bank.

A driver may not install arbitrary code directly into the IM2 page.

---

## 18.10 Handler contract

An IRQ handler:

- owns servicing/clearing its device-specific interrupt condition;
- must be bounded;
- must not busy-wait for long operations;
- must not call interfaces that can sleep or recursively depend on the same interrupt source;
- returns normally to the IRQ executive rather than executing `RETI`.

The executive owns the final `RETI` so the Z80 daisy chain is released correctly.

Unexpected or unclaimed interrupts must not print large diagnostics from interrupt context. The executive records a compact diagnostic counter/source record and, where a safe platform quiesce operation exists, disables the offending source.

---

# 19. M2 acceptance criteria

M2 is accepted when all of the following pass on physical Zephyr-80 hardware:

- `I = FDh` and IM2 is active after IRQ initialization;
- `FD00h-FE00h` is validated as the IM2 page plus guard;
- the `FFh` vector resolves to `irq_ff_unexpected` in the final linked image;
- CTC0 at port `40h` repeatedly interrupts through the ZephyrOS executive;
- a handler registered for `IRQ_CTC0` receives the interrupts;
- the handler executes in mode `11`;
- the interrupted bank identity is preserved;
- interrupts arriving from mode `10` return to the same mode, bank, SP, PC, and register state;
- interrupts arriving from mode `11` return to the prior mode-`11` context;
- CTC0 can be disabled cleanly after the test;
- the early serial console still works before and after the interrupt test;
- deliberately unclaimed test interrupts take the unexpected-source path without jumping into arbitrary memory;
- linker/map validation rejects a misaligned vector page or broken `FFh` guard relationship.

CPU-level emulation may exercise context save/restore logic, but physical hardware remains authoritative for interrupt acknowledgement, daisy-chain behavior, and CTC timing.

---

# 20. Milestone M3 — Device registry and static drivers

## 20.1 Objective

M3 introduces the ZephyrOS device/driver model while keeping every driver statically linked into the kernel image.

M3 proves the architecture without yet introducing module relocation or runtime loading.

The first migrated device is SIO0/B, because it is already known-good hardware and can demonstrate normal console output without making the kernel dependent on a more complex device.

---

## 20.2 Driver, device, capability, and service

ZephyrOS distinguishes four concepts.

```text
driver
    implementation code for a hardware family

device
    one registered hardware instance

capability
    operation(s) that device can provide

service
    OS-level binding that chooses device(s) to satisfy a role
```

For example:

```text
SIO driver
    │
    └── sio0.b
          ├── SERIAL
          ├── CONSOLE_IN
          └── CONSOLE_OUT
```

A future V9958 device may provide:

```text
VIDEO
CONSOLE_OUT
```

while a future HID device may provide:

```text
HID
CONSOLE_IN
```

Therefore a console is not assumed to be one physical device. Input and output may bind to different devices.

---

## 20.3 Device classes and capabilities

The registry uses a small class identifier plus a capability bitmask.

Initial classes should include:

```text
ZDEV_SERIAL
ZDEV_VIDEO
ZDEV_STORAGE
ZDEV_HID
ZDEV_IOC
ZDEV_OTHER
```

> **M8 update (section 30.3).** The M3 placeholder names `ZDEV_INPUT`
> and `ZDEV_TRANSPORT` were refined to `ZDEV_HID` (4) and `ZDEV_IOC`
> (5); the numeric slots are unchanged. Driver ABI v2 makes
> `capabilities` a 32-bit mask with documented per-class ranges
> (generic 0-3, VIDEO 8-14, HID 16-18, IOC 24-27).
>
> **M9 update (section 32).** `ZDEV_STORAGE` is renamed directly to
> `ZDEV_BLOCK`, preserving the existing numeric class value. M9 does not
> retain a second storage alias as an architectural interface.

Initial capability bits should include only what is needed by real drivers, beginning with:

```text
ZCAP_CHAR_IN
ZCAP_CHAR_OUT
ZCAP_CONSOLE_IN
ZCAP_CONSOLE_OUT
```

Later milestones may add block I/O, graphics, event, or transport-specific capabilities without changing the basic registry model.

Clients should prefer capability queries over testing a specific driver name.

---

## 20.4 Device registry

M3 uses a fixed-size registry allocated by the kernel.

No heap allocator is required.

Conceptually (updated for driver ABI v2; see section 30.3):

```c
typedef uint8_t zdev_handle_t;
typedef uint32_t zdev_caps_t;

struct zdevice {
    zdev_handle_t          handle;
    uint8_t                class_id;
    zdev_caps_t            capabilities;
    const char            *name;
    const struct zchar_ops *char_ops;  /* optional generic interface */
    const void            *class_ops;  /* type by class_id           */
    void                  *state;
    const void            *driver;
};
```

The exact C layout is internal, pinned to the driver ABI version, and asserted at compile time.

Required registry operations are semantically equivalent to:

```c
void device_registry_init(void);

int device_register(const struct zdevice_desc *desc,
                    zdev_handle_t *out_handle);

const struct zdevice *device_get(zdev_handle_t handle);

zdev_handle_t device_find(uint8_t class_id,
                          zdev_caps_t required_caps,
                          zdev_handle_t after);

int device_interface(zdev_handle_t handle,
                     uint8_t expected_class,
                     const void **class_ops,
                     void **state);
```

Rules:

- handles are kernel-issued;
- handle `0` is reserved as invalid;
- registration order must not be treated as a permanent external ABI;
- duplicate instance registration is rejected;
- registry overflow is an explicit error;
- the registry stores references to driver-private state; it does not absorb that state.

---

## 20.5 Driver descriptor

> **Retirement notice (stage 10).** The static driver table described in
> sections 20.5-20.7 (`struct zdriver`, `boot_drivers[]`,
> `driver_init_static()`) is **retired**. Since M4 the kernel boots only
> embedded ZMOD drivers; production images link no static driver table.
> These sections are retained as M3 milestone history and must not be read
> as the current production tree. The `zdriver_api` contract itself is
> still current: it is the interface the loader passes to a loaded module
> (`drivers/sio/sio_module.c` is the production example).

A statically linked driver exposes a descriptor and initialization entry.

Conceptually:

```c
struct zdriver {
    const char *name;
    uint8_t     abi_version;
    int       (*init)(const struct zdriver_api *api);
};
```

A driver may register zero, one, or several device instances.

Driver initialization failure is reported to the kernel and does not corrupt already registered devices.

M3 does not require driver unload.

---

## 20.6 Static driver table

M3 uses an explicit compile-time driver table rather than linker discovery magic.

For example:

```c
static const struct zdriver *const boot_drivers[] = {
    &sio_driver,
    0
};
```

Boot order:

```text
kernel
  │
  ├── irq_init()
  ├── device_registry_init()
  ├── driver_init_static()
  │      └── each driver registers device instance(s)
  ├── service_bind_defaults()
  └── continue boot
```

This table is intentionally simple. M4 replaces the source of driver code, not the registry model.

---

## 20.7 Driver placement and private state

Static M3 driver code and private state live in bank 7.

Driver implementation files live under `drivers/`, never under `platform/zephyr80/`. The platform tree supplies only the register addresses, bit definitions, and wiring constants consumed by those drivers.

A driver's private state belongs to that driver and should be colocated conceptually with it rather than scattered through common memory.

Only platform IRQ source stubs and the generic IRQ crossing machinery belong in common.

If a static driver claims an IRQ source, its handler may remain in bank 7 because the M2 IRQ executive maps bank 7 before dispatching it.

---

## 20.8 SIO0/B static driver

*(M3 history: since M4 stage 08 this driver is loaded as a ZMOD module instead of being linked; see sections 22.9 and 22.13. The registry, capability, and console contracts below are unchanged.)*

The first M3 driver wraps the same physical channel used by the early console:

```text
SIO0/B data       22h
SIO0/B control    23h
115200 8N1
```

The early console remains direct and independent.

The static SIO driver may initially use polling for both TX and RX.

It must not make kernel panic output dependent on the registry.

The same hardware may therefore have two software access paths:

```text
panic / very early boot
    -> early_console_*()
    -> SIO0/B directly

normal kernel console
    -> console service
    -> registered sio0.b device
    -> SIO driver
```

The driver initialization should be idempotent with respect to the known SIO configuration or explicitly reinitialize the channel to the documented state.

---

## 20.9 Console service binding

M3 introduces a minimal console service above devices.

It stores:

```text
console input device handle
console output device handle
```

Default boot binding:

```text
console input  -> sio0.b
console output -> sio0.b
```

The service exposes kernel-internal operations equivalent to:

```c
int console_putc(char ch);
int console_write(const void *buf, uint16_t len);
int console_poll(void);
int console_getc(void);
```

The normal console service must fail cleanly if no suitable device is registered.

Kernel panic continues to bypass it.

---

# 21. M3 acceptance criteria

M3 is accepted when:

- the kernel initializes an empty fixed-size device registry;
- the static SIO driver initializes and registers one SIO0/B device;
- the registered device advertises serial/character/console capabilities appropriate to its implementation;
- capability lookup locates SIO0/B without comparing its driver name;
- default console input/output bindings select that device;
- normal kernel output can be sent through the console service and appears correctly at 115200 8N1;
- the early console still works when the registry or normal console binding is deliberately disabled;
- invalid handles and duplicate registrations fail predictably;
- registry exhaustion is detected rather than overwriting memory;
- a static driver may claim an M2 IRQ source and have a bank-7 handler invoked correctly, even if the initial SIO driver remains polling-only;
- driver code/state is linked in bank 7, not common memory.

---

# 22. Milestone M4 — Dynamic driver/module loading

## 22.1 Objective

M4 changes drivers from being compiled into the kernel to being **boot-loadable resident modules**.

The device registry and driver contracts from M3 remain unchanged.

M4 proves:

- module allocation in bank 7;
- relocation;
- BSS/private-state allocation;
- driver initialization;
- device registration;
- optional IRQ-source ownership from dynamically loaded code.

M4 does **not** require runtime unload or hot-plug.

Once loaded successfully, a driver remains resident until reboot.

---

## 22.2 Fixed-granule driver arena

M4 carries forward the proven fixed-slot idea, but makes placement a loader decision.

A linker-defined region of bank 7 is reserved as the **driver arena**.

The arena:

- lies entirely within `2000h-DFFFh`;
- is visible only while mode `11` is active;
- is not used by the kernel image, stacks, or other persistent state;
- begins and ends on the allocation granule boundary.

Initial allocation granule:

```text
512 bytes
```

A module receives a contiguous whole number of granules.

Examples:

```text
620-byte module + state   -> 2 granules = 1024 bytes
2100-byte module + state  -> 5 granules = 2560 bytes
```

Slack inside an allocation belongs to the module. It is reserved headroom, not free space available to the next driver.

The exact arena base and size are linker symbols, not hard-coded module ABI addresses:

```text
__driver_arena_start
__driver_arena_end
```

The build must report arena usage.

Implemented geometry (stage `07-m4-module-loader`): the initial arena is `A000h-BFFFh` (8192 bytes, 16 granules) between the loadable kernel image and the `C000h-DFFFh` runtime area, declared in `platform/zephyr80/memory.inc`, exported by `kernel/module/arena.asm`, and enforced by `tools/mapcheck.py` / `tools/pack_image.py` (no kernel section, stack, or ROM payload may occupy it). The build prints arena geometry plus free granules, and `zephyros.manifest.json` carries an `arena` object. Concrete loader behavior, error codes, and tooling are documented in `docs/module-format.md`.

This `A000h-BFFFh` geometry is the historical M4 acceptance geometry, not the long-term production budget. M8 supersedes it with a linker-derived arena sized from the end of the linked kernel to `C000h`; see section 30.2.

---

## 22.3 Module format

M4 introduces **Zephyr Module Format v1 (ZMOD)**.

ZMOD is a generic relocatable container. M4 supports only module type `DRIVER`.

Later executable milestones may reuse its relocation machinery where useful, but native program execution has its own ZEX contract.

A ZMOD v1 header contains, at minimum:

```text
magic               "ZMOD"
format_version      1
module_type         DRIVER
driver_abi_version
flags

image_size
bss_size
required_granules

init_offset
fini_offset         optional / may be zero

reloc_offset
reloc_count
```

Implemented header (stage 07): a packed little-endian 28-byte structure with `payload_offset`, `total_size`, and a `reserved` word, pinned by `include/zephyros/module.h` compile-time assertions. `docs/module-format.md` records the byte table and the manifest/packer contract.

All addresses stored in the module are offsets relative to the module load base unless explicitly defined otherwise by a relocation record.

The loader validates all sizes and offsets before modifying the driver arena.

---

## 22.4 Relocation model

M4 modules are not linked to a fixed slot address.

The initial required relocation type is:

```text
ZREL_ABS16
```

Meaning:

```text
word_at(module_base + patch_offset) += module_base
```

The host packer/link step derives patch locations from the SDCC/ASxxxx relocation records. Symbol information is used to resolve addresses; the loader does not scan opcodes or infer relocation sites from symbols alone.

The host tool resolves the module at virtual base `0000h`, emits a complete payload, and converts surviving module-base-dependent absolute references into the compact ZMOD patch table. The target-side loader therefore needs no object-file parser or symbol table.

The host packer must reject object constructs that require relocation types the M4 loader does not understand.

Additional relocation types may be added only when a real driver/toolchain output requires them.

The loader must validate that every relocation patch address lies entirely inside the loaded image before applying it.

No relocation is permitted to patch arbitrary kernel memory.

---

## 22.5 Kernel imports

A dynamic driver must not link directly against arbitrary private kernel symbols.

At initialization the kernel passes a versioned driver API object:

```c
int driver_init(const struct zdriver_api *api);
```

The API provides the operations a driver needs, such as:

```text
device_register
irq_claim
irq_release
console-independent diagnostic logging if provided
platform-safe helpers explicitly exposed to drivers
```

The driver ABI version in the module header must match a supported kernel ABI.

M4 may use the SDCC calling convention internally, but the versioned driver ABI boundary must be isolated so it can be replaced by explicit assembly thunks later without changing the module/registry architecture.

---

## 22.6 BSS and private state

The allocation size covers:

```text
loaded image
+ zero-initialized BSS/private state
+ alignment
```

After relocation:

```text
[module image][BSS/private state][reserved slack]
```

The loader zeros BSS before calling the module's init entry.

A driver's persistent private state remains inside its allocation.

The registry stores pointers to that state but does not own it.

---

## 22.7 Module load sequence

Conceptual loader sequence:

```text
locate module source
    │
    ▼
read and validate header
    │
    ├── verify magic/version/type
    ├── verify driver ABI compatibility
    ├── validate sizes/offsets/relocations
    │
    ▼
allocate contiguous driver-arena granules
    │
    ▼
copy image
    │
    ├── zero BSS
    ├── apply relocation table
    └── construct module descriptor
    │
    ▼
call module init(driver_api)
    │
    ├── module registers device(s)
    └── module may claim IRQ source(s)
    │
    ▼
mark module RESIDENT
```

If any step before successful initialization fails, the allocation is reclaimed and no partially registered devices may remain.

M4 needs rollback for **failed load**, not general runtime unload.

---

## 22.8 Initial module source

M4 occurs before any filesystem milestone.

Therefore the M4 loader source abstraction does not depend on a filesystem.

The implemented M4 path uses an **embedded module catalog** generated at build time.

For example:

```text
embedded_modules[]
    "sio" -> byte blob / size
```

M5 replaces this production source with the ROM recovery filesystem. The ZMOD allocator, relocation, driver initialization, device registration, and IRQ ownership machinery remain unchanged; only the byte source changes.

---

## 22.9 Dynamic SIO driver experiment

The M3 SIO driver is the first M4 module.

For the M4 acceptance build:

- remove the normal SIO driver from the static boot-driver table;
- package it as a ZMOD driver;
- load it from the embedded module catalog;
- allocate it at a loader-selected driver-arena address;
- relocate it;
- run its init function;
- register `sio0.b`;
- bind the console service;
- print through the dynamically loaded driver.

The early console remains available throughout the experiment.

This makes failure diagnosable even when the dynamic driver cannot initialize.

---

## 22.10 Dynamic IRQ handlers

A loaded driver may claim an M2 logical IRQ source.

Its registered IRQ handler lives inside the module allocation in bank 7.

The M2 common IRQ gate maps mode `11` before dispatch, so no dynamically loaded ISR implementation is required in common memory.

The IRQ registry must reject handler addresses outside valid resident kernel/module memory.

Because M4 has no unload, a successfully registered handler cannot become dangling during normal operation.

---

## 22.11 No runtime unload in M4

M4 explicitly excludes:

- hot unplug;
- arbitrary driver unload;
- module replacement in place;
- reference counting;
- rebinding live devices while clients are using them.

A reboot is the unload mechanism.

This keeps module lifetime simple:

```text
load -> initialize -> resident until reboot
```

An optional `fini_offset` exists only for failed-initialization cleanup or future expansion; M4 does not expose a user-visible unload operation.

---

## 22.12 Host-side module tooling

`tools/zmodpack.py` produces and validates ZMOD files.

It must:

- consume the supported SDCC/assembler build outputs and their relocation information;
- resolve module-local symbols;
- generate the runtime relocation table;
- reject unsupported relocation records;
- emit module size and granule requirements;
- provide a dump/validation mode suitable for automated tests.

The build should be able to produce a human-readable report similar to:

```text
sio.zmod
  image:        734 bytes
  bss:           82 bytes
  relocations:   19
  allocation:     2 x 512-byte granules
```

Numbers above are illustrative; the build reports the real values.

---

## 22.13 M4 implementation notes (stages 07-08)

The loader half of M4 is implemented as:

```text
include/zephyros/module.h       ZMOD v1 definitions, errors, catalog,
                                descriptors, loader API, size asserts
kernel/module/arena.asm         __driver_arena_start/__driver_arena_end
kernel/module/relocate.c        container validation, copy, BSS,
                                ZREL_ABS16 application
kernel/module/loader.c          first-fit granule allocator, descriptors,
                                transactional module_load_source,
                                module_code_allowed (IRQ validation hook)
tests/target/m4/m4_test.c       loader/dynamic-driver acceptance test
tools/zmodpack.py               pack/validate/dump/emit-asm
```

M2's registry gained claim serials, owner tokens, and checkpoint/rollback (`irq_registry_checkpoint/rollback/set_owner`) so a failed module init rolls back its IRQ claims exactly like its device registrations. `irq_claim` now accepts handler addresses that are linked bank-7 code or lie inside the active/committed module images; see `docs/module-format.md` section 4.1.

Stage 08 (`08-m4-dynamic-driver-acceptance`) then removed the static SIO driver from the kernel entirely; stage 10 separated production drivers, module packaging/runtime support, and acceptance code:

```text
drivers/sio/sio.c               SIO implementation (packed into the module)
drivers/sio/sio_io.asm          SIO port I/O
drivers/sio/sio_module.c        production module entry (descriptor + init)
modules/manifests/sio.json      production module manifest
modules/runtime/sdcc_indirect_calls.asm  shared module-local SDCC glue
tests/modules/sio_irq_module.c  IRQ self-test variant entry
tests/modules/sio_fail_module.c init-failure variant entry
```

* the M3 static driver table is retired: no `boot_drivers[]`, `driver_init_static()`, or `_driver_static_*` symbol is linked, and `tools/mapcheck.py --production` rejects any acceptance symbol;
* every M4 image embeds a boot module through the generated catalog: `sio.zmod` (production, loader image, placement image), `sio-irq.zmod` (`make m4-selftest`), or `sio-fail.zmod` (`make m4-failure-selftest`).  **The M1-M7 acceptance images, the embedded boot catalog and the legacy fixed-arena geometry were retired under the Mn-1 retention rule at the start of M9 (work package 25a); the loader mechanisms this section proves are now exercised on the current boot path by the loader group of the subsystem acceptance image -- `tests/target/loader/` replayed by `tests/test_subsys_sim.py` -- with container validation and two-base relocation proven on host by `tests/test_zmod.py` and arena exhaustion by the `m8-fb-arena` image;**
* the (retired) placement image regenerated `platform/zephyr80/memory.inc` with the arena shifted to 9E00h-BE00h and embedded byte-identical module files;
* `kernel_main()` prints `console: dynamic sio0.b OK` through the bound module (or `console: dynamic driver failed` on load failure);
* `tools/mapcheck.py` rejects any linked `_sio_`/`_sio0b_`/`_sio_driver` symbol, so the static driver cannot return unnoticed.

See `docs/module-format.md` section 6 for the pipeline and `docs/devices.md` for the resulting device-layer behavior.

---

# 23. M4 acceptance criteria

M4 is accepted when:

- the driver arena is linker-reserved and excluded from kernel/static state;
- the granule allocator detects full/exhausted conditions safely;
- a ZMOD header with bad magic, bad version, invalid size, invalid relocation, or unsupported ABI is rejected without corrupting the arena;
- the SIO driver is absent from the static driver table;
- the SIO ZMOD is loaded from the embedded module catalog;
- its actual load address is chosen by the allocator rather than compiled into the driver;
- relocation is applied successfully;
- BSS/private state is zeroed;
- module initialization registers `sio0.b` through the same M3 device registry used by static drivers;
- the console service binds to the dynamically registered device;
- serial output succeeds through the dynamic driver on physical hardware;
- the early console remains functional if dynamic loading fails;
- at least one test build deliberately changes the module's arena placement and the same module binary continues to work;
- a dynamically loaded handler can claim an M2 IRQ source and execute correctly from its relocated bank-7 address;
- failed module initialization rolls back its arena allocation and registry changes;
- no runtime unload capability is required.

Evidence mapping (as of M4): `tests/test_m4.py`/`tests/test_m4_sim.py` and `tests/test_zmod.py` proved every criterion on host (including the shifted placement and the failure rollback); `make m4-selftest`, `make m4-placement-selftest`, `make m4-failure-selftest` and the corresponding `smoke-hardware-*` captures provided the physical-hardware record (`docs/validation/2026-10-02-m4-acceptance.md`).  Those images and tests were retired under the Mn-1 retention rule (work package 25a); the criteria are now carried by `tests/test_zmod.py`, the loader group of `tests/target/loader/` replayed by `tests/test_subsys_sim.py`, and the `m8-fb-arena` image.

---

# 24. Milestone M5 — ROM recovery filesystem

## 24.1 Objective

M5 adds a deliberately small read-only filesystem backed by ROM.

Its job is not to define the general ZephyrOS filesystem architecture. It is the **boot and recovery store** that exists even when the IOC, SD card, FAT filesystem, or later shell are unavailable.

M5 provides a dependable source for:

- boot-time ZMOD drivers;
- the first native ZEX programs;
- recovery and diagnostic programs;
- small immutable configuration or recovery assets where useful.

The M4 module loader does not change. M5 replaces the embedded-module byte source with ROMFS.

---

## 24.2 ROM access under the Zephyr memory model

Normal runtime modes `10` and `11` have ROM disabled. Runtime ROM reads therefore require a controlled mapping transition through **mode 00**, the only mode that selects ROM — and it selects ROM for every read (including instruction fetch), while every write still targets the selected SRAM bank. There is no mode that exposes ROM together with a SRAM common region; `08h` is a flat view of one SRAM bank.

The transition therefore follows the same pattern as this machine's drive-A ROM primitive: a small stub is mirrored into every recovery ROM page at the same address as its common-SRAM copy; a two-byte trampoline `OUT` switches to mode 00; the stub runs stackless (`LDIR`, then the latch-restoring `OUT`); ordinary common code resumes in mode 11:

```text
mode 00 window (one bounded copy)
0000h-FFFFh   reads: selected ROM page
              writes: selected SRAM bank (bank 0 for staging)
```

This has three consequences:

1. the code executing between the two `OUT`s must be byte-identical in common SRAM and in every selected ROM page, must not touch the stack, and must not read any SRAM variable (registers only);
2. the logical ROMFS payload uses only `0000h-BFFFh` of each allocated ROM page; the `C000h-FFFFh` half is never file data (it carries the fixed stub mirror at `ZOS_ROM_ACCESS_BASE` and `FFh` elsewhere);
3. the common staging destination is reached correctly because the mode-00 latch selects SRAM bank 0, and mode-00 writes land in bank 0 across the whole address space.

The logical ROMFS page payload is therefore:

```text
ROMFS_PAGE_BYTES = C000h = 48 KiB
```

The `C000h-FFFFh` ROM bytes are not part of the runtime recovery filesystem because that window is reserved for the stub mirror.

The exact ROM pages assigned to ROMFS are build/platform configuration. M5 must not hard-code assumptions that collide with page 0 boot contents, page 1's kernel image, or any ROM-backed assets retained elsewhere.

---

## 24.3 Common ROM-access primitive

A ROM read temporarily hides all runtime mappings, so the actual mapping transition and copy loop execute from code that exists in both worlds: the mirrored stub in the ROM page and its byte-identical copy in common SRAM.

Conceptually:

```text
kernel in mode 11
    │
    ├── validate ROM page / offset / length
    ├── save the exact latch; lock interrupts; claim the busy flag
    ├── arm registers: HL=source, DE=staging, BC=length,
    │       A=mode-00 latch, A'=exact caller latch
    │
    ├── trampoline OUT (00h): mode 00, page p, bank 0
    │
    ▼
mirrored stub in the selected ROM page (stackless)
    │
    ├── LDIR: ROM (HL) -> SRAM bank 0 staging (DE), BC bytes
    ├── OUT (00h): restore the exact caller latch
    │
    ▼
common epilogue, mode 11 restored
    │
    ├── release the busy flag; restore IFF; return
```

The ROM-access routine must never execute, return into, or stack on `2000h-DFFFh` while mode 00 is active, and it must not use the stack at all between the two `OUT`s; the simulator pins the executed window to the mirrored stub and rejects any stack movement.

The initial implementation may stage small chunks rather than trying to map an entire file at once.

---

## 24.4 Recovery filesystem format

ROMFS v1 is intentionally simple and read-only.

It uses a flat native namespace. Directories, permissions, timestamps, allocation, deletion, and write support are unnecessary for recovery.

A host-built image contains:

```text
ROMFS header
file table
file payloads
```

The header contains at minimum:

```text
magic
format_version
file_count
directory_size
image_size
```

Each file-table entry contains at minimum:

```text
name
logical_offset
size
flags/type
```

A checksum/CRC may be added if it materially improves recovery diagnostics, but M5 does not require a general filesystem integrity subsystem.

`logical_offset` is measured across the concatenated 48-KiB usable windows of the configured ROMFS pages; the reader translates it into `(ROM page, offset within 0000h-BFFFh)`.

Names are native ZephyrOS recovery names, not CP/M 8.3 names.

---

## 24.5 Kernel ROMFS interface

The M5 API should remain small and allocation-free.

Semantics equivalent to the following are sufficient:

```c
int romfs_init(void);
int romfs_lookup_ci(const char *name, romfs_file_t *out);
int romfs_read(const romfs_file_t *file,
               uint32_t offset,
               void *dst,
               uint16_t length);
```

`romfs_file_t` is immutable metadata identifying one ROMFS entry.

M5 does not expose this API as the future native filesystem ABI. M9 supersedes the application-facing part of this design by adapting the existing low-level ROMFS reader beneath the VFS provider interface; section 32.13 collapses the lookup to the single ASCII case-folded `romfs_lookup_ci`.

---

## 24.6 Host-side ROMFS construction

`tools/romfspack.py` builds and validates the recovery image from a manifest.

Conceptually:

```text
recovery/manifest.json
    │
    ├── sio.zmod
    ├── monitor.zex        (added by M7)
    └── recovery assets
    │
    ▼
romfspack.py
    │
    ▼
recovery.romfs
    │
    ▼
pack_image.py
    │
    └── configured ROM pages
```

The packer must reject duplicate names, capacity/window violations and nondeterministic output, and emit a human-readable placement report.

---

## 24.7 Boot-module source transition

M5 changed the normal production boot path from an embedded module catalog to direct ROMFS lookup. This remains historical M5 behavior; M9 replaces direct boot-time ROMFS lookup above the provider with an early VFS bootstrap and generic VFS-backed module source.

---

## 24.8 M5 implementation notes (stage 11)

M5 is implemented and physically accepted. The concrete ROMFS image, mirrored-stackless ROM access primitive, source-backed module loader, production corruption diagnostics, host tools and acceptance evidence are documented in `docs/romfs-format.md` and `docs/validation/2026-10-03-m5-romfs.md`.

---

# 25. M5 acceptance criteria

M5 is complete and physically accepted. Its recovery-store and ROM-access invariants remain binding under M9; M9 wraps them rather than replacing the low-level medium implementation.

---

# 26. Milestone M6 — Native process and ZEX execution

## 26.1 Objective

M6 introduces the first native ZephyrOS process lifecycle and the ability to execute a native program.

M6 remains deliberately single-tasking:

```text
parent runs
    -> exec child
    -> parent suspended
    -> child runs alone
    -> child exits
    -> parent resumes
```

There is no scheduler and no preemption requirement.

The first acceptance parent may be the kernel launcher. The process model must nevertheless be structured so the later native shell can become an ordinary parent process without redesigning `exec`/`exit`.

---

## 26.2 Process descriptor

The kernel keeps process descriptors in bank 7.

A descriptor contains at least the state needed to suspend and restore a process:

```text
process id / slot
state: FREE, RUNNING, SUSPENDED
application bank 0-6
parent process id or kernel-parent marker
saved application SP / return context as required
exit status
process flags
```

The table may be fixed-size; M6 does not require a heap.

Bank 7 is never a process bank.

Only one process is RUNNING at a time.

---

## 26.3 Native page zero

Every native process receives a ZephyrOS-owned page zero in its own application bank.

At minimum it contains the native syscall gateway already reserved by M1:

```text
0030h   RST 30h entry / JP zos_syscall_gate
```

Other bytes are initialized by ZephyrOS rather than inherited from a previous program.

```text
0080h   startup block v1 (all 128 bytes at 0080h-00FFh)
        0080h  u8  version = 1
        0081h  u8  tail_length  0..125
        0082h  ... tail bytes, then one NUL
```

Every native process receives this block, including the kernel-launched
one; the launcher passes an empty tail.  The launch gate enters the
program with `HL = 0080h`.  The kernel transports a `SYS_EXEC` caller's
raw command tail into `tail` byte-exact and never parses it; user space
owns splitting (`user/sdk/include/zos/process.h`, docs/zex-format.md section
5.1).  The block is inside the caller-visible low window, so it is
readable in both mode `10` and mode `11`; it sits below the `0100h`
syscall buffer window, so a program copies the tail into its own data
before a syscall.  A program that ignores `HL` is unaffected.

This is a native page zero. It does not contain CP/M `CALL 0005h`, FCBs, command tail, or other compatibility structures.

The versioned block is the single startup interface: a later version can
carry more than a command tail (environment, handles) without changing
the launch contract, because the program reads its shape from
`version`.

---

## 26.4 ZEX v1: single-bank native executable

M6 defines the first **ZEX** executable contract.

The M6 form is intentionally simpler than the later far-code/far-data format:

- one application bank;
- one main code/data image;
- one zero-initialized BSS region;
- one entry point;
- one normal process stack;
- no auxiliary-bank allocations;
- no far-call or far-data segments.

The executable's logical load address is fixed at:

```text
0100h
```

This is important: selecting a different physical application bank does **not** change the Z80 logical addresses seen by the program. A normal single-bank ZEX linked at `0100h` therefore does not need M4-style base relocation merely because it is loaded into bank 0, 1, 2, etc.

M12 may extend ZEX with segment/relocation metadata for auxiliary far code/data without changing this simple primary-bank model.

---

## 26.5 ZEX v1 header

A ZEX v1 header contains at minimum:

```text
magic               "ZEX1"
format_version      1
abi_version
flags
image_size
bss_size
entry_offset
stack_size
header_size
```

The effective entry address is `0100h + entry_offset`.

The loader validates that the image and BSS fit below the reserved stack, the entry lies inside executable image space, arithmetic cannot wrap, and no payload overlaps page zero or common memory.

---

## 26.6 Native process memory layout

```text
0000h-00FFh   ZephyrOS native page zero
0100h-....    ZEX image
....          zero-initialized BSS
....          free process memory
....-DFFFh    process stack growing downward
E000h-FFFFh   permanent physical bank-0 common memory
```

---

## 26.7 ZEX load/launch sequence

M6 loads programs from M5 ROMFS through the source-backed ZEX/process loader. M9 will make that loader source-neutral with VFS-backed input without changing ZEX validation or process-bank semantics.

---

## 26.8 Minimal native syscall set

M6 established:

```text
SYS_EXIT
SYS_CONSOLE_WRITE
SYS_CONSOLE_READ
SYS_CONSOLE_POLL
SYS_GETINFO
```

M9 extends the same native syscall mechanism with filesystem services.
Package 28 adds `SYS_EXEC` (a synchronous child through the same process
machinery); package 28b extends its block with a raw command tail that
reaches the child's startup block and enters every program with
`HL = 0080h`.

---

## 26.9 Exit and parent restoration

`SYS_EXIT` records status, releases the child bank/resources and restores the suspended parent/kernel launcher. This remains the native lifecycle used by later shell and personalities.

M6 restored exactly one suspended launcher: the kernel launcher on the boot
stack. Package 28 keeps that same lifecycle but generalizes "the suspended
parent" to an ordinary process suspended in its own `SYS_EXEC`, up to
`ZOS_EXEC_MAX_DEPTH` (4) user processes deep (section 32.40). The process
table's `parent` field is the only nesting state; there is no second
process manager, no separate exec table and no new loader.

---

## 26.10 M6 non-goals

M6 excluded scheduling, signals, pipes/redirection, dynamic linking, writable filesystems, far allocations, CP/M `.COM` execution and a shell.

---

## 26.11 M6 implementation notes (stage 12)

M6 is implemented, host-validated and physically accepted. The concrete process table, launch/copy gates, syscall implementation, ZEX runtime, acceptance programs and physical evidence are documented in `docs/zex-format.md`, `docs/abi.md` and `docs/validation/2026-10-03-m6-zex.md`.

---

# 27. M6 acceptance criteria

M6 is complete and physically accepted. Its application-bank, page-zero, ZEX validation, syscall-crossing and parent-resume invariants remain binding for M9.

---

# 28. Milestone M7 — Native monitor and diagnostic foundation

## 28.1 Objective

M7 ports the Zephyr-80 monitor concept into a native ZephyrOS ZEX program and establishes the reusable diagnostic boundary. The monitor is the recovery environment used before the native shell exists.

---

## 28.2 Port, do not transplant

The monitor preserves useful operator behavior while using native ZephyrOS services rather than old monitor-ROM or CP/M assumptions.

---

## 28.3 Monitor uses native console services

`monitor.zex` performs normal input/output only through native console syscalls and therefore runs unchanged through the M8 HID/V9958 console or SIO fallback.

---

## 28.4 Diagnostic syscall boundary

M7 added diagnostic syscalls for memory, I/O-port, bank and system inspection/manipulation. These remain explicit recovery/diagnostic interfaces rather than normal driver bypasses.

---

## 28.5 Diagnostic-suite foundation

Reusable line editing, the diagnostic runtime wrappers and the string/format helpers live in the user SDK (`user/sdk/`) for reuse by M11; the monitor's own hex parsing/formatting, hexdump and status output live in `user/monitor/` (docs/sdk.md).

---

## 28.6 Monitor packaging and launch

`monitor.zex` is an ordinary ZEX. Under M9 the boot/recovery launcher should obtain it through `/SYSTEM` and VFS rather than direct ROMFS lookup.

---

## 28.7 Relationship to the future native environment

The monitor remains a permanent recovery/diagnostic asset after the shell arrives.

### 28.8 M7 implementation notes (stage 13)

M7 is implemented, host-validated and physically accepted. Details and transcripts are recorded in `docs/validation/2026-10-03-m7-monitor.md`.

---

# 29. M7 acceptance criteria

M7 is complete and physically accepted. Its monitor remains device-independent and uses only the native syscall/service boundary.

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

M8 turns the M3/M4 character-device proof into a class-aware device architecture. The registry remains the common discovery mechanism, but video, HID and IOC gain explicit class contracts rather than being forced through a single character-device operations shape.

The V9958, HID and IOC drivers are ordinary resident ZMOD modules sourced from ROMFS and relocated by the M4 loader.

### M8 source-tree additions

```text
include/zephyros/video.h
include/zephyros/hid.h
include/zephyros/ioc.h

drivers/v9958/
drivers/hid/
drivers/ioc/

modules/manifests/v9958.json
modules/manifests/hid.json
modules/manifests/ioc.json
```

---

## 30.2 Production driver-area sizing

M9 (package 25b) removes every fixed boundary from the bank-7 layout.
There is one geometry, derived by a two-pass build: a probe link places no
arena, the build measures the exclusive end of the linked loadable kernel
and the runtime-data size (`l__DATA + l__INITIALIZED`), and derives

```text
arena      = [align_up(__kernel_code_end, 512), data_base)
data_base  = align_down(DF00h - data_size, 512)
arena_end  = data_base
```

with a minimum production arena of 16 KiB (32 granules) and 512-byte
allocation granularity.  The bank-7 layout is therefore kernel code, then
the derived arena, then the derived runtime data, then the fixed 256-byte
ISR stack at `DF00h-DFFFh`.  `ZOS_BANK7_LOAD_END` (`C000h`) is only the
architectural ceiling on the loadable kernel end, not a boundary.
`pack_image.py` and the arena plan print the single bank-7 budget ledger
section 32.39 requires.

---

## 30.3 Driver ABI v2 and class-aware operations

Driver ABI v2 uses 32-bit capabilities, optional generic character operations and class-specific operations. The current classes include:

```text
ZDEV_SERIAL
ZDEV_VIDEO
ZDEV_BLOCK      # M9 rename of the former ZDEV_STORAGE numeric slot
ZDEV_HID
ZDEV_IOC
ZDEV_OTHER
```

The numeric slot formerly called `ZDEV_STORAGE` is preserved but the architectural name is now `ZDEV_BLOCK`.

M9 publishes the block class contract in `include/zephyros/block.h` (`zblock_info`, `zblock_ops`) with the `ZCAP_BLOCK_READ | ZCAP_BLOCK_WRITE | ZCAP_BLOCK_FLUSH | ZCAP_BLOCK_REMOVABLE` bits in capability range 20-23 (section 32.15). No `ZDEV_BLOCK` driver is resident: the first block driver arrives with its first consumer.

---

## 30.4 VIDEO class and V9958

V9958 registers `ZDEV_VIDEO`, the native `ZVIDEO_*` operations and `ZCAP_CHAR_OUT | ZCAP_CONSOLE_OUT`. It provides the 85x26 6x8 terminal/VT-style console renderer and remains independent of IOC.

---

## 30.5 HID class

HID registers keyboard/text input above IOC and advertises `ZCAP_HID_TEXT | ZCAP_CHAR_IN | ZCAP_CONSOLE_IN`. It discovers IOC by class/capability rather than name or symbols.

---

## 30.6 IOC class

IOC owns the SIO1 command/reply and bulk transport, READY/DONE lifecycle, sequencing, timeouts/recovery and semantic status. Consumers never see raw SIO/CTS/CRC details. M9 uses the same IOC class for both the raw-SD block driver and the IOC-FAT filesystem provider.

---

## 30.7 Driver-to-driver discovery

Loaded modules discover dependencies by class plus capabilities using the ABI-v2 `device_find()` / `device_interface()` path. Modules remain resident until reboot, so no lifetime reference counting is required.

---

## 30.8 Boot order

M8 production boot loads `sio.zmod`, `ioc.zmod`, `hid.zmod`, `v9958.zmod` from the recovery store, then binds the normal console and launches the monitor. M9 supersedes the direct ROMFS aspect of that sequence by bringing VFS up before dynamic module loading and reading those module files through `/SYSTEM`.

---

## 30.9 Console binding policy

Preferred console:

```text
input:   ZDEV_HID   + ZCAP_CONSOLE_IN
output:  ZDEV_VIDEO + ZCAP_CONSOLE_OUT
```

Each direction independently falls back to a generic console-capable device, normally SIO. Panic/early boot remains direct SIO.

---

## 30.10 M8 non-goals

M8 did not implement the native filesystem, shell, complete diagnostic suite, module unload, far-memory services or compatibility personalities.

---

# 31. M8 acceptance criteria

M8 is complete and physically accepted. `docs/validation/2026-10-03-m8-device-stack.md` records the integrated image, full test suite, fallback matrix and physical HID/V9958 console validation completed on 2026-10-04.

---

# 32. Milestone M9 — Native VFS and filesystem framework

## 32.1 Objective and goals

M9 gives ZephyrOS a real native filesystem framework rather than hard-wiring the OS to the current MCU FAT implementation.

The architecture must support:

- one native hierarchical namespace rooted at `/`;
- no native drive letters;
- multiple filesystem implementations;
- static boot-time mount configuration;
- ROMFS as a normal filesystem implementation;
- the current MCU/FatFs service as one filesystem provider;
- raw SD access through a generic block-device abstraction;
- future filesystems implemented directly on the Z80, such as VFAT or a Fuzix filesystem;
- path-, handle-, and byte-oriented native APIs;
- provider-local caching where a backend needs it (IOC-FAT keeps a Z80-side file-data cache to avoid repeated transport traffic);
- a clean boundary for later personalities such as CP/M.

The central rules are:

> **VFS owns the namespace. Filesystems provide trees attached to paths in that namespace.**

> **IOC is transport only. Filesystem and block semantics live above it.**

> **Caching is filesystem-provider policy. VFS owns no cache.**

> **Resident bytes are an architectural resource (section 32.39).**

---

## 32.2 High-level layering

```text
native applications
        |
        v
+-----------------------+
|         VFS           |
| namespace / paths     |
| mounts / CWD / fd     |
+-----------------------+
        |
        +---------------------+----------------------+
        |                     |                      |
        v                     v                      v
   ROMFS filesystem      IOC-FAT filesystem      future FS
                         (file-data cache)    (optional block cache)
                              |                      |
                              |                      v
                              |                block layer
                              |                      |
                              |                  ZDEV_BLOCK
                              |                      |
                              |                 SD-over-IOC
                              |                      |
                              +----------+-----------+
                                         |
                                         v
                                      ZDEV_IOC
                                         |
                                         v
                                        MCU
                                         |
                                         v
                                         SD
```

Ownership is deliberately separated:

- **VFS** owns the global namespace and process-visible filesystem semantics.
- **Filesystem providers** own filesystem-specific behavior, including any caching.
- **Block devices** expose raw sector/block storage.
- **IOC** owns only the Z80-to-MCU transport.

The MCU currently provides both raw SD primitives and a FatFs-based filesystem service. ZephyrOS exposes both without making either one the architectural foundation of the other.

---

## 32.3 Native namespace

ZephyrOS has exactly one native namespace:

```text
/
```

There are no native equivalents of `A:`, `B:`, `C:` or CP/M USER areas. Those belong, if needed, to compatibility personalities.

The initial system layout is:

```text
/           -> writable SD FAT filesystem
/SYSTEM     -> internal ROMFS
```

Future devices may be attached at additional root-level mountpoints such as `/CF`, `/SD2`, `/EXTERNAL`, or `/BACKUP`. No drive-selection state exists in native ZephyrOS.

---

## 32.4 VFS owns `/`

The root namespace exists independently of any mounted filesystem. `/SYSTEM` must remain accessible even if the SD card is absent, corrupt, or unavailable.

Conceptually:

```text
VFS namespace
/
|-- SYSTEM     -> ROMFS
|-- CF         -> future external filesystem
`-- default tree supplied by filesystem attached at /
```

The filesystem attached at `/` is the normal writable filesystem, but it does not own the namespace. If the root filesystem is unavailable, `/SYSTEM` remains present and usable.

---

## 32.5 Static mount configuration

M9 does not provide runtime `mount` or `umount` commands. Mount topology is defined by build-time JSON configuration and established during boot.

The production manifest is:

```text
config/mounts.json
```

This manifest is the only place where mount policy (paths, providers, sources, system/required flags) exists. The build translates it into an immutable table compiled into the kernel image, and VFS executes that table at boot. Section 32.33 defines the translation and how VFS consumes the result.

Schema:

```json
{
  "format": "vfs-mounts-v1",
  "mounts": [
    {
      "path": "/SYSTEM",
      "filesystem": "romfs",
      "source": "recovery",
      "system": true
    },
    {
      "path": "/",
      "filesystem": "iocfat",
      "source": "ioc.0",
      "required": false
    }
  ]
}
```

Exactly one configured mount must be marked:

```text
system = true
```

The system filesystem is instantiated during early VFS bootstrap, before any loadable modules are required. Its provider must therefore be statically available and must have no dependency on loadable modules or dynamically registered devices.

Field semantics:

```text
path
    Absolute VFS mountpoint.

filesystem
    Name of a filesystem provider registered with VFS.

source
    Opaque provider-specific source identifier.
    VFS stores/passes it but does not interpret it.

system
    true:
        this is the unique bootstrap filesystem.
        it is instantiated immediately after VFS core initialization.
        its provider must already be available in the kernel image.
        it may not depend on loadable modules or dynamically registered devices.
        failure to instantiate it is fatal.

required
    true:
        for non-system mounts, failure to instantiate the mount is a
        boot/configuration failure.

    false:
        for non-system mounts, mount failure is tolerated and the remaining
        namespace stays usable.
```

`system: true` is stronger than `required: true`; the system entry does not need a separate `required` field.

A future manifest may look like:

```json
{
  "format": "vfs-mounts-v1",
  "mounts": [
    {
      "path": "/SYSTEM",
      "filesystem": "romfs",
      "source": "recovery",
      "system": true
    },
    {
      "path": "/",
      "filesystem": "iocfat",
      "source": "ioc.0",
      "required": false
    },
    {
      "path": "/CF",
      "filesystem": "vfat",
      "source": "cf.0",
      "required": false
    },
    {
      "path": "/SD2",
      "filesystem": "fuzixfs",
      "source": "sd.1",
      "required": false
    }
  ]
}
```

The `source` field is provider-specific. For example, ROMFS may interpret `recovery` as the built-in recovery ROM source, IOC-FAT may interpret `ioc.0` as the MCU filesystem service, while native VFAT/Fuzix providers may interpret `cf.0` or `sd.1` as `ZDEV_BLOCK` instances. VFS does not interpret those strings.

### 32.5.1 Manifest validation

The host-side build tool must reject at least:

```text
unknown format/version
missing required fields
empty filesystem/source names
zero system filesystems
more than one system filesystem
system filesystem naming a provider that is not built into the kernel
`required` present on the system entry
relative mount paths
paths longer than ZOS_PATH_MAX
'.' or '..' in configured mountpoints
non-canonical mount paths (lowercase letters, trailing '/', repeated '/')
duplicate mountpoints
case-only duplicate mountpoints
duplicate '/'
unsupported nested mountpoints in M9
more entries than the kernel mount-table capacity
```

The build tool is told which providers are built into the kernel image. A system entry naming any other provider would have to be loaded from a module, which the system filesystem must never require (section 32.32.2). Such a manifest is therefore rejected at build time.

Because native VFS pathname matching is case-insensitive, `/SYSTEM`, `/system`, and `/System` collide. Configured mount paths must use a canonical spelling: absolute, upper-case ASCII, no trailing or repeated `/`. The tool rejects non-canonical paths; it never rewrites them. The table in ROM is therefore exactly what the reviewed manifest says.

---

## 32.6 Initial mount restrictions

M9 limits configured mountpoints to `/` or one root component such as `/SYSTEM`, `/CF`, or `/SD2`. Arbitrary nested mountpoints may be added later if a real use case appears.

---

## 32.7 Root directory enumeration

`readdir("/")` is a VFS operation. The visible root merges entries from the filesystem attached at `/` with configured root-level mountpoints.

If the SD root contains `GAMES`, `TOOLS`, and `README.TXT`, while VFS has `/SYSTEM` and `/CF`, then `ls /` exposes all five entries. If the backing root also contains a real entry named `SYSTEM`, the VFS mountpoint wins and shadows it while mounted.

---

## 32.8 Path resolution and case folding

VFS owns absolute/relative paths, CWD, normalization, case-insensitive comparison, mount selection and filesystem-relative path construction.

M9 supports `/`, `.`, `..`, repeated slash normalization, absolute paths and relative paths. Traversal above `/` remains at `/`.

Native pathname comparison is ASCII case-insensitive:

```text
/system/monitor.zex
/SYSTEM/MONITOR.ZEX
/System/Monitor.Zex
```

refer to the same namespace object. The comparison rule is simply `A-Z == a-z`; all other bytes compare exactly. ZephyrOS does not attempt Unicode case folding.

The rule is split between VFS and providers:

- VFS normalizes the path (absolute against CWD; `.`, `..`, repeated `/` resolved) and compares only the **first component** case-insensitively against the root-level mountpoints. It does not walk or fold the remaining components.
- Every provider is required to implement ASCII case-insensitive semantics for the provider-relative paths it receives. IOC-FAT inherits this from FAT; ROMFS uses a small ASCII-folded compare.

VFS selects the owning mount before calling a provider. M9 mount selection is flat (section 32.6): if the first component matches a mounted root-level mountpoint, that mount owns the path; otherwise the filesystem attached at `/` owns it. For example:

```text
/SYSTEM/monitor.zex
    mount       = /SYSTEM
    FS-relative = /monitor.zex
    provider    = ROMFS

/GAMES/DOOM.ZEX
    mount       = /
    FS-relative = /GAMES/DOOM.ZEX
    provider    = IOC-FAT
```

A provider never needs to know its external mountpoint.

---

## 32.9 Path and component limits

M9 pins:

```text
ZOS_PATH_MAX = 128
ZFS_NAME_MAX = 64
```

`ZOS_PATH_MAX` includes the terminating NUL for user-facing C wrappers. The syscall ABI still uses explicit pointer/length fields rather than requiring unbounded kernel scans for NUL. `ZFS_NAME_MAX = 64` deliberately avoids baking FAT 8.3 restrictions into the native ABI.

---

## 32.10 VFS responsibilities

`kernel/vfs/` owns:

```text
global namespace
configured mount table
path normalization
case-insensitive mount/path comparison
mountpoint resolution
current working directory
process-visible file descriptors
directory handles
file offsets
filesystem syscall dispatch
user-buffer validation and copying
process cleanup
cross-filesystem policy
```

VFS must not contain FAT parsing, FS2 command numbers, IOC READY/DONE handling, ROMFS on-ROM layout, SD-sector commands, caching policy, or CP/M FCB logic. VFS uses fixed tables only (no dynamic allocation) and one shared 128-byte pathname scratch buffer.

---

## 32.11 Filesystem-provider registration

Filesystem providers register directly with VFS; they are not represented as hardware devices merely because some providers are ZMODs.

Conceptually:

```c
int vfs_register_filesystem(const struct zfs_provider *provider);

struct zfs_provider {
    const char *name;
    const struct zfs_ops *ops;
    void *provider_state;
};
```

This creates distinct registries:

```text
device registry
    SIO
    IOC
    HID
    VIDEO
    BLOCK

VFS filesystem-provider registry
    ROMFS
    IOC-FAT
    future VFAT
    future Fuzix filesystem
```

The build-time mount configuration selects a provider and source and attaches an instance to a namespace path.

---

## 32.12 Filesystem-provider interface

Providers are **path-based**. M9 has a flat namespace (section 32.6) and no use for a Unix-style vnode layer, so VFS does not keep node objects, node lifetimes, or per-component traversal state. Every operation receives a provider-relative path.

```c
struct zfs_ops {
    int (*mount)(void *provider_state, const char *source, void **fs);

    int (*open)(void *fs, const char *path, uint16_t flags, void **file,
                uint32_t *size);
    int (*close)(void *fs, void *file);
    int (*read)(void *fs, void *file, uint32_t offset,
                void *buf, uint16_t len);
    int (*write)(void *fs, void *file, uint32_t offset,
                 const void *buf, uint16_t len);
    int (*sync)(void *fs, void *file);

    int (*stat)(void *fs, const char *path, struct zfs_stat *out);

    int (*opendir)(void *fs, const char *path, void **dir);
    int (*readdir)(void *fs, void *dir, struct zfs_dirent *out);
    int (*closedir)(void *fs, void *dir);

    int (*mkdir)(void *fs, const char *path);
    int (*rmdir)(void *fs, const char *path);
    int (*unlink)(void *fs, const char *path);
    int (*rename)(void *fs, const char *oldpath, const char *newpath);
};
```

Contract for every path argument:

- absolute within the provider (begins with `/`; `/` is the provider root);
- already normalized by VFS: no `.`, `..`, empty or repeated components, no trailing `/`;
- NUL-terminated, at most `ZOS_PATH_MAX - 1` bytes;
- compared by the provider with ASCII case-insensitive semantics (section 32.8).

`open` flags are the native `ZFS_O_*` flags, including `ZFS_O_TRUNC`; `open` reports the current file size. A read-only provider may leave mutation pointers null; VFS returns `EROFS` for a null mutation operation. `readdir` returns 1 with an entry, 0 at the end, or a negative error.

Deliberately absent in M9: `unmount` (no runtime unmounting), filesystem-wide `syncfs`, a separate `truncate` operation (`ZFS_O_TRUNC` covers M9), `statfs` (deferred until a consumer requires it, as sections 32.26 and 32.34 record), and any lookup/release/node operations. They are added when a real consumer appears. ZephyrOS is still being built and keeps no backward-compatibility obligation, so such additions may reshape the interface freely.

Opaque provider handles remain private. Applications never see ROMFS entry indices, FatFs `FIL` objects, FS2 tokens, FAT clusters, or future filesystem-native inode structures.

---

## 32.13 ROMFS becomes a filesystem provider

M9 preserves the proven M5 low-level ROMFS reader but exposes it through the VFS framework:

```text
ROM page access
      |
      v
existing ROMFS format reader
      |
      v
fs/romfs
      |
      v
zfs_ops
      |
      v
VFS mount at /SYSTEM
```

ROMFS remains read-only, resident in recovery ROM pages, available without SD, and authoritative for boot/recovery assets. Mutation operations return `EROFS`. ROMFS has no cache: its reads already come straight from ROM.

There is exactly one ROMFS lookup: the provider's ASCII-folded name match over the validated directory. The M5 name-lookup and read helpers that the provider supersedes are removed, not kept beside it. The ABI-v2 module resource imports are served through the same lookup.

The current ROMFS format may remain flat; there is no M9 requirement to add on-ROM subdirectories.

VFS initializes before dynamic module loading. The ROMFS provider is statically available, registers with VFS during bootstrap, and is instantiated as the configured system filesystem at `/SYSTEM`.

Normal boot must therefore not require a separate `romfs_load_zmod()`-style path once VFS bootstrap is complete. After `/SYSTEM` is mounted, module and executable loading consume files through generic VFS-backed source adapters. A tiny direct ROM access primitive remains underneath the ROMFS provider, but it is not a parallel filesystem API.

---

## 32.14 `/SYSTEM` policy

`/SYSTEM` contains the minimum ROM-resident material required to boot ZephyrOS, recover from external-storage failure, and meaningfully diagnose the machine.

The exact contents are build-time configuration, not VFS policy. Typical contents include essential ZMODs, the monitor, possibly the recovery shell, diagnostic assets, and fonts/resources required by the boot environment.

---

## 32.15 Generic block devices

The existing `ZDEV_STORAGE` class is renamed directly to:

```text
ZDEV_BLOCK
```

The numeric class value is preserved; no compatibility alias is part of the architecture.

A block device exposes geometry and raw block I/O:

```c
struct zblock_info {
    uint16_t block_size;
    uint32_t block_count;
    uint16_t max_transfer_blocks;
    uint16_t flags;
};

struct zblock_ops {
    int (*get_info)(void *state, struct zblock_info *out);
    int (*read)(void *state, uint32_t lba, uint16_t count, void *buffer);
    int (*write)(void *state, uint32_t lba, uint16_t count, const void *buffer);
    int (*flush)(void *state);
    int (*media_status)(void *state);
};
```

For current hardware:

```text
sd-ioc.zmod
    |
    | consumes ZDEV_IOC
    v
ZDEV_BLOCK: sd.0
```

The SD block driver translates generic block requests to the MCU raw-SD command/bulk protocol. It does not implement FAT.

**M9 scope:** M9 renames the class and publishes `include/zephyros/block.h` (the `zblock_info` / `zblock_ops` contract and capability bits). No `ZDEV_BLOCK` driver is resident in M9, because nothing consumes one: IOC-FAT talks to `ZDEV_IOC` directly. `sd-ioc.zmod` is built when the first native filesystem (VFAT/Fuzix) needs raw blocks.

---

## 32.16 IOC remains transport only

This is an architectural invariant. `drivers/ioc/` owns SIO1 framing, command and bulk lanes, READY/DONE lifecycle, sequencing, timeouts/recovery and semantic IOC status. It does not own namespace, FAT semantics, file handles, directory traversal, caching policy or native path syntax.

Both raw SD and IOC-FAT may consume IOC:

```text
                    ZDEV_IOC
                    /       \
                   /         \
                  v           v
          SD block driver   IOC-FAT filesystem
               |                  |
          ZDEV_BLOCK           zfs_ops
```

---

## 32.17 IOC-FAT filesystem provider

The MCU FatFs service is exposed as a resident ZMOD filesystem provider:

```text
iocfat.zmod
```

Stack:

```text
VFS
 |
 v
IOC-FAT filesystem provider
 |
 | FS2 filesystem protocol
 v
ZDEV_IOC
 |
 v
MCU FatFs
 |
 v
SD
```

`iocfat.zmod` discovers `ZDEV_IOC`, initializes private provider state, and calls `vfs_register_filesystem()`.

IOC-FAT owns FS2 commands, file/directory tokens, generation/cookie handling, MCU/FatFs error translation, filesystem-specific command/bulk sequencing, and its file-data cache (section 32.19). FS2 tokens never escape into the native application ABI.

The current backend may retain FAT 8.3 limitations if required by deployed firmware. VFS itself must not inherit that limitation.

---

## 32.18 Future native filesystems

The block-device layer deliberately enables Z80-side filesystems:

```text
VFS
 |
 +--> IOC-FAT
 |
 +--> native VFAT
 |       |
 |       v
 |    ZDEV_BLOCK
 |
 +--> Fuzix filesystem
         |
         v
      ZDEV_BLOCK
```

Changing the filesystem provider must not require changes to the shell, native ABI, VFS namespace, CWD semantics or IOC transport.

---

## 32.19 Caching is provider policy

VFS defines no cache. There is no single useful cache key or invalidation model across backends:

```text
ROMFS              no cache (reads come straight from ROM)
IOC-FAT            file-data cache: FS2 file + generation + aligned offset
native VFAT/Fuzix  block cache below the filesystem, keyed by LBA
```

Each provider decides whether it caches and implements the policy that fits its backend. If two providers later need the same mechanism, a shared helper is extracted then, not before.

### IOC-FAT file-data cache (required in M9)

MCU FatFs buffering does not avoid repeated Z80-to-MCU transport transactions, so IOC-FAT keeps a local Z80-side cache in its own module state:

```text
slot:
    owning open file (FS2 token)
    FS2 generation at fill time
    aligned file offset (512-byte block)
    valid-byte count
    data[512]
```

- Slot size is 512 bytes. The slot count is a build setting of the IOC-FAT module, default 1 under the bank-7 budget, raised only when the ledger shows room. It is reported in the bank-7 ledger (section 32.39).
- Read hit returns locally; a miss reads the aligned block through FS2 and fills a slot. Reads crossing a block boundary are split.
- Writes are **write-through**: FS2 write, then confirmed DONE, then update or invalidate overlapping slots. A failed or unconfirmed write changes no cached byte.
- Because FS2 serves at most two open files, a confirmed write, a truncating open, `unlink`, or `rename` also invalidates every slot of the other open file. This is the whole coherence rule.
- A generation change or `STALE` reply invalidates every slot.

No write is committed merely because bytes reached the IOC bulk lane. Write-back may be added later only if measurement justifies its recovery complexity.

---

## 32.20 Future raw block cache

A future native VFAT/Fuzix implementation may cache raw FAT/directory/allocation/data sectors below the filesystem:

```text
native filesystem
    |
    v
generic block cache
    |
    v
ZDEV_BLOCK
```

This is distinct from the IOC-FAT file-data cache. Block caching must not be buried inside IOC.

---

## 32.21 Native process filesystem state

Each process has at least:

```text
current working directory
ownership of zero or more entries in the global descriptor pool
```

The initial native environment starts at `/`. A child inherits its parent's CWD. A child does not initially inherit open file or directory descriptors. On exit, all child descriptors close and child FS context is discarded without altering the parent's CWD/state.

---

## 32.22 Descriptor table

Files and directory iterators share one **global** VFS descriptor pool:

```text
8 descriptors total, each owner-tagged
```

Each entry is `FREE`, `FILE`, or `DIRECTORY` and records its owner PID, mount, provider handle, uint32 offset and access flags. ZephyrOS is single-tasking, so per-process reserved tables would mostly sit empty. A process may use any free entry; a suspended parent's open descriptors stay allocated to it while its child runs. Exhaustion returns `EMFILE`.

The descriptor number a process sees is a small per-process index; one process can never name another process's descriptor.

These are VFS descriptors, not Unix `stdin/stdout/stderr`; console I/O remains its own native syscall family in M9/M10. The pool may grow later without changing filesystem semantics.

---

## 32.23 Current working directory

CWD is a VFS namespace concept, not a provider token exposed to applications. Applications see canonical paths such as `/`, `/SYSTEM`, `/GAMES` and `/CF/WORK`. Internally VFS may cache private node handles, but `chdir()` crosses mounts transparently.

---

## 32.24 Native file handles

Applications receive small ZephyrOS descriptors. Conceptually:

```text
application fd
      |
      v
VFS descriptor
      |-- owner PID
      |-- type: file or directory
      |-- mount
      |-- filesystem-private handle
      |-- access flags
      `-- uint32 current offset
```

File size and position are 32-bit. Individual transfer lengths may remain 16-bit. There is no native 128-byte-record abstraction.

---

## 32.25 Native metadata ABI v1

The first metadata ABI exposes only broadly useful filesystem-neutral information.

Types:

```c
#define ZFS_TYPE_FILE 1u
#define ZFS_TYPE_DIR  2u
```

Attributes:

```c
#define ZFS_ATTR_READONLY 0x01u
#define ZFS_ATTR_HIDDEN   0x02u
#define ZFS_ATTR_SYSTEM   0x04u
```

There is no native `ARCHIVE` attribute.

Zephyr-80 has no RTC. M9 defines:

```c
#define ZFS_TIME_UNKNOWN 0u
```

A zero timestamp means unknown/unavailable. The ABI still uses 32-bit fields so adding a time source later does not change structure layout. M9 exposes `ctime` and `mtime`, not `atime`.

Conceptual `stat()` result:

```c
struct zfs_stat {
    uint8_t  type;
    uint8_t  attributes;
    uint16_t reserved;
    uint32_t size;
    uint32_t ctime;
    uint32_t mtime;
};
```

Conceptual `readdir()` result:

```c
struct zfs_dirent {
    uint8_t  type;
    uint8_t  attributes;
    uint16_t name_length;
    uint32_t size;
    uint32_t ctime;
    uint32_t mtime;
    char name[ZFS_NAME_MAX];
};
```

The ABI does not expose inode numbers, FAT clusters, sectors, FS2 tokens, ROMFS offsets, uid/gid, Unix modes, or access time.

---

## 32.26 Native filesystem syscall family

M9 exposes native services semantically equivalent to:

```text
FS_OPEN      (flags include CREATE, EXCL, TRUNC)
FS_CLOSE
FS_READ
FS_WRITE
FS_SEEK
FS_STAT

FS_OPENDIR
FS_READDIR
FS_CLOSEDIR

FS_MKDIR
FS_RMDIR
FS_UNLINK
FS_RENAME

FS_CHDIR
FS_GETCWD
FS_SYNC
```

Syscall argument blocks use bounded pointer/length fields. User runtime wrappers may expose C strings.

`FS_STATFS` and truncate-to-arbitrary-size are deferred until a consumer
requires them - explicitly **not** scheduled for M10, whose shell and core
commands do not need them (`df` is deferred with them). Truncation to zero
is available in M9 through `FS_OPEN` with `TRUNC`.

---

## 32.27 `CLOSE` and `SYNC`

M9 requires both.

`close(fd)` flushes pending file state as required, releases the provider handle and releases the VFS descriptor.

`sync(fd)` flushes dirty cached/provider state associated with that file through the underlying provider/storage while keeping the descriptor open.

M9 has no filesystem-wide sync operation and no Unix-style global sync command.

---

## 32.28 Byte-oriented I/O

Native ZephyrOS I/O is byte-oriented. EOF is a successful zero-byte read and short reads are valid.

Read path:

```text
application
    |
    v
SYS_FS_READ
    |
    v
VFS descriptor + offset
    |
    v
filesystem.read()
    |
    +--> ROMFS -> ROM access primitive
    |
    +--> IOC-FAT -> cache hit -> return locally
    |              `-> miss -> IOC bulk -> fill slot
    |
    `--> future FS -> optional block cache -> block layer
```

A write advances the file offset only after confirmed successful completion. IOC-FAT retains the existing strict IOC DONE-confirmation semantics.

---

## 32.29 Transfer staging

M9 may reserve one bounded 512-byte bank-7 transfer buffer in runtime/system workspace:

```text
application bank
      ^
      | checked bank-copy gate
      v
512-byte staging buffer
      |
      v
VFS/provider
      |
      v
IOC bulk or block layer
```

It is persistent kernel workspace, not a large automatic stack object, and is not part of the public ABI.

---

## 32.30 Filesystem errors

M9 adds native equivalents of:

```text
EEXIST
ENOTDIR
EISDIR
ENOTEMPTY
EROFS
ESTALE
EXDEV
EMFILE
```

`EMFILE` reports descriptor-pool or provider open-handle exhaustion, distinct from `ENOSPC` (storage full). `ESTALE` supports generation-aware FS2 handles. `EXDEV` is required for cross-mount rename. Numeric values are pinned when the syscall ABI is implemented.

---

## 32.31 Cross-filesystem operations

A same-filesystem rename calls the provider's native `rename()`. A cross-mount rename such as `/FOO.DAT -> /CF/FOO.DAT` returns `EXDEV`.

VFS does not silently turn rename into copy. M10 userland may implement `mv` as copy + successful close/sync + unlink when appropriate.

---

## 32.32 Boot lifecycle and system filesystem

VFS is a foundational kernel service and initializes immediately after the minimal kernel environment is established.

### 32.32.1 Minimal kernel establishment

Before VFS, the kernel establishes only what is required to run safely:

```text
memory mode / bank state
kernel/common stacks
early direct SIO diagnostics
core interrupt state
```

No loadable module is required.

### 32.32.2 VFS bootstrap phase

```text
kernel start
    |
    v
vfs_init()
    |
    v
register built-in filesystem providers
    |
    `--> ROMFS
    |
    v
read generated mount configuration
    |
    v
find the unique system=true entry
    |
    v
instantiate system filesystem
    |
    v
publish /SYSTEM
```

For the initial system:

```text
provider   = romfs
source     = recovery
mountpoint = /SYSTEM
system     = true
```

The system provider must be statically available, have no dependency on ZMODs, dynamically registered devices, or non-system filesystems, and use only primitive facilities already available at bootstrap. Failure is fatal and reported through early direct SIO.

### 32.32.3 Module-loading phase

Once `/SYSTEM` exists, boot assets are loaded through VFS:

```text
/SYSTEM/sio.zmod
/SYSTEM/ioc.zmod
/SYSTEM/hid.zmod
/SYSTEM/v9958.zmod
/SYSTEM/iocfat.zmod
...
```

Conceptually:

```text
VFS file
   |
   v
generic zmod_source.read()
   |
   v
module loader
```

The same source-neutral approach should later be used for ZEX loading.

Loadable modules may register devices, filesystem providers, and other resident services. `iocfat.zmod`, for example, discovers IOC and registers the `iocfat` provider with VFS.

### 32.32.4 Normal mount phase

After resident modules and their providers are loaded, VFS instantiates the remaining configured mounts:

```text
/       -> IOC-FAT, if available
/CF     -> future provider
/SD2    -> future provider
```

For non-system mounts, `required=true` makes failure a boot/configuration failure; `required=false` tolerates failure. The optional root filesystem can therefore fail while `/SYSTEM` remains available for recovery.

### 32.32.5 VFS ready phase

Normal user filesystem syscalls become available only after the configured normal mount pass completes, so applications never observe a partially constructed boot namespace.

Conceptual lifecycle:

```text
VFS_PHASE_BOOTSTRAP
    vfs_init()
    register built-in providers
    instantiate system filesystem
    publish /SYSTEM

VFS_PHASE_MODULES
    load resident modules through VFS
    modules register devices/providers

VFS_PHASE_MOUNT
    instantiate remaining configured mounts

VFS_PHASE_READY
    expose normal process filesystem services
```

A literal phase enum is not required, but this lifecycle and dependency order are architectural.

### 32.32.6 Failure behavior

System filesystem failure:

```text
/SYSTEM unavailable
    |
    v
cannot load required modules
    |
    v
early-console fatal error
```

Normal root failure:

```text
/ fails to mount
    |
    v
/SYSTEM remains available
    |
    v
recovery monitor / diagnostics remain usable
```

---

## 32.33 Build-time mount table generation

Mount policy flows in one direction, from a reviewed manifest to an immutable kernel table to runtime mount state. Nothing flows back, and nothing in the path re-decides policy.

```text
config/mounts.json            (or a test manifest selected by the build)
        |
        |  host build tool: validate (32.5.1), never rewrite
        v
generated C mount table       (build output only, never committed)
        |
        |  compiled and linked into the bank-7 kernel image as const data
        v
immutable mount configuration (ROM-loaded, read-only to VFS)
        |
        |  VFS bootstrap and mount phases (32.32)
        v
runtime mount state           (bank-7 RAM, owned by VFS)
```

### 32.33.1 Inputs

The build selects exactly one manifest per image. Production images use `config/mounts.json`. Acceptance images may select a test manifest, which goes through the same tool and rules. The build also passes the tool the list of providers linked into the kernel image (M9: `romfs`). That list is what lets the tool enforce the system-filesystem rule in section 32.5.1.

### 32.33.2 Generated table

The tool emits a single generated C source containing a constant array, one record per manifest entry in manifest order, and an entry count. Conceptually:

```c
const struct vfs_mount_config vfs_mount_table[] = {
    { "/SYSTEM", 7u, "romfs",  "recovery", VFS_MOUNT_SYSTEM   },
    { "/",       1u, "iocfat", "ioc.0",    VFS_MOUNT_OPTIONAL }
};
const uint8_t vfs_mount_count = 2u;
```

Each record carries the canonical mount path and its length, the provider name, the opaque source string, and flags:

```text
VFS_MOUNT_SYSTEM     the unique bootstrap filesystem (system = true)
VFS_MOUNT_REQUIRED   non-system mount whose failure is a boot failure
VFS_MOUNT_OPTIONAL   non-system mount whose failure is tolerated
                     (required = false or absent)
```

The record layout is a kernel-private build contract between the tool and VFS. It is neither a native application ABI nor a module ABI, and may change with the kernel. The generator output is deterministic (stable order, no timestamps or host paths), so rebuilds stay byte-identical. It lives only in the build directory.

### 32.33.3 Configuration versus runtime state

The generated table is `const` data in the ROM-loaded kernel image. VFS reads it and never writes it.

VFS keeps a separate runtime array in bank-7 RAM, indexed in parallel with the configuration table. Each runtime entry records:

```text
state         UNMOUNTED, MOUNTED or FAILED
provider      the registered provider selected by name
fs_state      the provider instance returned by mount()
last error    the failure code, for diagnostics
```

Separating the two keeps policy auditable in ROM and keeps all mutable state with VFS.

### 32.33.4 Consumption at boot

1. **Bootstrap (32.32.2).** VFS registers the built-in providers and selects the single `VFS_MOUNT_SYSTEM` record. It re-checks that exactly one exists; the build already guarantees this, and a violation is fatal. VFS looks up the record's provider by name in the provider registry and calls `mount(provider_state, source, &fs_state)`. The source string is passed through unchanged as an opaque pointer into the table. On success the entry becomes `MOUNTED`; on failure boot stops through the early console.
2. **Module phase (32.32.3).** No mount records are consumed. Modules may register additional providers.
3. **Mount phase (32.32.4).** VFS walks the remaining records in table order, with the same lookup and `mount()` call. A missing provider counts as a mount failure. `REQUIRED` failures are fatal; `OPTIONAL` failures mark the entry `FAILED`, print a diagnostic, and leave the rest of the namespace usable.
4. **Ready (32.32.5).** The runtime state is now fixed for the life of the boot.

### 32.33.5 Use after boot

Path resolution (32.8) considers only `MOUNTED` entries. Selection is flat: the first path component is compared case-insensitively with the mounted root-level mountpoints; a match selects that mount, otherwise the `/` mount is selected. The remainder becomes the provider-relative path, which always begins with `/`.

Root enumeration (32.7) lists the `MOUNTED` entries whose path has exactly one component, plus the entries of the filesystem mounted at `/`.

A `FAILED` or never-mounted entry contributes nothing to the namespace. In particular, a failed optional `/` leaves only the root-level mountpoints, such as `/SYSTEM`, visible.

### 32.33.6 Invariants

- Changing mount topology means editing the manifest and rebuilding; no kernel source changes.
- VFS source contains no Zephyr-80 paths, provider names, or source strings.
- The ROM table is exactly what the reviewed manifest says. Invalid manifests fail the build rather than being corrected.
- Built-in provider membership is enforced twice: by the tool at build time and by the provider lookup at bootstrap.

---

## 32.34 M9 non-goals

M9 does not implement:

- runtime `mount` / `umount`;
- drive letters;
- CP/M USER areas;
- FCBs;
- CP/M extents;
- 128-byte record I/O;
- symbolic links;
- permissions/users/groups;
- pipes;
- descriptor inheritance;
- shell redirection;
- automatic filesystem probing;
- arbitrary nested mountpoints;
- hot-plug policy beyond clean failure detection;
- write-back file caching;
- a VFS-level cache of any kind;
- generic raw block caching unless a native filesystem actually requires it;
- a resident `ZDEV_BLOCK` driver (`sd-ioc.zmod`) before a consumer exists;
- `FS_STATFS` and truncate-to-arbitrary-size (deferred until a consumer
  requires them, not M10);
- provider `unmount`, `syncfs`, `truncate`, `statfs`, or node lookup operations;
- dynamic allocation inside VFS.

---

## 32.35 Personality boundary

Later personalities consume the native VFS rather than redefine native storage.

A CP/M personality may synthesize FCB, drive, USER and 128-byte-record semantics over native VFS paths/directories. Drive letters and USER areas therefore remain personality policy, not VFS concepts.

---

## 32.36 Architectural invariants

M9 preserves these rules:

1. **VFS owns the global namespace.**
2. **VFS initializes before dynamic module loading.**
3. **Mount topology is static JSON build configuration in M9, compiled into an immutable kernel table that VFS executes at boot (section 32.33).**
4. **Exactly one configured mount is the system filesystem.**
5. **The system filesystem provider is statically available and has no dependency on loadable modules or dynamically registered devices.**
6. **The system filesystem is instantiated before resident modules are loaded.**
7. **After the system filesystem is mounted, subsequent boot assets are loaded through VFS.**
8. **Native ZephyrOS has no drive letters.**
9. **Native path comparison is ASCII case-insensitive.**
10. **`ZOS_PATH_MAX = 128`.**
11. **`ZFS_NAME_MAX = 64`.**
12. **ROMFS is a filesystem provider mounted at `/SYSTEM`.**
13. **The normal writable SD FAT filesystem is mounted at `/`.**
14. **Filesystem providers do not know their external mountpoint.**
15. **IOC is transport, not a filesystem or block-device API.**
16. **`ZDEV_STORAGE` is renamed directly to `ZDEV_BLOCK`.**
17. **`ZDEV_BLOCK` is defined in M9; its first resident driver arrives with the first consumer (native VFAT/Fuzix).**
18. **IOC-FAT is a resident ZMOD filesystem provider registered with VFS.**
19. **MCU FatFs support is one filesystem backend, not the VFS architecture.**
20. **Future VFAT/Fuzix-style filesystems may consume `ZDEV_BLOCK` directly.**
21. **Native I/O is path-, descriptor-, and byte-oriented.**
22. **VFS has one global, owner-tagged pool of 8 descriptors shared by files and directories.**
23. **M9 provides both `CLOSE` and `SYNC`.**
24. **Timestamp value zero means unknown/unavailable.**
25. **The native metadata ABI exposes file/dir type, readonly/hidden/system attributes, size, ctime, and mtime.**
26. **The FAT archive bit is not exposed by native VFS.**
27. **Caching is filesystem-provider policy; VFS defines and owns no cache.**
28. **IOC-FAT keeps a write-through 512-byte-slot Z80 file-data cache; ROMFS keeps none.**
29. **The IOC-FAT cache slot count is build-time configurable and reported in the bank-7 ledger.**
30. **CP/M semantics remain confined to a later personality layer.**
31. **Failure of external storage must not remove `/SYSTEM` recovery access.**
32. **Providers are path-based; VFS keeps no node objects and matches only root-level mountpoints itself.**
33. **Bank-7 residency is budgeted (section 32.39); each package reports against the ledger.**

---

## 32.37 Suggested source ownership

```text
kernel/
    vfs/
        namespace.c
        path.c
        mount.c
        file.c
        dir.c
        syscall.c

fs/
    romfs/
        ...

    iocfat/          # includes the IOC-FAT file-data cache
        ...

    fat/                 # future
        ...

    fuzix/               # future
        ...

drivers/
    ioc/
        ...

    sd/              # future: first ZDEV_BLOCK driver, with its first consumer
        ...

include/zephyros/
    vfs.h
    fs.h
    block.h

config/
    mounts.json
```

This is architectural guidance, not a requirement to create every file immediately. Executable behavior remains outside `platform/zephyr80/`; platform files contain hardware facts/configuration only.

---

## 32.38 M9 architecture summary

```text
                         kernel bootstrap
                                |
                                v
                              VFS
                    owns one namespace: /
                    paths / CWD / fd pool
                                |
                     system=true mount
                                |
                                v
                            /SYSTEM
                              ROMFS
                                |
                      load resident ZMODs
                                |
                                v
                    register normal providers
                                |
                                v
          +---------------------+---------------------+
          |                                           |
          v                                           v
      /SYSTEM                                      /
       ROMFS                                    IOC-FAT
      (no cache)                           (file-data cache)
          |                                           |
          |                                           v
          |                                        ZDEV_IOC
          |                                           |
          |                                           v
          |                                          MCU
          |                                           |
          |                                           v
          |                                          SD
          |
          +---------------- future -------------------+
                                |
                         native VFAT / Fuzix
                                |
                                v
                           block cache
                                |
                                v
                           ZDEV_BLOCK
                                |
                                v
                           SD-over-IOC
                                |
                                v
                             ZDEV_IOC
```

The system-filesystem rule makes VFS part of the boot foundation rather than a service layered on top of module loading: the kernel mounts one dependency-free built-in filesystem first, then uses it to obtain the modules and providers required to construct the rest of the machine.

### 32.38.1 Implementation notes (stage 25)

The bootstrap slice above is implemented by work package
`25-m9-vfs-bootstrap-romfs` (with corrections 01 and 02); `docs/vfs.md` is
the implementation record.

```text
abi/zos/fs.h             native metadata/limit ABI (stat, dirent, flags;
                         no statfs) -- syscall blocks arrive in pkg 27;
                         include/zephyros/fs.h re-exports it
include/zephyros/vfs.h   provider ABI: zfs_ops (the 14 path-based
                         operations of 32.12), zfs_provider
kernel/vfs/              mount_config.h, path.c, mount.c, file.c, dir.c,
                         mutate.c
fs/romfs/romfs_provider  ROMFS over the M5 reader, path-based (pkg 25)
tools/mountgen.py        manifest -> build/generated/vfs_mounts.c
```

Implemented: normalization; flat first-component mount selection
(section 32.8, so `/SYSTEMX` does not select `/SYSTEM`); the capacity-4
provider registry; the four lifecycle phases; the kernel file API on
caller-provided `vfs_file` storage plus the `zmod_source` adapter; the
mutation entry points that turn a null provider operation into `ZOS_EROFS`;
one ASCII case-folded ROMFS lookup (section 32.13) serving the provider,
the ZEX launcher and the module resource imports; `/SYSTEM` mounted by
`vfs_init()` before any module load; every boot ZMOD and
`monitor.zex` opened by VFS path; `vfs_mount_all()` for the remaining
configured mounts; and the generated static mount table with its
validation rules.

VFS keeps no node objects, no object identity and no cache: providers are
path-based, so there is no per-component walk and no `object_id`.  The
`ZOS_ROMFS_BOOT=0` embedded-catalog images link no VFS byte at all
(correction-01): the object group is dropped and `kernel/process/exec.c`
compiles its M6 ROMFS body instead of the VFS body.

Deliberately deferred at stage 25 (all since implemented): descriptors and
syscalls (package 27), provider-local caching (IOC-FAT, package 30), a
writable provider and the `/` mount (packages 26/30).  Cross-filesystem move
semantics are implemented by the VFS rename path (architecture 32.31): a
rename across mounts returns `ZOS_EXDEV` and VFS never copies.
`romfs_load_zmod()` no longer exists.

Budget (section 32.39): package 25 measured production `_CODE` ending at
`7BC5h` with a `7C00h-C000h` arena — 34 granules against the unchanged
32-granule floor — and `_DATA` of 1294 bytes.  Package 25b then added the
size-optimized compile and the derived data base: the current ledger is
`code 21689 bytes end 74B9h | arena 49 granules | data 1309 bytes at
D800h | isr 256 | slack 483`, recorded in `docs/memory.md` section 2.2.1.
Correction-01 trimmed the V9958 font staging (`4008/724 -> 4038/460`
image/BSS), so that module needs 9 granules and the boot set uses 20 of
the 49 (`sio x1, ioc x8, hid x2, v9958 x9`), leaving 29 free.
The per-object package-25 sizes and the correction-02 saving are in
`docs/vfs.md` section 9.

---

### 32.38.2 Implementation record (M9 close)

The sections above are the architectural contract; these are the documents
that record what was actually built, and the acceptance that proves it:

```text
docs/vfs.md            VFS core, provider ABI, descriptor/CWD model,
                       syscalls, ROMFS and IOC-FAT providers
docs/abi.md            native syscall ABI: FS_* family, SYS_EXEC and the
                       startup block v1 argument contract, error values
docs/devices.md        device/class model, driver ABI v3, ZDEV_BLOCK and the
                       resident IOC-FAT provider
docs/module-format.md  ZMOD v1 and the loader lifecycle at ABI v3
docs/memory.md         bank-7 geometry, the M9 runtime-data inventory, the
                       common-memory budget and the final ledger
docs/boot.md           the boot path and the VFS lifecycle
docs/sdk.md            the native user SDK and its ZEX programs
docs/validation/2026-10-05-m9-filesystem.md
                       the M9 acceptance matrix, transcripts and hashes
```

The milestone's own acceptance image is `make m9-selftest`: the production
boot path with `/` served by IOC-FAT from the card, running the native
`fstest.zex` (which exec's the deep-matrix program `fsfat.zex`), the
`fsdeep.zex` depth probe and the two fault windows, then the unchanged
monitor launcher.  The per-package images (`m9-vfs`, `m9-provider`,
`m9-fs`, `m9-exec`, `m9-iocfat`) remain as their packages' evidence and as
subsystem regression coverage.

Deliberately deferred beyond M9: `FS_STATFS` and arbitrary-size truncate
(no M9 consumer), runtime unmount, the first `ZDEV_BLOCK` driver (it
arrives with the first native VFAT/Fuzix consumer), and every
compatibility/personality layer.  Nothing in M9 is kept as a
backward-compatibility path.

## 32.39 Bank-7 residency budget

Bank 7 holds everything resident in OS mode except common memory. The kernel executive, the resident driver/provider modules and the runtime data share one window:

```text
2000h ............ kernel executive (_CODE, linked)
align 512 ........ resident ZMOD arena (derived)
derived .......... runtime data (_DATA/_INITIALIZED, derived down from DF00h)
DF00h-DFFFh ...... ISR stack
```

After the first M9 slice (package 25), the kernel executive was 24.3 KiB and the original M9 design projected about 48 KiB of resident code at SDCC density, beyond the window. M9 therefore treats resident bytes as an architectural resource. Because the arena start follows the kernel end and the arena end follows the data base, kernel code, module code and runtime data are interchangeable within the window. The budget is therefore one total, not separate per-kind limits:

```text
2000h-DEFFh  47.75 KiB  kernel + resident ZMODs + runtime data + ISR stack
-------------------------------------------------------------------------
production arena floor   >= 32 granules, holding the production boot set
slack                    >= 1 KiB at the end of M9
```

The end-of-M9 slack target was lowered from 2 KiB to 1 KiB by the project
owner on 2026-10-05. The reason is measured, not projected: the finished
IOC-FAT provider needed 15 granules (7441 bytes) as first written, which
exactly consumed the production arena (35 granules, 0 free, 63 bytes of
data slack). The correction trimmed it to 13 granules (6643 bytes) and kept
the required one-slot write-through cache, leaving 2 free granules plus 63
bytes of data slack: 1087 bytes, above the 1 KiB target but below 2 KiB.

Rules:

- Kernel and modules are compiled for size. User programs keep their own flags.
- The runtime-data base is derived from the linked data size, so unused data space goes to the arena (no fixed `C000h` boundary).
- Every package reports a one-line `bank7:` ledger before and after, and the bytes its own code and data add.
- Reserved data is sized from measurement, not round numbers. For example, exec frame saves follow the measured worst-case gate-stack depth.
- Provider caches are sized by budget. The IOC-FAT cache defaults to one 512-byte slot unless the ledger shows room for more (section 32.19).
- A mechanism that M9 replaces is deleted, not kept as a compatibility path. ZephyrOS is still under construction: no interface, binary format, image or transcript carries a backward-compatibility obligation. Completed milestones prove mechanisms; they do not freeze them.
- A feature with no consumer is not made resident (for example `sd-ioc.zmod`, `FS_STATFS`).
- Supervisor checkpoints compare the measured ledger with the remaining projection: after package 26 (passed 2026-10-05: 26 free granules, 291 bytes of slack, projected end-of-M9 slack 1.4-3.2 KiB before the measures above), and after package 28, before IOC-FAT is built.
- Package 30 (passed 2026-10-05) is the last bank-7 consumer: `bank7: code 28476 bytes end 8F3Ch | arena 35 granules (33 boot, 2 free) | data 2241 bytes at D600h | isr 256 | slack 63 bytes`, i.e. 2 free granules and 1087 bytes of total slack against the 1 KiB target. `iocfat.zmod` is 6006 image + 637 BSS = 6643 bytes, 13 granules, 13 bytes under the 6656-byte ceiling.
- Package 30c (passed 2026-10-05) then removed dead kernel code and shared one ASCII name helper, moving the arena start one granule down.  Package 31 changes no kernel or module byte; it is the acceptance and documentation package.  The **final end-of-M9 ledger** is:

```text
bank7: code 27990 bytes end 8D56h | arena 36 granules (33 boot, 3 free) | data 2241 bytes at D600h | isr 256 | slack 63 bytes
common: used 2104 of 7424 bytes (code 1410, data 694) | ceiling 3072, m10 512, limit 2560
```

  i.e. 3 free granules plus 63 bytes of runtime-data slack = **1599 bytes** of total slack, above the 1 KiB target, with the 32-granule arena floor and the common ceiling both held.  Boot set: `sio x1, ioc x8, hid x2, v9958 x9, iocfat x13` = 33 granules.
- If the projection falls below the slack target, the choice between descoping and a second kernel bank goes back to the project owner. It is never made by raising a limit or lowering the arena floor.

---

## 32.40 Native process exec (package 28)

`SYS_EXEC 05h` (`docs/abi.md` section 11) turns the M6 launcher into a
general parent/child mechanism: any ordinary process may run a child
synchronously and receives its exit status, and a child may do the same.
Everything is the existing machinery - one process table, one bank
allocator, one ZEX loader, one page-zero path, one VFS process hook, one
exit path - plus a bounded continuation stack for the shared gate stack.

Geometry:

```text
depth 0 (kernel launcher, boot stack)      no saved gate frame
depth 1 (first user process)               kernel continuation
depth 2..ZOS_EXEC_MAX_DEPTH                frame save #depth-2
the next exec at the limit                 ZOS_EBUSY, nothing allocated
```

Suspension and resume:

- `process_prepare()` does every fallible thing (path copy-in, slot, bank,
  CWD start, load/validate, page zero, close) and returns; only then does
  `process_launch()` enter the common gate. Nothing past the suspension
  point can fail, so no rollback of suspended processes exists.
- The gate saves the launcher's SP/IX/latch/IFF per depth and copies the
  gate frame `[saved_sp, zos_gate_stack_top)` into that depth's save area
  (48 bytes, measured 32 + margin). The child's own syscalls reuse the
  same 256-byte common gate stack, clobbering the suspended frame; the
  resume copies it back and restores SP, IX, latch and IFF before the
  parent's `SYS_EXEC` returns.
- From the start of the resume's `LDIR` until SP is restored there is no
  `CALL`, `PUSH`, `POP` or interrupt enable: the frame being restored is
  the one the code is running on.
- The syscall gate's own transition state is single-depth, so the gate
  shadows the launcher's copy per depth as well; without it a resumed
  parent would return through the gate into its child's bank.
- The continuation state is common-resident because the resume gate
  reads it while it changes the mapping and switches stacks.  The
  `SYS_EXEC` path buffer is bank-7 `_DATA` (it is only touched in mode
  11), and common-memory usage is reported on its own `common:` ledger
  line beside the `bank7:` line, because common memory is the scarcest
  region in the machine.  Nothing is placed there for isolation: mode 10
  maps common memory exactly as mode 11 does.

Failure mapping is pinned in `docs/abi.md` section 11 (`ENOENT`,
`EISDIR`, `EINVAL`, `ENOSPC`, `EBUSY`, provider errors passed through),
because user-visible error semantics matter more here than in the kernel
launcher's internal `ZEX_EXEC_ERR_*` space.

Startup block (package 28b):

- Every native process receives a versioned startup block v1 in its own
  page zero at `0080h` (`version`, `tail_length`, raw `tail`, NUL), and the
  launch gate enters it with `HL = 0080h` (section 26.3).
- `SYS_EXEC`'s block gains `{args, args_length}`: the kernel copies the
  caller's raw tail byte-exact and never parses it; user space owns
  splitting (`user/sdk/include/zos/process.h`, docs/zex-format.md section 5.1).
  A tail over 125 bytes or outside `0100h-DFFFh` is `ZOS_EINVAL` in the
  preflight, and the copy is made at the end of `process_prepare()` once
  the executable source is closed and the VFS workspace is free, through
  the existing checked bank-copy gate into the VFS staging view.  No tail
  is ever stored: there is no persistent kernel argument buffer, in bank 7
  or common memory.
- The boot program is a build setting, `ZOS_BOOT_PROGRAM`
  (`config/system.mk`), validated against the recovery manifest and
  compiled into the kernel as a constant string.  Switching to the M10
  shell is that one line plus the ZEX in the manifest, with no kernel
  edit.

Acceptance: `make m9-exec-selftest` (extended `fstest.zex` exec case plus
the recursive `fsdeep.zex` depth probe) replayed by
`tests/test_m9_exec_sim.py`, which measures the copied frame against
`ZOS_EXEC_FRAME_SAVE`, compares the restored frame byte-for-byte with the
suspended one at every depth, and checks the parent returns over its own
bank; `tests/test_m9_exec.py` pins the ABI, the geometry, the launch
primitive's structure and the production map.

---

# 33. M9 acceptance criteria

M9 acceptance uses an ordinary native program such as:

```text
fstest.zex
```

loaded from `/SYSTEM`. It exercises the public native filesystem ABI, not IOC or FS2 directly.

Acceptance covers at least:

```text
namespace:
    /
    /SYSTEM
    root mountpoint visibility
    mountpoint shadowing
    case-insensitive lookup
    absolute paths
    relative paths
    .
    ..
    CWD crossing between / and /SYSTEM

ROMFS:
    open/read/stat
    directory enumeration
    mutation returns EROFS

IOC-FAT:
    open/create/close
    open with TRUNC
    read/write
    sync
    seek
    stat
    mkdir/rmdir
    rename
    unlink

sizes:
    1
    127
    128
    129
    511
    512
    513
    >64 KiB

IOC-FAT cache:
    repeated-read hit
    miss/fill
    cross-slot reads
    write-through update/invalidate
    no stale data after a write through the other open file
    generation invalidation

error paths:
    missing file
    wrong object type
    descriptor-pool exhaustion (EMFILE)
    stale backend handle
    unavailable SD
    failed confirmed write
    cross-filesystem rename -> EXDEV

process lifecycle:
    child inherits CWD
    child CWD changes do not alter parent
    child descriptors close on exit
    repeated exec/exit leaks no handles
    nested exec chain reaches exactly ZOS_EXEC_MAX_DEPTH levels
    the level past the limit is refused with EBUSY and leaks nothing
    exit-status round trip, including a raw-return ZOS_EFAULT
    the suspended gate frame is restored byte-for-byte at every depth
```

budget:
    final bank-7 ledger meets section 32.39

Physical hardware remains authoritative for SD media behavior, IOC bulk transfers, media/reset handling, confirmed writes and cache behavior under real transport timing.

---

# 34. Milestone summary

M9 closed on 2026-10-06 (project-owner acceptance). Package 31 completed the
implementation, the host/simulator acceptance and the documentation. The
physical rows were accepted as follows, as recorded in
[`docs/validation/2026-10-05-m9-filesystem.md`](./validation/2026-10-05-m9-filesystem.md)
section 8:
- production with and without a FAT card, and the monitor launching from
  `/SYSTEM`, **passed on byte-identical production bytes** (ROM `f4da0496…`,
  physically run in package 30c);
- the `m9-selftest` integration image and the M8 fallback rows were
  **accepted by the project owner without a fresh bench run**. No code
  changed after the last physical validation, the same IOC-FAT paths passed
  physically in package 30, and the fallback rows' simulator replays pass.

```text
M0  COMPLETE
M1  COMPLETE
M2  COMPLETE
M3  COMPLETE
M4  COMPLETE
M5  COMPLETE
M6  COMPLETE
M7  COMPLETE (host + physical acceptance passed)
M8  COMPLETE (host + physical acceptance passed)
│
├── dynamic class-aware device stack
├── IOC transport
├── HID/V9958 console
└── serial recovery/fallback
    │
    ▼
M9  COMPLETE (host + simulator; physical by byte identity + owner acceptance)
│
├── VFS owns one namespace rooted at /
├── early system filesystem mounted at /SYSTEM
├── VFS-backed boot module loading
├── filesystem-provider registration API
├── path-based provider ABI
├── IOC-FAT ZMOD provider mounted at /, with its own 512-byte file-data cache
├── ZDEV_BLOCK class defined (first driver deferred to its first consumer)
├── global descriptor pool, native path/descriptor/byte-oriented syscall ABI
└── budgeted bank-7 residency
    │
    ▼
M10 extended memory / far code+data + ZEX v2        (see section 36)
M11 complete native diagnostic suite                (see section 38)
M12 native shell and core built-in userland         (see section 40)
    │
    ▼
---------------- native ZephyrOS complete ----------------
    │
    ▼
compatibility/personality architecture              (deferred)
```

The end-of-M9 bank-7 ledger and the require-before-M10 residency finding are
in sections 32.39 and 35.

---

# 35. Native completion sequence after M9

The native sequence after M9 is:

```text
M10  Extended memory / far code / far data
     + ZEX v2

M11  Complete native diagnostic suite

M12  Native shell and core built-in userland

---------------- native ZephyrOS complete ----------------

Later compatibility/personality architecture
```

The ordering is deliberate:

1. M10 completes native kernel/executable mechanisms.
2. M11 validates and exercises the completed native mechanisms and hardware.
3. M12 builds the normal interactive environment entirely on those completed mechanisms.
4. Compatibility personalities begin only after native ZephyrOS is complete.

M9 already supplies the process-startup and argument-tail contract required by the later shell. Package 28b establishes startup block v1 and synchronous `SYS_EXEC` argument transport; M10 does not redesign that contract.

M9 acceptance and the section-32.39 residency budget remain binding. M10 does not weaken the production arena floor or redefine M9 acceptance to make later work fit.

M9 facts the target relies on, all unchanged by M10-M12:

```text
startup block v1 at 0080h, and the raw-tail argument contract of SYS_EXEC
  (package 28/28b; user-space splitting only, kernel never parses)
ZOS_BOOT_PROGRAM (config/system.mk) as the single boot-program selector
the native user SDK (user/sdk, docs/sdk.md): a program includes only "zos.h"
  and links crt0 plus libzos; it never includes a kernel header
the bank-7 residency budget of section 32.39, including the 32-granule
  production arena floor and the common-memory ceiling pinned by package 30c
  (ZOS_COMMON_CEILING 3072 bytes, of which 512 is held for M10's far-call
  gate and state, so M9 enforces used <= 2560)
```

**Residency decision required before M10.** The measured end-of-M9 ledger is
`bank7: code 27990 bytes end 8D56h | arena 36 granules (33 boot, 3 free) |
data 2241 bytes at D600h | isr 256 | slack 63 bytes` (section 32.39),
i.e. 3 free granules plus 63 bytes of runtime-data slack = **1599 bytes**
of mixed kernel room. The 30b audit's low M10 estimate (`SYS_XMEM_*`,
unified bank ownership, the ZEX v2 loader, the far-entry table,
`SYS_FAR_CALL` and `SYS_ZEX_SEGMENT_INFO`) is about **1530 bytes of code
plus 136 bytes of data**, i.e. 1666 bytes - more than the ledger holds. The
reclaim package 30c implemented gained one granule (F7) but not the
exec-path-buffer share (F4), so the favourable `1727`-byte case did not
materialise. Per audit section 7 and section 36.18, a **kernel-residency
architecture decision (for example a second kernel bank, or relocating a
resident mechanism) is required before M10 implementation**; the 32-granule
arena floor and the common-memory ceiling are not to be weakened to make M10
fit.

# 36. Milestone M10 — Extended memory, far code/data, and native execution completion

## 36.1 Objective

M10 completes the native ZephyrOS memory and executable mechanisms required by applications before the diagnostic suite and native shell are built.

Its responsibilities are:

```text
extended/far memory
    process-owned auxiliary memory objects
    allocations smaller than a physical 64-KiB bank
    bounded copy access to far data

extended native executables
    ZEX v2
    auxiliary segments
    relocatable far code/data
    executable-entry metadata

far execution
    synchronous far calls
    safe bank transition and return
    ordinary native syscalls while far code runs
```

Process startup, argument-tail transfer, nested `SYS_EXEC`, CWD inheritance, descriptor cleanup and parent restoration are already established before M10 and are preserved.

After M10, later native milestones must not require another fundamental memory, executable-format, or process-lifecycle mechanism merely to implement diagnostics or the shell.

The central rules are:

> **Physical banks are a Zephyr-80 implementation detail. Native applications consume memory objects, segments, and handles.**

> **The primary application bank remains the ordinary execution environment. Far memory extends it; it does not replace it.**

> **Executable auxiliary memory is created only by the validated ZEX loader. Runtime far-memory allocation is data-only.**

---

## 36.2 Existing primary-bank model remains

Every native process continues to own one primary application bank.

Normal execution remains:

```text
0000h-00FFh   native page zero
0100h-....    primary ZEX image/data
....          free primary memory
....-DFFFh    primary stack
E000h-FFFFh   permanent common memory
```

Mode `10` remains the normal application view and mode `11` remains the kernel view.

A ZEX that needs no auxiliary segments still behaves like the existing single-bank native program from the application's perspective.

M10 does not turn ordinary native programs into bank-aware programs.

---

## 36.3 Auxiliary-bank geometry

Application banks remain physical SRAM banks `0-6`. Bank 7 remains kernel-owned unless a separately approved later kernel-residency architecture explicitly changes that ownership model.

A bank used as an application auxiliary bank has a different software ownership layout from a primary process bank:

```text
0000h-00FFh   ZephyrOS-owned page zero
0100h-1FFFh   auxiliary execution/control window
2000h-3FFFh   allocation slot 0
4000h-5FFFh   allocation slot 1
6000h-7FFFh   allocation slot 2
8000h-9FFFh   allocation slot 3
A000h-BFFFh   allocation slot 4
C000h-DFFFh   allocation slot 5
E000h-FFFFh   permanent common memory
```

Thus one auxiliary bank provides:

```text
6 x 8 KiB = 48 KiB
```

of allocatable far storage.

The low 8 KiB is deliberately not allocatable. It preserves the same architectural property used by ordinary syscalls: when mode `11` is entered, `0000h-1FFFh` remains visible from the currently selected bank.

This area holds the auxiliary page zero, runtime scratch, far-call frame and auxiliary execution stack.

The exact subdivision inside `0100h-1FFFh` is single-sourced in the platform memory description and validated by the linker/build tools.

---

## 36.4 Allocation granularity is not part of the native ABI

The Zephyr-80 allocator uses 8-KiB slots because they fit the machine's physical memory geometry.

Applications do not request:

```text
bank 5, slot 2
```

They request:

```text
memory object of N bytes
```

and receive an opaque handle.

The kernel rounds the request to the platform allocation granularity.

For example:

```text
native request          Zephyr-80 implementation
------------------------------------------------
allocate 6000 bytes  -> one 8-KiB slot
allocate 12000 bytes -> two contiguous 8-KiB slots
```

A single Zephyr-80 far object must fit in one auxiliary bank and therefore cannot exceed 48 KiB.

A program requiring more storage allocates multiple objects.

This restriction is platform-specific and must not be encoded into the portable meaning of the native memory API.

---

## 36.5 Unified ownership of application banks

The kernel owns one allocator for application banks.

A physical application-capable bank may be:

```text
FREE
PRIMARY(process)
AUXILIARY
```

A primary process bank owns the complete bank and cannot contain auxiliary allocations.

An auxiliary bank may contain multiple 8-KiB allocations.

Allocation policy should preserve whole banks for future `SYS_EXEC` operations. On Zephyr-80 the intended policy is:

```text
primary process allocation: low free bank first
auxiliary allocation:       high bank first, pack existing auxiliary banks first
```

This minimizes fragmentation without making placement observable through the native API.

A bank containing even one live auxiliary allocation is not eligible to become a primary process bank.

Consequently `SYS_EXEC` may return `ZOS_ENOSPC` even when free auxiliary slots exist if no completely free primary bank remains.

That is a valid resource-exhaustion condition.

---

## 36.6 Process ownership and lifetime

Every auxiliary allocation has exactly one owning process.

Ownership applies equally to:

```text
runtime far-data objects
ZEX auxiliary segments
far-code segments
loader-created BSS
```

When a process exits, ZephyrOS releases all auxiliary resources owned by that process.

A suspended parent retains its auxiliary resources while a child runs.

A child does not inherit the parent's auxiliary allocations.

The child receives only:

```text
its primary bank
its own ZEX auxiliary segments
its own later runtime allocations
its inherited CWD and normal process-startup state
```

Failed load, failed relocation, failed far-segment allocation, malformed ZEX metadata or failed launch must roll back every allocation made for the attempted child.

No partial process may become visible.

---

## 36.7 Native extended-memory syscall family

M10 should use the currently available native syscall space for a compact extended-memory family. Exact numbers are pinned when the ABI is implemented.

The semantic operations are:

```text
SYS_XMEM_ALLOC
SYS_XMEM_FREE
SYS_XMEM_READ
SYS_XMEM_WRITE
SYS_XMEM_INFO
SYS_ZEX_SEGMENT_INFO
SYS_FAR_CALL
```

### `SYS_XMEM_ALLOC`

Semantics:

```text
request:
    size
    flags

result:
    opaque handle
```

Runtime allocations are:

```text
process-owned
zero-initialized
read/write
non-executable
```

An allocation of zero bytes is invalid.

### `SYS_XMEM_FREE`

Releases a runtime allocation owned by the calling process.

Loader-created ZEX segments cannot be freed individually.

Invalid, stale or foreign handles are rejected.

### `SYS_XMEM_READ` / `SYS_XMEM_WRITE`

Transfer between:

```text
the calling execution bank
        and
the named far-memory object
```

using:

```text
handle
object offset
caller buffer
length
```

All range arithmetic is checked before any copy occurs.

The caller never maps the object's physical bank directly through this API.

### `SYS_XMEM_INFO`

Returns a versioned logical description:

```text
requested size
allocated size
flags
object type
```

Normal applications are not told the physical bank or slot.

Physical placement belongs only to diagnostics.

### `SYS_ZEX_SEGMENT_INFO`

Returns the logical state of one loader-created auxiliary segment identified by its ZEX segment ID.

It exposes semantic information such as:

```text
segment size
segment flags
loaded/present state
```

but not a writable physical mapping.

### `SYS_FAR_CALL`

Invokes a declared far-code entry point synchronously.

Section 2.13 defines the execution contract.

---

## 36.8 ZEX v2 replaces ZEX v1

M10 replaces ZEX v1 with ZEX v2.

There is no production compatibility path for ZEX v1.

ZephyrOS is still under construction, and executable formats from earlier milestones carry no backward-compatibility obligation. All first-party native programs, tools and tests are converted to ZEX v2 as part of M10.

The ZEX-v1 parser, packer path and format-specific compatibility code are removed once the ZEX-v2 toolchain and loader are accepted.

A ZEX v2 executable contains:

```text
header

primary image descriptor
auxiliary segment table
far-entry table
relocation tables

primary image payload
auxiliary segment payloads
```

A program with no auxiliary segments is simply the minimal form of ZEX v2.

Its primary image continues to load at logical `0100h`.

---

## 36.9 ZEX v2 segment model

The primary segment preserves the current normal process model.

Auxiliary segments are independently described.

Each auxiliary segment contains semantically:

```text
segment id
flags
image size
BSS size
required allocation size
payload location
relocation-table location/count
```

Useful flags include at least:

```text
EXEC
WRITE
```

The flags describe intended use; the Z80 has no hardware protection enforcing them.

An auxiliary segment may contain:

```text
code
constant data
initialized writable data
BSS
```

that must be directly mutually addressable while that segment is selected.

Code and its private static data should therefore normally be packaged into one segment rather than creating cross-bank near references.

An auxiliary segment must fit entirely inside the allocatable `2000h-DFFFh` portion of one auxiliary bank.

---

## 36.10 ZEX v2 uses 32-bit file geometry

The complete ZEX file may exceed 64 KiB once auxiliary segments exist.

Therefore:

```text
file offsets
payload offsets
table offsets
total file size
```

must be validated using 32-bit arithmetic.

The ZEX loader consumes the M9 VFS directly through its 32-bit file-position interface.

It must not inherit a 16-bit source-size limitation from the ZMOD loader.

Individual Zephyr-80 segment sizes remain bounded by the target address-space rules even though the container itself may be larger than 64 KiB.

---

## 36.11 Auxiliary-segment relocation

Auxiliary segments are built as relocatable segment images.

M10 initially requires only the relocation form actually needed by generated Z80 code.

The expected first form is equivalent to:

```text
ABS16:
    word_at(segment_base + patch_offset) += segment_base
```

The host packer and target loader both validate:

```text
relocation type
patch bounds
16-bit overflow
target segment
entry-point bounds
payload/table overlap
```

A normal 16-bit relocation may refer only within its own auxiliary segment.

A direct near pointer from one auxiliary segment to another is invalid because the two segments are not guaranteed to be simultaneously mapped.

Cross-segment relationships use:

```text
far-entry IDs
far-memory handles
explicit far-memory operations
```

rather than hidden 16-bit pointers.

The packer rejects unsupported or ambiguous linker relocations rather than guessing.

---

## 36.12 Far-entry table

Executable auxiliary segments export explicit far-call entry points.

A far entry is identified by a stable ZEX-local entry ID and resolves to:

```text
segment id
offset within segment
```

Applications never pass an arbitrary physical address to `SYS_FAR_CALL`.

The loader validates every declared entry point before publishing the process.

This preserves the distinction used elsewhere in ZephyrOS:

```text
container metadata describes intent
kernel validates placement
application consumes a logical identifier
```

---

## 36.13 Far-call execution contract

`SYS_FAR_CALL` is synchronous.

Conceptually:

```text
primary process
    |
    | SYS_FAR_CALL(entry, frame)
    v
kernel validates entry and ownership
    |
    | copy bounded call frame
    v
auxiliary low-window call frame
    |
    | select target auxiliary bank
    | mode 10
    | auxiliary SP in 0100h-1FFFh
    v
far entry in 2000h-DFFFh
    |
    | RET to common return trap
    v
restore previous execution bank/SP
    |
    | copy returned call frame back
    v
SYS_FAR_CALL returns
```

The stable far-entry ABI is not the SDCC C calling convention.

The kernel-level contract passes a bounded in/out call frame to the far entry. First-party C tooling may generate near-call wrappers that marshal an ordinary C function's parameters into that frame and adapt the result back to SDCC conventions.

Those wrappers are compiler/toolchain implementation, not ZephyrOS ABI.

The call-frame maximum is a build-time architectural constant and must be measured against the auxiliary low-window and stack requirements rather than chosen as an unbounded convenience.

---

## 36.14 Far code uses an auxiliary low window

Before executing far code, ZephyrOS prepares the selected auxiliary bank's low window with at least:

```text
native page zero
RST 30h syscall vector
far-runtime scratch
far-call frame
far execution stack
```

The far function executes in mode `10`.

Therefore it may issue an ordinary:

```text
RST 30h
```

and the existing mode-11 transition preserves that auxiliary bank's `0000h-1FFFh` exactly as it preserves the primary process bank's low window.

The syscall gate must distinguish:

```text
process identity
```

from:

```text
currently selected execution bank
```

after M10.

The owning process remains the same while a far function executes.

User-buffer copies for a syscall use the saved current execution bank, not blindly the process's primary bank.

This is an explicit M10 audit requirement for every existing native syscall family.

---

## 36.15 Restrictions while a far call is active

M10 supports one cross-bank far call at a time per process.

Near calls inside the selected auxiliary segment are unrestricted.

Recursive cross-bank `SYS_FAR_CALL` is rejected with `ZOS_EBUSY`.

`SYS_EXEC` from far execution is also rejected. Process creation originates from the primary execution context.

`SYS_EXIT` remains valid and terminates the complete process regardless of whether it is invoked from primary or far execution. Cleanup releases:

```text
primary bank
all auxiliary segments
all runtime xmem objects
VFS descriptors
other process-owned resources
```

and restores the suspended parent normally.

Later milestones may add recursive far calls only if a real consumer requires them.

---

## 36.16 Existing process-startup contract

M10 preserves the process-startup and argument-tail contract already implemented before this milestone.

`SYS_EXEC` already transports the caller's bounded raw tail into the child's startup block v1 at `0080h`; the kernel does not parse shell syntax.

M10 does not introduce:

```text
a second argv/tail representation
far-specific startup semantics
a new shell-oriented process startup structure
```

Far execution remains part of the same process and therefore does not create a new process-startup environment.

The first-party user runtime remains responsible for interpreting the startup block for C programs.

The process-startup ABI is only changed during M10 if the far-memory implementation exposes a concrete architectural incompatibility. Such a change is treated as a correction to the still-unfrozen native ABI, not as a new M10 feature.

---

## 36.17 M10 portability boundary

The following are Zephyr-80 implementation details:

```text
physical bank numbers
8-KiB slots
mode 10 / mode 11
RST 30h
auxiliary low-window geometry
```

The following are native ZephyrOS concepts:

```text
process
primary address space
auxiliary memory object
segment
far entry
opaque memory handle
logical size
```

No public extended-memory ABI should require a future ZephyrOS port to emulate an 8-KiB Z80 bank slot.

---

## 36.18 Memory-budget discipline

M10 inherits the M9 rule that resident bytes are an architectural resource.

Every package reports the existing bank-7 ledger and common-memory ledger.

Neither the production arena floor nor the common-memory ceiling may be weakened to make M10 fit.

If the optimized end-of-M9 image does not leave enough residency for the M10 kernel mechanisms, kernel residency is expanded through a separately reviewed architecture change before the far-memory implementation proceeds. M10 must not hide such a change inside ordinary feature work.

---

## 36.19 M10 non-goals

M10 does not add:

```text
preemptive scheduling
multitasking
shared writable memory between processes
runtime-generated executable memory
dynamic libraries
arbitrary cross-segment near pointers
recursive cross-bank far calls
virtual memory
swap
memory protection
```

The Z80 remains a cooperative, single-running-process machine.

---

# 37. M10 acceptance criteria

M10 acceptance includes an ordinary ZEX v2 program exercising both the unchanged primary-program case and auxiliary facilities.

At minimum:

```text
ordinary ZEX v2:
    zero auxiliary segments
    normal exec/exit
    nested parent/child exec
    existing startup-tail transport remains intact

xmem allocation:
    1 byte
    8191 bytes
    8192 bytes
    8193 bytes
    multiple objects in one auxiliary bank
    48-KiB maximum object
    oversized rejection
    exhaustion
    fragmentation
    stale/foreign handle rejection
    zero-filled allocation
    automatic cleanup on exit

xmem transfers:
    first/last byte
    slot boundary
    complete object
    out-of-range read/write rejection
    caller-buffer bounds
    exact current-bank restoration

ZEX auxiliary segments:
    one segment
    multiple segments
    image + BSS
    valid ABS16 relocations
    malformed relocation
    relocation overflow
    invalid entry
    segment allocation rollback
    container larger than 64 KiB

far calls:
    primary -> far -> primary
    in/out call frame
    far private BSS
    normal near calls inside far segment
    ordinary syscall from far code
    filesystem syscall from far code
    exact primary SP/bank/IFF restoration
    recursive far call -> EBUSY
    SYS_EXEC from far context rejected
    SYS_EXIT from far context cleans the whole process

lifetime:
    suspended parent retains far allocations
    child receives no parent far handles
    repeated exec/exit leaks no slots
    repeated far calls leak no state

budget:
    production residency constraints remain satisfied
    common linked content remains below its architectural ceiling
    bank7 and common ledgers are emitted
```

Physical hardware is authoritative for bank switching, interrupt interaction and far-call return behavior.

---

# 38. Milestone M11 — Native diagnostic suite

## 38.1 Objective

M11 builds the complete native ZephyrOS diagnostic environment on top of the now-complete kernel mechanisms.

M11 is primarily userland.

It does not move diagnostic implementations into the kernel merely because they inspect hardware.

The existing M7 monitor remains the minimal recovery tool; M11 adds a broader diagnostic suite beside it.

---

## 38.2 Diagnostic layering

The diagnostic suite follows this order of preference:

```text
1. ordinary native service
2. public device/filesystem behavior
3. M7 diagnostic syscall
4. narrowly added diagnostic introspection service
```

A diagnostic must not link private kernel or driver symbols.

Direct memory and I/O operations continue to cross the explicit M7 diagnostic boundary.

The suite does not acquire arbitrary bank-7 read/write access.

---

## 38.3 Diagnostic program organization

The primary diagnostic program is:

```text
/SYSTEM/diag.zex
```

`diag.zex` is an ordinary ZEX v2 executable.

It may use M10 auxiliary code segments to keep the primary image small and to make the diagnostic suite the first substantial production consumer of far-code support.

Conceptually:

```text
primary segment:
    command/menu
    common formatting
    result aggregation

far segments:
    memory/banking
    IRQ/CTC
    serial
    video
    HID
    IOC
    storage
    sound
```

The exact segmentation is a build decision, not a user ABI.

---

## 38.4 Diagnostic command families

The complete suite covers at least:

```text
diag system
diag memory
diag irq
diag serial
diag video
diag hid
diag ioc
diag storage
diag sound
diag all
```

### `system`

Reports and validates:

```text
kernel/native ABI identity
process identity
memory geometry
free primary banks
auxiliary-memory state
loaded modules
registered devices
mounted filesystems
console bindings
```

### `memory`

Tests:

```text
xmem allocation/free
pattern write/readback
slot-boundary transfers
bank restoration
allocator exhaustion/cleanup
```

Destructive arbitrary-bank testing is not the default.

### `irq`

Inspects or tests:

```text
IRQ executive state
source ownership
CTC delivery
unexpected-source accounting
interrupt enable/restore behavior
```

### `serial`

Exercises the registered serial device where practical without replacing the permanent panic/early-console path.

### `video`

Provides:

```text
console test
character/font test
VRAM/data-path test where exposed safely
visual test patterns
```

### `hid`

Provides an interactive keyboard/input test and reports translated events through the normal HID/console path.

### `ioc`

Exercises:

```text
link status
command/reply transactions
bulk read/write path
error/recovery counters where exposed
```

without bypassing the active IOC driver and corrupting its transport state.

### `storage`

Exercises the native filesystem and, where appropriate, block layer:

```text
sequential read/write
seek
metadata
cache behavior
throughput
confirmed write
media/reset behavior
```

A destructive raw-media test requires an explicit destructive mode and never runs under `diag all`.

### `sound`

Exercises the installed sound hardware/driver when present.

Where no native sound service exists yet, a board-level diagnostic may use the explicit diagnostic-I/O boundary rather than turning sound testing into a new kernel subsystem.

---

## 38.5 Read-only diagnostic introspection

M11 may add a read-only diagnostic enumeration service after the existing M7 diagnostic family when concrete tests need information that normal APIs cannot expose safely.

Conceptually:

```text
SYS_DIAG_ENUM

kind
index
output record
output size
```

with versioned record kinds such as:

```text
memory
irq source
device
module
mount
```

The service copies stable descriptive values.

It does not expose:

```text
kernel pointers
driver state pointers
mutable registry objects
bank-7 addresses usable as capabilities
```

End-of-list is distinct from failure.

No generic "call arbitrary driver operation" diagnostic syscall is introduced.

If a diagnostic cannot be expressed safely through normal services, existing diagnostic primitives or this read-only introspection model, that requirement returns to architecture review.

---

## 38.6 Non-destructive by default

Every M11 diagnostic is non-destructive by default.

Potentially destructive operations require an explicit operator choice identifying the resource to modify.

`diag all` must be safe to run on a normal booted system.

Device absence is:

```text
SKIP
```

not failure.

Unsupported optional functionality is also `SKIP`.

A real test failure is `FAIL`.

---

## 38.7 Stable result model

Every diagnostic command produces a final status:

```text
PASS
FAIL
SKIP
```

and returns a process exit status consistent with that result.

`diag all` aggregates the individual tests and exits successfully only when no required test failed.

Human-readable detail may precede the summary, but automated acceptance must be able to identify the final result without screen scraping arbitrary prose.

---

## 38.8 Monitor relationship

`monitor.zex` remains small, independent and recovery-oriented.

M11 does not absorb the full diagnostic suite into the monitor.

The monitor remains usable if:

```text
diag.zex is missing
an auxiliary allocation fails
a diagnostic far segment is malformed
external storage is absent
```

Before M12 supplies the normal shell, acceptance images may launch `diag.zex` directly.

A minimal monitor command that executes a named native program through the ordinary `SYS_EXEC` path may be added for operator convenience, but it creates no new privileged execution path.

---

## 38.9 M11 non-goals

M11 does not add:

```text
a second driver stack
private diagnostic driver entry points
kernel-resident test suites
automatic destructive media testing
a shell
CP/M diagnostics
personality-specific diagnostics
```

It diagnoses native ZephyrOS and the physical Zephyr-80 platform.

---

# 39. M11 acceptance criteria

Acceptance includes:

```text
system:
    coherent version/process/memory report
    device enumeration
    module enumeration
    mount enumeration

memory:
    xmem pattern test
    allocation exhaustion and recovery
    no resource leakage

irq/ctc:
    known interrupt delivery
    counters/state
    exact return state

serial:
    registered path
    bounded failure behavior

video:
    console output
    operator-visible test pattern

hid:
    interactive key/input validation

ioc:
    command exchange
    bulk path
    recovery/error handling

storage:
    filesystem read/write/seek
    cache path
    throughput reporting
    safe failure with no media

sound:
    positive hardware test when installed
    SKIP when absent

aggregate:
    diag all never performs destructive testing
    PASS/FAIL/SKIP aggregation
    meaningful exit status

architecture:
    no private kernel/driver links in diag.zex
    diagnostic ABI exposes no kernel pointers
    monitor still works independently
    ZEX far segments used successfully where configured

budget:
    production residency ledgers pass
```

Hardware-observable device tests require physical acceptance.

---

# 40. Milestone M12 — Native shell and core userland

## 40.1 Objective

M12 provides the normal interactive native ZephyrOS environment.

At the beginning of M12 the kernel, memory system, process model, filesystem, device stack, diagnostics and executable model are already complete.

M12 is therefore a userland milestone.

The shell must not require a new kernel mechanism merely to become usable.

If a fundamental kernel facility is found missing, the omission is treated as an earlier architecture defect rather than hidden inside shell-specific kernel code.

---

## 40.2 Shell is an ordinary process

The native shell is:

```text
/SYSTEM/shell.zex
```

and is an ordinary ZEX v2 application.

It has no private kernel imports.

It uses only:

```text
native console ABI
native filesystem ABI
SYS_EXEC / process ABI
existing startup-tail contract
```

The shell becomes the normal parent process for launched native programs.

The existing lifecycle remains:

```text
shell running
    |
    | SYS_EXEC
    v
shell suspended
    |
    v
child running
    |
    | SYS_EXIT
    v
shell resumed with child status
```

No scheduler is required.

---

## 40.3 Boot policy

After normal boot:

```text
IRQ/device/VFS initialization
    ->
console binding
    ->
launch /SYSTEM/shell.zex
```

`/SYSTEM/shell.zex` is part of the recovery filesystem so the native environment remains available without writable SD media.

If the shell cannot be loaded or launched, ZephyrOS falls back to:

```text
/SYSTEM/monitor.zex
```

through the established recovery path.

A normal shell exit may be relaunched by the kernel supervisor.

An abnormal shell failure is reported and may enter the monitor rather than endlessly relaunching a failing shell.

The exact exit-status policy is pinned during implementation and tested.

**Reconciliation with the M9 launcher (stated M12 change).** M9's
`ZOS_BOOT_PROGRAM` (`config/system.mk`, compiled into the kernel as a
constant string) already selects the single program the boot launcher starts
and, on a normal exit, re-executes; switching native ZephyrOS to the shell is
that one setting plus a `shell.zex` entry in the selected recovery manifest.
What is **new in M12, not M9**, is the failure policy above: the M9 launcher
reports and re-executes its configured program, whereas M12 adds the
`/SYSTEM/monitor.zex` fallback for a shell that cannot be loaded or launched
and stops an abnormally failing shell from being relaunched indefinitely.
That fallback is implemented and tested in M12; M9 leaves the launcher
unchanged.

---

## 40.4 Command parsing belongs entirely to userland

The shell receives the existing raw startup tail and owns command-line parsing.

The kernel never parses shell syntax.

M12 supports at least:

```text
whitespace-separated words
single-quoted strings
double-quoted strings
backslash escaping
```

For an external command, the shell serializes the resulting arguments into the existing bounded `SYS_EXEC` raw-tail contract used by startup block v1.

M12 does not require:

```text
pipes
I/O redirection
background jobs
job control
shell scripting
variable expansion
command substitution
filename glob expansion
```

Those can be added later without changing the kernel process model.

---

## 40.5 Shell built-ins

The native shell deliberately contains the core userland commands as built-ins.

At minimum:

```text
cd
pwd
ls
cp
mv
rm
mkdir
rmdir
cat
stat
echo
path
help
exit
```

These commands are part of the shell executable rather than separate ZEX programs.

This has several advantages on Zephyr-80:

```text
no repeated exec/exit overhead for simple filesystem operations
no duplicated runtime/startup code across tiny utilities
smaller recovery-store footprint
shared parsing and formatting code
shared filesystem buffers
simpler recovery environment
```

Commands such as `cd`, `pwd`, `path` and `exit` necessarily operate on shell-owned state and therefore must be built-ins.

For architectural simplicity and compactness, M12 applies the same model to the remaining core commands.

This packaging choice is deliberate but does not make the commands kernel services. They remain ordinary userland logic inside `shell.zex`.

---

## 40.6 External command lookup

Commands not recognized as shell built-ins are treated as executable program names.

An argument containing `/` is treated as an explicit path.

A bare command name is resolved entirely by shell policy through a bounded shell-local command path.

A suitable initial default is:

```text
.
/BIN
/SYSTEM
```

Missing path components are skipped.

The shell may try the exact name and then the conventional `.zex` suffix.

The kernel has no `PATH` concept and performs no command search.

Changing the shell command path changes only shell state.

---

## 40.7 Core native userland

The core native userland is the shell itself.

The following facilities are built into `shell.zex`:

```text
filesystem navigation
    cd
    pwd

directory/file inspection
    ls
    stat
    cat

filesystem mutation
    cp
    mv
    rm
    mkdir
    rmdir

general shell utilities
    echo
    path
    help
    exit
```

Larger or specialized applications remain ordinary external ZEX programs.

Examples include:

```text
diag.zex
monitor.zex
editors
compilers
language runtimes
games
future personality launchers
```

Thus the distinction is:

```text
small universal operating commands     -> shell built-ins
substantial applications/tools          -> external ZEX programs
```

The exact internal organization of built-ins may use separate source modules, but they link into one `shell.zex`.

---

## 40.8 Userland filesystem policy

Shell built-ins consume only the M9 filesystem ABI.

### `cp`

Uses ordinary:

```text
open
read
write
close/sync
```

with a bounded shell-owned buffer.

### `mv`

First attempts native `rename`.

If the VFS reports:

```text
ZOS_EXDEV
```

for a cross-filesystem move, the shell may implement:

```text
copy
sync/close
unlink source
```

with appropriate failure handling.

The kernel does not acquire a hidden cross-filesystem rename implementation for the benefit of the shell.

### `ls`

Uses:

```text
opendir
readdir
closedir
stat where required
```

and understands native mountpoints exactly as exposed by VFS.

---

## 40.9 Recovery operation without writable storage

With the normal writable root unavailable, the user must still receive a useful system:

```text
/
`-- SYSTEM/
    |-- shell.zex
    |-- monitor.zex
    |-- diag.zex
    `-- essential recovery assets
```

The shell starts normally.

Read-only commands continue to work.

Mutation attempts against `/SYSTEM` report the normal `ZOS_EROFS` behavior.

The shell does not treat absence of the writable root as a fatal condition.

---

## 40.10 Exit-status handling

`SYS_EXEC` returns the child's native exit status to the shell.

The shell stores the most recent status and reports nonzero failures in a consistent form.

Built-ins and external native programs return meaningful native status values rather than always succeeding after printing an error.

No POSIX-specific status encoding is implied.

---

## 40.11 Shell memory rules

The shell is not special with respect to memory.

It may use:

```text
primary memory
ordinary xmem
far code
```

through exactly the M10 interfaces available to every other native ZEX.

Because the shell contains the complete core userland command set, M10 far-code support may be used to partition built-in command families into auxiliary executable segments if the primary image would otherwise become unnecessarily large.

That is an implementation choice, not part of shell command semantics.

---

## 40.12 M12 non-goals

M12 does not require:

```text
multitasking
preemption
pipes
redirection
background jobs
shell scripts
environment variables
dynamic linking
networking
CP/M execution
other compatibility personalities
```

Those are later facilities or personality work.

---

# 41. M12 acceptance criteria

Acceptance covers at least:

```text
boot:
    shell launched from /SYSTEM
    HID/V9958 normal console
    SIO fallback
    shell available with no writable root
    monitor fallback when shell is missing/rejected

parser:
    empty line
    multiple arguments
    single quotes
    double quotes
    escaped characters
    tail-limit rejection/recovery

built-ins:
    cd
    pwd
    ls
    cp
    mv
    rm
    mkdir
    rmdir
    cat
    stat
    echo
    path
    help
    exit

execution:
    bare-name command search
    explicit absolute path
    explicit relative path
    .zex suffix handling
    child exit 0
    child nonzero exit
    malformed executable
    repeated child execution leaks no process/bank/xmem/fd state

CWD:
    cd absolute
    cd relative
    ..
    child inherits shell CWD
    child chdir does not alter shell CWD

filesystem policy:
    mv same filesystem
    mv cross-filesystem through copy/sync/unlink
    writes to /SYSTEM fail cleanly with EROFS

native tools:
    launch monitor
    launch diag
    return cleanly to shell

recovery:
    /SYSTEM visible with SD absent
    read-only built-ins work
    mutation failure is clean and non-fatal

budget:
    final production residency ledgers pass
    recovery ROMFS fits
```

Physical acceptance includes a complete interactive session on the HID/V9958 console and the SIO fallback console.

---

# 42. Native ZephyrOS completion boundary

Completion of M12 marks the first complete native ZephyrOS environment.

At that point the system has:

```text
boot/recovery
memory ownership
interrupt executive
dynamic drivers
native device model
native console
process lifecycle
native executable format
hierarchical VFS
writable filesystem
extended/far memory
far code/data
complete diagnostics
native shell
core built-in userland
```

Only after this boundary does detailed compatibility/personality architecture begin.

Personality implementations consume native ZephyrOS mechanisms rather than forcing compatibility concepts into the native kernel.

Initial Zephyr-80 personality candidates include:

```text
CP/M
Coleco
GameOS
```

Future ZephyrOS ports may define personalities appropriate to their processor/platform family.

The native kernel itself remains free of:

```text
CP/M FCB semantics
Coleco BIOS semantics
foreign filesystem namespaces
foreign process ABIs
```

except at explicit personality boundaries.

---

## 42.1 Native ABI baseline

During M0-M12 ZephyrOS deliberately carries no backward-compatibility obligation between development milestones.

Completion of M12 ends that phase.

The accepted:

```text
native syscall ABI
ZEX v2 format
driver/module ABI
filesystem ABI
process-startup ABI
extended-memory ABI
```

form the first stable native ZephyrOS compatibility baseline.

Subsequent incompatible changes require an explicit ABI or format version change rather than silently changing an established contract.

---
