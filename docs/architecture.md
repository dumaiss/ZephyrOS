# ZephyrOS Architecture

**Status:** M0/M1/M2/M3 complete; M4 complete (stage 07 loader, stage 08 dynamic-driver acceptance — acceptance, placement, and failure images hardware-validated 2026-10-02)  
**Scope:** Milestones M0 through M4  
**Target:** Zephyr-80  
**Language/toolchain:** SDCC + sdasz80/sdldz80, GNU Make, Python host tools

---

## 1. Purpose

ZephyrOS is the native operating system for the Zephyr-80.

The initial milestones intentionally do **not** attempt to implement a complete operating system. Their purpose is to establish a clean boot path, a stable kernel execution environment, and the architectural boundaries on which later ZephyrOS subsystems can be built.

The milestones covered by this document are:

- **M0 — Kernel bring-up:** boot ZephyrOS, initialize the early serial console, print a banner, and halt. **Complete.**
- **M1 — Kernel foundation:** establish the kernel memory model, page-zero ownership, common kernel entry points, and the initial native ABI scaffolding. **Complete.**
- **M2 — Interrupt executive:** establish kernel-owned IM2, safe mode-crossing IRQ entry/return, source ownership, and the first hardware interrupt.
- **M3 — Device registry and static drivers:** establish the driver/device model and prove it with statically linked hardware drivers.
- **M4 — Dynamic driver/module loading:** load relocatable driver modules into a managed bank-7 driver arena and register their devices at boot.

Filesystem, process execution, native userland, ZEX programs, and CP/M compatibility remain later milestones.

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

### 2.2 Hardware details are platform-specific

Kernel code should not scatter Zephyr-80 I/O port values or board-specific constants throughout the source tree.

Hardware definitions and low-level primitives belong under:

```text
platform/zephyr80/
```

is **declarative only**. It describes what the Zephyr-80 machine is:

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
```

Those implementations consume constants from `platform/zephyr80/`.

The rule is:

> **Platform describes the machine; boot, kernel, and drivers operate the machine.**

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

M0 and M1 do not reserve kernel architecture around CP/M concepts such as BDOS, FCBs, 128-byte records, or `CALL 0005h`.

---

# 3. Source tree through M4

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
│       └── module.h
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
│   │   └── entry.asm
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
│   └── module/
│       ├── loader.c
│       ├── relocate.c
│       ├── arena.asm
│       └── embedded.c
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
├── tests/
│   ├── modules/
│   │   └── sio_irq_module.c, sio_fail_module.c, *.json
│   └── target/
│       ├── m1/ .. m4/
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
    └── zmodpack.py
```

Directories for filesystems, processes, CP/M compatibility, libc, and userland remain deferred.

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

### `linker/`

Owns link placement and section layout.

The linker may consume values derived from the platform description, but linker scripts are not executable target code.

### `tools/`

Owns host-side build, validation, packing, and inspection tools.

These rules apply equally to M0-M4 and to later milestones.

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
- **Python** — host-side image, map, validation, and later executable/module tooling

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
| `D3` | `RAM_SHADOW`; with ROM disabled, selects OS mode |
| `D4` | `ROM_DIS`; `1` selects RAM-only runtime operation |
| `D5-D7` | selected ROM page, `0-7` |

For ROM page `p`, SRAM bank `b`, and mode value `m`:

```text
latch = ((p & 7) << 5) | m | (b & 7)
```

The four hardware modes are:

```text
D4 D3   value   meaning
 0  0    00h    boot / ROM-visible
 0  1    08h    shadow / ROM-to-RAM copy
 1  0    10h    application execution
 1  1    18h    OS execution
```

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
  │
  │ latch = 00h
  ▼
ROM page 0 bootstrap
  │
  ├── DI
  ├── copy the high ROM/bootstrap/common region into hidden SRAM bank 0
  ├── enter shadow/copy mode
  ├── install the bank-0 runtime/common image
  ├── copy the ZephyrOS bank-7 image from ROM page 7 into SRAM bank 7
  ├── enter RAM-only mode 10 over bank 0
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

Shadow/copy mode exposes ROM at `0000h-BFFFh` while writes target the selected SRAM bank. Consequently, the **loadable bank-7 kernel image must fit below `C000h`**.

For M0/M1:

```text
bank 7:
    2000h-BFFFh   loadable kernel image
    C000h-DFFFh   zero-initialized runtime area / stacks / scratch

bank 0:
    E000h-FFFFh   permanent common code/state
```

The exact amount of `C000h-DFFFh` used at M0/M1 is linker-defined, but it must not contain ROM-initialized data that depends on the shadow-copy operation.

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
D3             RAM_SHADOW / OS-mode select
D4             ROM_DIS
D5-D7          ROM page

00h            boot / ROM-visible
08h            shadow / copy
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
INPUT
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
ZDEV_INPUT
ZDEV_TRANSPORT
ZDEV_OTHER
```

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

Conceptually:

```c
typedef uint8_t zdev_handle_t;

struct zdevice {
    zdev_handle_t handle;
    uint8_t       class_id;
    uint16_t      capabilities;
    const char   *name;
    const void   *ops;
    void         *state;
    const void   *driver;
};
```

The exact C layout is internal and may evolve.

Required registry operations are semantically equivalent to:

```c
void device_registry_init(void);

int device_register(const struct zdevice_desc *desc,
                    zdev_handle_t *out_handle);

const struct zdevice *device_get(zdev_handle_t handle);

zdev_handle_t device_find_capability(
    uint16_t required_caps,
    zdev_handle_t after
);
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
> `driver_init_static()`) is **retired**.  Since M4 the kernel boots only
> embedded ZMOD drivers; production images link no static driver table.
> These sections are retained as M3 milestone history and must not be read
> as the current production tree.  The `zdriver_api` contract itself is
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

*(M3 history: since M4 stage 08 this driver is loaded as a ZMOD module\ninstead of being linked; see sections 22.9 and 22.13.  The registry,\ncapability, and console contracts below are unchanged.)*

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

Implemented geometry (stage `07-m4-module-loader`): the initial arena is
`A000h-BFFFh` (8192 bytes, 16 granules) between the loadable kernel image
and the `C000h-DFFFh` runtime area, declared in
`platform/zephyr80/memory.inc`, exported by `kernel/module/arena.asm`, and
enforced by `tools/mapcheck.py` / `tools/pack_image.py` (no kernel
section, stack, or ROM payload may occupy it).  The build prints arena
geometry plus free granules, and `zephyros.manifest.json` carries an
`arena` object.  Concrete loader behavior, error codes, and tooling are
documented in `docs/module-format.md`.

---

## 22.3 Module format

M4 introduces **Zephyr Module Format v1 (ZMOD)**.

ZMOD is a generic relocatable container. M4 supports only module type `DRIVER`.

Later executable milestones may extend the same container rather than invent an unrelated relocation format.

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

Implemented header (stage 07): a packed little-endian 28-byte structure
with `payload_offset`, `total_size`, and a `reserved` word, pinned by
`include/zephyros/module.h` compile-time assertions.  `docs/module-format.md`
records the byte table and the manifest/packer contract.

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

M4 occurs before the native filesystem milestone.

Therefore the loader source abstraction must not depend on a filesystem.

The first implementation uses an **embedded module catalog** generated at build time.

For example:

```text
embedded_modules[]
    "sio" -> byte blob / size
```

The same loader operates on those bytes exactly as it later will on module bytes obtained from a filesystem.

M5 may add a filesystem-backed module source without changing relocation, allocation, driver initialization, or device registration.

The acceptance test must not special-case the SIO driver's load address.

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

- consume the supported SDCC/assembler build outputs;
- generate the relocation table;
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
                                transactional module_load, catalog load,
                                module_code_allowed (IRQ validation hook)
kernel/module/embedded.c        production boot-module catalog
                                (tests/target/m4/ holds the fixture catalog)
tests/target/m4/m4_test.c       loader/dynamic-driver acceptance test
tools/zmodpack.py               pack/validate/dump/emit-asm
```

M2's registry gained claim serials, owner tokens, and
checkpoint/rollback (`irq_registry_checkpoint/rollback/set_owner`) so a
failed module init rolls back its IRQ claims exactly like its device
registrations.  `irq_claim` now accepts handler addresses that are linked
bank-7 code or lie inside the active/committed module images; see
`docs/module-format.md` section 4.1.

Stage 08 (`08-m4-dynamic-driver-acceptance`) then removed the static SIO
driver from the kernel entirely; stage 10 separated production drivers,
module packaging/runtime support, and acceptance code:

```text
drivers/sio/sio.c               SIO implementation (packed into the module)
drivers/sio/sio_io.asm          SIO port I/O
drivers/sio/sio_module.c        production module entry (descriptor + init)
modules/manifests/sio.json      production module manifest
modules/runtime/sdcc_indirect_calls.asm  shared module-local SDCC glue
tests/modules/sio_irq_module.c  IRQ self-test variant entry
tests/modules/sio_fail_module.c init-failure variant entry
```

* the M3 static driver table is retired: no `boot_drivers[]`,
  `driver_init_static()`, or `_driver_static_*` symbol is linked, and
  `tools/mapcheck.py --production` rejects any acceptance symbol;
* every image embeds a boot module through the generated catalog:
  `sio.zmod` (production, loader image, placement image),
  `sio-irq.zmod` (`make m4-selftest`), or `sio-fail.zmod`
  (`make m4-failure-selftest`);
* the placement image regenerates `platform/zephyr80/memory.inc` with the
  arena shifted to 9E00h-BE00h and embeds byte-identical module files;
* `kernel_main()` prints `console: dynamic sio0.b OK` through the bound
  module (or `console: dynamic driver failed` on load failure);
* `tools/mapcheck.py` rejects any linked `_sio_`/`_sio0b_`/`_sio_driver`
  symbol, so the static driver cannot return unnoticed.

See `docs/module-format.md` section 6 for the pipeline and
`docs/devices.md` for the resulting device-layer behavior.

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

Evidence mapping: `tests/test_m4.py`/`tests/test_m4_sim.py` and
`tests/test_zmod.py` prove every criterion on host (including the shifted
placement and the failure rollback); `make m4-selftest`,
`make m4-placement-selftest`, `make m4-failure-selftest` and the
corresponding `smoke-hardware-*` captures provide the physical-hardware
record (`docs/validation/2026-10-02-m4-acceptance.md`).

---

# 24. Deferred architecture

The following remain intentionally deferred until after M4:

- native filesystem API and IOC filesystem integration
- native process descriptors
- `exec` / `exit`
- native shell and userland
- native application libc/runtime bindings
- `.ZEX` program executable semantics
- extended/far-memory objects and far-code services
- CP/M BDOS compatibility
- `.COM` loader and CP/M personality
- driver unload/hot-plug
- preemptive multitasking or scheduling

These systems should build on the M0-M4 kernel, IRQ, device, and module foundations rather than being incorporated prematurely.

---

# 25. Milestone summary

```text
M0  COMPLETE
│
├── enter kernel
├── establish stack
├── initialize early serial
├── call C successfully
├── print banner
└── halt
    │
    ▼
M1  COMPLETE
│
├── establish mode 10/11 kernel memory model
├── formalize platform banking boundary
├── own native page zero
├── reserve syscall RST
├── create common kernel gate
├── define ABI scaffold
├── generate/validate link map
└── retain early panic console
    │
    ▼
M2
│
├── kernel-owned IM2
├── common IRQ crossing gate
├── common transition + bank-7 ISR stacks
├── source ownership registry
├── safe FFh vector
└── CTC0 hardware interrupt
    │
    ▼
M3
│
├── device registry
├── capability model
├── static driver table
├── static SIO0/B driver
└── normal console service over registered device
    │
    ▼
M4
│
├── bank-7 driver arena
├── 512-byte granule allocator
├── ZMOD v1 driver format
├── relocation + BSS
├── embedded module catalog
└── dynamically loaded SIO driver
```

At the end of M4, ZephyrOS has a deterministic kernel, safe interrupt crossing, a device model, and relocatable boot-loadable drivers. It still does not require a filesystem or user process in order to prove those foundations.

---

# 26. Later milestone roadmap

The following milestone names define the intended sequence after M4. Their detailed architecture is intentionally deferred until each milestone is approached.

```text
M5  Native filesystem and IOC integration
M6  Process lifecycle and native exec/exit
M7  Native program executable format and loader
M8  Native shell and core userland
M9  CP/M compatibility personality
M10 COM executable support
M11 Extended memory / far-code / far-data services
M12 System hardening, diagnostics, and release integration
```

These milestone names are roadmap markers only and are not architectural commitments beyond their broad sequencing.

