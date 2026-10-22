# KonaOS Architecture

**Target:** Zephyr-80 (Z80 at 10 MHz, banked SRAM, decoder revision 13)
**Toolchain:** SDCC 4.5 (`sdcc`, `sdasz80`, `sdldz80`), GNU Make, Python 3

This document is the architectural contract of KonaOS: what the system is,
the rules every part of it follows, and how the parts fit together. The
detailed references are:

| document | subject |
|---|---|
| `docs/memory.md` | memory map, residency, ledgers |
| `docs/boot.md` | reset, the boot image, the boot sequence |
| `docs/abi.md` | the syscall ABI, every argument block and error code |
| `docs/devices.md` | resident drivers, protocols, the driver bootstrap API |
| `docs/vfs.md` | the namespace, the three providers, handles |
| `docs/filesystem.md` | files and the filesystem driver |
| `docs/exe-format.md` | the EXE executable container |
| `docs/module-format.md` | the MOD driver image |
| `docs/romfs-format.md` | the ROM store |
| `docs/sdk.md` | building programs, the standalone SDK |
| `docs/monitor.md` | the native monitor |

Where this document and a reference disagree, this document wins and the
reference is wrong.

---

## 1. Purpose and principles

### 1.1 What KonaOS is

KonaOS is the native operating system of the Zephyr-80. It is a
**trusted, single-tasking** system: one process runs at a time, a parent
waits while its child runs, and there is no protection between programs,
drivers and the operating system.

Compatibility environments such as CP/M are later **personalities** built
on top of the native system. They do not shape the native kernel.

### 1.2 Principles

1. **KonaOS owns the machine.** Once control enters KonaOS, the nucleus owns
   the CPU state, the banking latch, page zero, stack placement, the
   interrupt mode, interrupt ownership and the entry points into resident
   code. Programs and drivers use defined interfaces, never kernel layout.
2. **If the host can precompute it, precompute it.** Driver placement,
   endpoint numbers, dependencies, device publications, executable
   geometry and store layout are computed and validated when the image is
   built. The target only copies, checks the bounds that protect the
   nucleus, and runs.
3. **Build no machinery the machine does not need.** The machine is
   trusted and single-tasking, so there is no isolation, discovery or
   concurrency machinery. Endpoints and IPC exist for modularity and for
   crossing banks, not for protection.
4. **The operating system uses at most two banks plus common memory**
   (bank 7 for the nucleus, bank 6 for resident drivers). When something
   does not fit, it is slimmed or moved to a lower tier; a third bank is
   never added.
5. **A program that uses no far facilities pays nothing.** It keeps its
   whole 56 KiB primary bank and never sees a far mechanism.
6. **Platform describes the machine; boot, kernel, drivers and programs
   operate it.** Machine constants live in `platform/zephyr80/` and
   nowhere else.
7. **The native ABI is separate from the compiler ABI.** Every stable
   interface is defined by KonaOS and reached through explicit glue, never
   through SDCC's calling convention.
8. **The early serial console is permanent and special.** It is the
   diagnostic path for boot, panics and failures of the normal console,
   and it works before and without any driver.
9. **A replaced mechanism is deleted.** No interface, format or image
   carries a backward-compatibility obligation until the native ABI is
   frozen.

### 1.3 Source tree and ownership

```text
boot/                 reset, ROM-to-RAM copy, handoff to the kernel
kernel/               the nucleus
  include/            nucleus-private headers
  driver/             the resident-driver loader and boot gate
  ipc/                endpoints, frames, descriptors, copy paths
  irq/                the interrupt executive and its platform quiesce
  lib/                small shared helpers
  memory/             banking, window and gate copies, upcall shims
  process/            the process table, exec, launch
  romfs/              the ROM store reader and the ROM-access stub
  runtime/            compiler helper routines
  syscall/            the syscall gate, dispatcher and bodies
  vfs/                the namespace router, the providers, handles
drivers/              resident drivers, one directory each
  include/            the driver kit (bootstrap API, IOC vocabulary)
  runtime/            the shared driver runtime object
formats/include/      on-media formats: ROMFS, EXE, MOD
abi/kos/              the public native ABI (one copy of every header)
platform/zephyr80/    machine description: public/ (drivers may use),
                      private/ (nucleus only), config.mk
systems/              one system file per image
config/               residency and SDK configuration
linker/               the link script
mk/kos.mk             the program build rules (shared with the SDK)
user/
  sdk/                the user SDK: headers, libkos, template, examples
  monitor/            the native monitor
  helloworld/         the demo image's build
tools/                host tools: packers, validators, smoke capture
tests/                host tests, simulator replays, target test code
```

Ownership rules:

- `platform/zephyr80/` holds **constants and configuration only**: port
  numbers, register masks, memory regions, mode values, IRQ wiring, build
  variables. It contains no `.c` or `.asm` file and no instruction-emitting
  macro. A file that emits bytes that run on the Z80 does not belong there.
- `boot/` owns the code that runs before the kernel environment exists.
- `kernel/` owns operating-system mechanisms. A mechanism stays kernel code
  even when it is Zephyr-specific: the fact that the latch is port `00h` is
  platform data; the routine that changes it safely is kernel code.
- `drivers/` owns active device behaviour: register programming,
  initialization, polling, interrupt handling after dispatch, protocol
  operations and driver-private state.
- `abi/kos/` owns the public native ABI. The kernel, the drivers and the
  SDK all compile against the same copy.
- `user/` owns native programs. They use the public ABI through the SDK
  and never include a kernel or driver header.
- Each audience has its own include roots:

  | audience | include roots |
  |---|---|
  | nucleus | `abi`, `kernel/include`, `drivers/include`, `formats/include`, `platform/zephyr80/public`, `platform/zephyr80/private` |
  | drivers | `abi`, `drivers/include`, `platform/zephyr80/public`, the driver's own directory |
  | programs | `abi` and the SDK include root only |

  A driver cannot include a nucleus header.

### 1.4 Toolchain

The nucleus, the drivers and the programs are built with SDCC for the Z80
and linked with `sdldz80`. Everything is freestanding: no hosted C runtime
and no standard I/O. The nucleus and the drivers are compiled for size.
Host tools are Python 3 standard library only.

---

## 2. Hardware and memory model

### 2.1 The banking latch

The memory latch is I/O port `00h` (the decoder answers `00h-0Fh`;
software uses `00h`). Only the nucleus's banking code writes it, except
that a program opens and closes the far aperture through the SDK
(section 7).

```text
D0-D2   Q   SRAM bank
D3-D4       memory mode
D5-D7   P   page: the ROM page in mode 00, the window/aperture page otherwise
```

`latch = (P << 5) | mode | Q`. Reading port `00h` returns the latch.

### 2.2 Modes (decoder revision 13)

| mode | value | `0000h-1FFFh` | `2000h-5FFFh` | `6000h-7FFFh` | `8000h-BFFFh` | `C000h-DFFFh` | `E000h-FFFFh` |
|---|---|---|---|---|---|---|---|
| 00 ROM | `00h` | reads ROM page P; writes SRAM bank Q | ← | ← | ← | ← | ← |
| 01 flat | `08h` | bank Q | bank Q | bank Q | bank Q | bank Q | bank Q |
| 10 application, P = 0 | `10h` | bank Q | bank Q | bank Q | bank Q | bank Q | bank 0 (common) |
| 10 application, P ≠ 0 | `10h` | bank Q | bank Q | bank Q | **bank 7 − P (aperture)** | bank Q | bank 0 (common) |
| 11 OS | `18h` | bank Q (caller window) | bank 7 (fixed core) | **bank 7 − P (window)** | **bank 7 − P** | **bank 7 − P** | bank 0 (common) |

- **P = 0 behaves exactly like the previous decoder revision** in every
  mode, so CP/M and any image that never sets P run unchanged.
- Mode 11's **window** `6000h-DFFFh` shows bank 7 when P = 0 and bank 6
  when P = 1. The operating system uses no other window page.
- Mode 10's **far aperture** `8000h-BFFFh` opens only when P ≠ 0.
- Mode 00 serves **every** read from ROM, including instruction fetch, while
  writes go to SRAM. A stack cannot survive it: code that runs in mode 00
  is stackless and byte-identical in ROM and RAM (sections 3.1 and 4.7).
- Mode 01 has no common region. It is reserved for personalities; KonaOS
  never assumes common memory is mapped while one runs.
- The decoder only selects which bank answers an 8 KiB region, at the same
  offset. There is no address translation. The decoder logic and its test
  vectors belong to the Zephyr-80 hardware project; the OS keeps a
  simulator model (`tests/z80sim.py`).

### 2.3 Bank roles

```text
bank 7      the nucleus
bank 6      the resident driver domain
banks 0-5   application banks and extended memory
bank 0      also provides common memory at E000h-FFFFh
```

### 2.4 Common memory

`E000h-FFFFh` of bank 0 is visible in modes 10 and 11. It holds what must
stay reachable while the mapping changes:

| range | contents |
|---|---|
| `E000h-FCFFh` | transition code, the syscall and IRQ gates, the copy stubs, the ROM-access stub (at `EF00h`), common state, the 384-byte gate stack |
| `FD00h-FE00h` | the IM2 vector page and its `FFh`-vector guard byte |
| `FE01h-FE08h` | IRQ transition state (saved latch, saved SP, common SP, active source) |
| `FE08h-FE88h` | the 128-byte IRQ transition stack, directly above the state cells |
| `FE88h-FFFFh` | the boot and common stack |

Common memory is about **reachability, not isolation**: mode 10 maps it as
readily as mode 11, and a program can read and write it. It is the scarcest
region, so it is measured: the packed `_COMMON` + `_COMMON_DATA` run may
not exceed `KOS_COMMON_CEILING` (3,072 bytes), and the ceiling is never
raised to make something fit.

### 2.5 The CTC and its port map

The Z80 CTC's channel select is **crossed** on this board: its `CS0`
(pin 18) is the address bus's A1 and its `CS1` (pin 19) is A0, so the
selected channel is `A0*2 + A1` and the port that answers a channel is
not `40h + channel`:

| physical channel | port | trigger input | typical owner |
|---|---|---|---|
| 0 | `40h` | 1.8432 MHz | the SIO0/A baud clock (application) |
| 1 | `42h` | 3.6864 MHz | free |
| 2 | `41h` | 7.3728 MHz | the resident timer driver (`docs/devices.md` 9) |
| 3 | `43h` | 7.3728 MHz | free |

`platform/zephyr80/public/hardware.h` names one port per physical
channel (`KOS_CTC0_PORT`-`KOS_CTC3_PORT`) and no code computes a port
from a channel number.  The channel a port selects also decides the IM2
vector byte the CTC requests (entry `2*N` of the page), so programming
the wrong port is not an addressing slip but a different interrupt: the
timer's stub answers entry `04h`, and a tick programmed through `42h`
would vector `02h` instead.  The board's CP/M BIOS documents the same
fault and fixes it with a four-entry port table
(`../Zephyr-80/Code/HOST/CPM2.2/docs/ctc-and-real-time-programming.md`).

### 2.6 The early serial console

SIO channel B (data `22h`, control `23h`, 1.8432 MHz clock): asynchronous
x16, 115,200 baud 8N1, polled, interrupts off. Initialization:

```text
23h <- 18h          channel reset
23h <- 04h, 44h     WR4: x16, 1 stop bit, no parity
23h <- 03h, C1h     WR3: receiver on, 8 bits
23h <- 05h, EAh     WR5: DTR, transmitter on, 8 bits, RTS
23h <- 01h, 00h     WR1: interrupts off
```

Transmit polls RR0 bit 2 (`04h`); receive polls RR0 bit 0 (`01h`). The
early console is reachable from every mapping and needs no driver. It
remains the panic path when a `CONSOLE` driver exists.

---

## 3. Boot

### 3.1 Reset and the ROM-to-RAM copy

At reset the latch is `00h`: mode 00, ROM page 0 on every read. The
bootstrap in ROM page 0 runs stackless:

```text
DI
copy ROM page 0 -> SRAM bank 0         (one LDIR, latch 00h)
latch 27h: ROM page 1, mode 00, bank 7
copy ROM page 1 -> SRAM bank 7         (one LDIR)
latch 10h: mode 10 over bank 0
common code: latch 18h, mode 11 over bank 0
kernel entry in bank 7
```

The two boot pages carry byte-identical bootstrap bytes at every address
where the latch changes, so the instruction fetched after each `OUT` is the
same in ROM and RAM. Mode changes that move `2000h-DFFFh` run from common
memory. The boot path depends on no state left by firmware or CP/M.

Kernel entry guarantees: interrupts disabled, mode 11, the kernel visible
at its linked addresses, a valid stack, the C runtime state initialized,
and no assumptions about previous register contents. `kernel_main()` does
not return.

`SYS_REBOOT` follows exactly this path again: the kernel masks interrupts,
stops every CTC channel it may have left programmed
(`platform_ctc_quiesce_all()`; the CTC is not reset by the ROM reset
vector, so a running tick or event timer would otherwise keep requesting
into the next boot), writes latch `00h` from
common memory and jumps to `0000h`, so the copy above runs a second time
on a machine whose RAM is still whatever the program left in it. Nothing
is inherited across the jump: no driver keeps a handle, no process keeps
a bank, and no peripheral receives a hardware reset. A driver's bring-up
must therefore program its device fully from any state
(`docs/devices.md` section 14).

### 3.2 Boot sequence

```text
reset and copy (3.1)
  v
nucleus initialization: early console, banner, IM2 executive
  v
VFS: the root and the three static providers
  |-- /system  ROMFS provider (usable now)
  |-- /fs      FS provider
  `-- /dev     DEVICE provider
  v
driver loader: one pass over drivers.man, in load order
  for each driver: copy its segments to their host-assigned bank-6
  addresses, install its endpoint, publication and dependency records,
  run its boot segment once, run its hot init once
  v
boot launcher: run the configured boot program
```

- The device-event table and its per-process flags are cleared before
  `irq_init()` enables interrupts (the common region arrives as `FFh`, so
  the table is invalid until the boot clears it; the two delivery checks
  in `kernel/irq/entry.asm` and `kernel/syscall/entry.asm` read it on
  every return).
- The SIO driver loads first, so a later driver failure still leaves the
  serial console.
- A required driver that fails to initialize is a boot failure, reported on
  the early console. The namespace is never rebuilt around a missing
  driver.
- There is no mount pass, no device registration, no class discovery and
  no hardware probing at boot. The selected image defines the machine.

### 3.3 The boot launcher

The boot program and the policy that follows its exit are build settings:
the `boot` and `after_exit` fields of the image's system file (section
8.1), for example `/system/shell.exe` with `relaunch`. The launcher runs
the program, prints one line per cycle, and applies the policy:

| event | action |
|---|---|
| the boot program exits with a status | `boot: <path> exit <n>`; `relaunch` starts it again, `monitor` starts `/system/monitor.exe` |
| the run cannot start (< 0: missing, rejected, fault) | `boot: <path> failed <code>` and `/system/monitor.exe` |
| the monitor exits | the boot program again |
| the monitor itself cannot start | the controlled halt |

Production boots the shell and relaunches it; every acceptance image ends
at the monitor after one run instead of looping. The boot path reaches the
kernel through a generated header, never a compiler `-D` flag.

Every start of the boot program begins on a console reset
(`KCONSOLE_OP_RESET`, `docs/devices.md` section 14): the terminal goes
back to its defaults on a clean screen and input the program that went
away never read is dropped. The ordinary console path is used; the
launcher has no second mechanism for display or input state.

The launcher is also the only place a warm reboot is noticed: a program
that calls `REBOOT` (section 6.2) re-enters the boot path of section 3.1
exactly as a power-on does, and the launcher then starts the boot program
again on that fresh console.

### 3.4 Failure

A fatal error prints the clearest available diagnostic on the early
console, disables interrupts and halts (`panic()`).

---

## 4. The nucleus

### 4.1 Responsibilities

The nucleus owns everything global, permanent and not pluggable:

- process identity, creation, image replacement, exit and context;
- bank and extended-memory ownership;
- execution-domain transitions and cross-bank copy primitives;
- the VFS root and the three static namespace providers;
- the global owner-tagged handle table;
- the endpoint table and IPC dispatch;
- IRQ ownership, dispatch and the final `RETI`;
- loading and validating executables and resident driver images;
- the ROMFS reader;
- boot, the early console, failure handling and panic.

Everything that implements a protocol is a driver (section 5).

### 4.2 Fixed core and cold half

In mode 11 the nucleus sees two parts of bank 7:

```text
2000h-5FFFh   fixed core (16 KiB)   always visible
6000h-DFFFh   cold half (32 KiB)    visible only while the window shows bank 7 (P = 0)
```

- **Fixed rule.** Code that executes, and data that is touched, while the
  window shows another bank lives in the fixed core: the IPC dispatcher,
  the endpoint and descriptor tables, the copy primitives and transition
  code, nucleus primitives callable by drivers (including the ROMFS
  reader), the IRQ executive, the upcall shims, the driver stack
  (`5D00h-5EFFh`, 512 bytes), the ISR stack (`5F00h-5FFFh`, 256 bytes)
  and the data all of these use.
- **Cold rule.** Everything else lives in the cold half: the syscall
  dispatcher and bodies, process management and the EXE loader,
  extended-memory allocation, the VFS router and providers, the driver
  loader, boot setup, diagnostics and panic formatting. The syscall gate
  clears P before dispatching, so syscall code always runs with the window
  showing bank 7.
- **Upcalls.** Cold code that calls a bank-6 driver (for example the loader
  streaming an executable) goes through a small fixed **upcall shim** that
  saves the latch, opens the driver window, calls and restores the latch.
- **Buffer rule.** Memory a driver reaches through a buffer or a path must
  never be in the cold half. The nucleus's provider-facing buffer, the VFS
  workspace with its path views (`vfs_workspace`), lives in the fixed
  core's runtime data. `config/residency.json` marks it `fixed-required`
  and `tools/mapcheck.py` fails the build if it moves.
- The fixed core's runtime data base and the cold half's data base are
  derived from the linked code at build time; there is no fixed boundary
  inside either region.

### 4.3 Page zero and the syscall gate

Every native process has a KonaOS-owned page zero in its own bank:

```text
0030h   RST 30h: JP kos_syscall_gate
0080h   startup block v1 (section 6.8)
```

No CP/M structures are present. The gate is in common memory because an
`RST` arrives in mode 10:

```text
RST 30h (mode 10, caller stack anywhere in 0000h-DFFFh)
  -> common gate: save the caller SP and interrupt state, switch to the
     common gate stack, save the registers the ABI needs, save the whole
     latch (Q and P), enter mode 11 over the caller's bank with P = 0
  -> restore the caller's interrupt state, then dispatch in the cold half
     (the body runs with it)
  -> return: mask again, restore the exact latch, the registers and the
     caller SP; restore the caller's interrupt state; RET
```

The gate never uses the caller's stack after the mapping changes, because
a caller stack in `2000h-DFFFh` disappears in mode 11. Syscall argument
blocks must lie in the caller window `0100h-1FFFh`, which stays visible in
mode 11; a block elsewhere is refused before dispatch. The gate stack is
384 bytes; the deepest measured chain (nested exec reading a driver-backed
file) uses 275 bytes, and the worst case must stay at or below 360.

Only the crossing itself is masked.  A syscall **body** runs with the
caller's interrupt state, because the CTC (5.6) holds one pending
interrupt: a body that ran masked for longer than two ticks would lose
every expiry after the first, and the 54/55 diagnostics would time the
loss as well as the call.  Masking inside a body is therefore explicit and
bounded - the copy primitives, ROM access, the IOC frame exchange and
bulk receive (4.5) - and an interrupt taken anywhere in the body is safe:
the executive keeps its own state and stacks, saves into the reservation
above the state cells, and restores the exact latch.

### 4.4 Syscall convention

```text
entry   RST 30h, A = syscall number, HL = argument block
return  carry clear: success, HL = result
        carry set:   failure, A = KOS_E* error code
```

Entry and return are masked; the body runs with the caller's interrupt
state (4.3), so a call may be interrupted and a driver that must hold
interrupts off masks explicitly and keeps the span bounded (4.5).

The full table and every block are in `docs/abi.md`. Section 6.4 explains
how `call` adds a second result channel.

### 4.5 The interrupt executive

- The nucleus owns the `I` register, IM2, the vector page, the IRQ entry
  path, the transition and ISR stacks, and the final `RETI`. A driver may
  own an interrupt **source**, never the mechanism.
- The **IM2 page** is `FD00h-FDFFh` with `I = FDh`, plus the guard byte at
  `FE00h`, because vector `FFh` reads its pointer from `FDFFh-FE00h`. Some
  expansion hardware presents `FFh` on the bus, so the build checks that
  `word_at(FDFFh)` is the address of `irq_ff_unexpected`.
- **Logical sources:** `IRQ_CTC0`-`IRQ_CTC3`, `IRQ_SIO0`, `IRQ_SIO1`,
  `IRQ_VDP_FF`. A source id is not a vector byte; the wiring is platform
  data.
- **Entry:** a common stub identifies the source, saves the interrupted SP,
  switches to the common IRQ transition stack, saves the whole register set
  (`AF BC DE HL IX IY` and the alternates), saves the exact latch, selects
  mode 11 and the owner's window, switches to the ISR stack and calls the
  handler. **Return** restores the exact latch, registers and SP, then
  `RETI`. An interrupt taken in mode 11 returns to mode 11. The transition
  reservation sits directly above the state cells and below the boot stack
  (`FE08h-FE87h`), so an interrupt taken while the boot stack has grown
  past its nominal base saves into the reservation and never into the
  interrupted chain's frames.
- IRQ delivery never disturbs a latch saved by the code it interrupts; the
  executive keeps its own save slot. An interrupt inside a bulk-map span
  (5.2) returns to Q = 6; handlers never use the caller window, and no
  event is delivered there, because the interrupted code is not a
  program's.
- **Handlers** are bounded, service their own device condition, never
  busy-wait, never call IPC, and return to the executive. Nested maskable
  interrupts are not allowed.
- **Reading the interrupt state:** every place that captures it (the
  syscall gate, the copy primitives, ROM access, `irq_arch_lock`, the
  driver helpers) reads IFF2 with `LD A,I` up to three times and takes any
  "enabled". An interrupt accepted right after `LD A,I` makes that one
  read report "disabled" (the documented `LD A,I` behaviour, seen on the
  board's Z84C0020); a gate that trusted it would hand a program back
  masked, and no event could then reach it.
- **Unexpected or unclaimed** interrupts are counted and, where a platform
  quiesce exists, the source is disabled. Nothing large is printed from
  interrupt context: the counters are published with the platform
  constants by `SYS_DIAG_SYSTEM_INFO` (a 13-byte record: `unexpected`,
  `unclaimed`, then the last source id) and the monitor's `R` prints them
  as its `irq:` line, which is what a board run reads when a handler that
  should run does not.
- **Driver-owned IRQs:** a resident driver claims a source through the
  bootstrap API (`irq_claim`, `irq_release`) and supplies a bank-6 handler.
  The executive opens the driver's window around the call. Delivery to a
  resident handler costs **1,343 T-states**, measured in the timer replay
  - gate 303, dispatcher and trampoline 674, handler 103, return 263 -
  which is 2.10 % of a tick at 156.25 Hz.  The timer driver's tick is the
  first production source that uses this path.
- **Device events** (`NOTIFY`, `EVENT_RETURN`, section 6.7 of
  `docs/abi.md`) ride the same two return paths.  A driver keeps its own
  interrupt and only calls `event_post`, so the delivery never runs in
  driver or interrupt context: the IRQ exit and the syscall exit each ask
  the event core whether the process they are about to return to has a
  deliverable subscription, and only deliver when that process's own code
  was interrupted - mode 10 over that process's bank, no handler of it
  running, and `pending > 0`.  A post for a process that is not the one
  running is dropped into `missed` rather than queued, which is what makes
  a child's run and another process's execution safe.

  | the process ... | delivery |
  |---|---|
  | is running its own code (an interrupt ends) | the frame is built below the interrupted SP from the transition stack, and the interrupt's `RETI` becomes a `RETI` into the libkos trampoline |
  | is inside its own syscall or `CALL` | the frame is built below the caller's return PC with the syscall's result registers in it, so the handler returns into the instruction after the call |
  | is inside its own handler | held, counted: the core refuses while that process's handler runs |
  | has a child running | dropped, counted in `missed` |
  | has exited or unbound the handle | the row is cleared with the handle |

  The frame is 24 bytes on the program's own stack - latch, the alternate
  set, IY/IX/HL/DE/BC/AF, PC - and `EVENT_RETURN` unwinds it to the exact
  interrupted latch, registers and SP.  Events coalesce: the handler gets
  `count` and `missed`, nothing queues.  The core is common memory
  (`kernel/event`), because both checks run with the application's latch in
  force, where bank 7 is mapped only by accident.
- **Masking is explicit and bounded.**  Nothing in the executive masks on
  a caller's behalf: a body runs with the caller's state (4.3), and the
  sections that hold interrupts off are the copy primitives and ROM
  access (5.3) and a driver's own critical segments.  The IOC driver's
  frame exchange and bulk receive are the longest of those, and a timed
  copy of a large card file matches a stopwatch on the board.

### 4.6 ROM access and the ROMFS reader

The recovery store occupies ROM pages 2-7. Only `0000h-BFFFh` of each page
is store data (48 KiB per page, 288 KiB in all); `C000h-FFFFh` carries the
ROM-access stub's mirror. A ROM read uses mode 00, so it runs through a stub
that is byte-identical in common SRAM and in every store page:

```text
mode 11: validate page/offset/length, save the exact latch, mask
         interrupts, set HL = source, DE = staging, BC = length
trampoline OUT: mode 00, page p, bank 0
mirrored stub: LDIR ROM -> bank-0 staging, OUT the saved latch
common epilogue: mode 11 restored, interrupts restored
```

The stub uses no stack and reads no SRAM between the two `OUT`s. The
**ROMFS reader** is a nucleus primitive in the fixed core, so drivers and
the loader can use it while bank 6 is mapped. There is exactly one ROMFS
lookup, an ASCII case-folded name match. The format is in
`docs/romfs-format.md`.

### 4.7 Processes

- **Single-tasking.** A parent runs a child synchronously and is suspended
  until the child exits. Only one process runs.
- **Process table:** 7 entries in the cold half, each with its state
  (`FREE`, `RUNNING`, `SUSPENDED`), primary bank, parent, saved context,
  exit status and flags. Bank 7 is never a process bank.
- **Nesting:** up to `KOS_EXEC_MAX_DEPTH` (4) user processes deep; the next
  exec is refused with `KOS_EBUSY` and allocates nothing. The kernel
  launcher is depth 0.
- **Exec** (`SYS_EXEC(path, tail)`):
  - every fallible step (path staging, slot, bank, CWD, load and
    validation, page zero) happens before the parent is suspended, so
    nothing past the suspension point can fail and no suspended process is
    ever rolled back;
  - the path is resolved through the namespace: the loader binds it,
    requires a `FILE`, streams the image through the FILE protocol and
    unbinds it;
  - the path is staged in the fixed-core VFS workspace (the buffer rule);
  - the caller's raw tail is copied byte-exact into the child's startup
    block; the kernel never parses it;
  - the child starts with P = 0;
  - a failed load rolls back everything allocated for the child.
- **Suspension:** the shared common gate stack is reused by the child, so
  the gate saves the parent's gate frame (SP, IX, latch, IFF and the
  frame bytes, 48 bytes) per depth and restores it before the parent's
  `SYS_EXEC` returns. From the start of the restoring copy until SP is
  restored there is no `CALL`, `PUSH`, `POP` or interrupt enable.
- **Exit** (`SYS_EXIT`) records the status, unbinds every handle the
  process owns (each owning provider closes or flushes its instance; a
  device handle with the release notification tells its driver the handle
  went away, and an exclusive one that the takeover ended), frees its
  extended-memory objects and segments and its bank, and resumes the
  parent with its exact latch, including P.
- A suspended parent keeps all its resources. A child inherits the parent's
  CWD (a copy) and nothing else.

### 4.8 Memory: application banks and extended memory

**Primary bank.** Every process owns one primary bank and runs in mode 10
with P = 0:

```text
0000h-00FFh   native page zero
0100h-....    the program image, then its zeroed BSS
....-DFFFh    free memory, then the stack, growing down from E000h
E000h-FFFFh   common memory
```

**Bank states.** One allocator owns banks 0-5. Each bank is `FREE`,
`PRIMARY` (the whole bank belongs to one process), or `AUXILIARY` (seven
8 KiB slots, each owned by one process). Primary banks are taken lowest
first; auxiliary slots highest bank first, filling existing auxiliary banks
before opening a new one. A bank with a live auxiliary slot cannot become a
primary bank, so `SYS_EXEC` can fail with `KOS_ENOSPC` while slots are
free.

**Auxiliary slots.**

```text
slots 0-2   0000h-5FFFh   data; copies go through the mode-10 gate (slow)
slot  3     6000h-7FFFh   data; window-speed copies
slots 4-5   8000h-BFFFh   the aperture slice: mappable objects, mappable data
                          segments and far-code segments
slot  6     C000h-DFFFh   data; window-speed copies
```

**Extended-memory objects** (`SYS_XMEM_*`):

- `ALLOC(size, flags)` returns an opaque handle; the size rounds up to whole
  slots; an object is contiguous in one bank; placement is never visible.
- A plain object is at most 32 KiB; a `MAPPABLE` object at most 16 KiB and
  always in the aperture slice. These are per-object limits. At most 16
  objects exist system-wide.
- Plain objects prefer the window-speed slots 3 and 6, then slots 0-2.
- `READ`/`WRITE` copy between an object and the caller's buffer, with all
  range arithmetic checked first. `INFO` reports sizes, flags and type,
  never a bank or slot. `FREE` releases a runtime object; loader-created
  segments are released at exit.
- `MAP(handle)` (mappable objects only) returns a **map token** and the
  object's address in the aperture (`8000h` or `A000h`).
- A far-code or mappable data segment takes slot 4 of its bank (slots 4-5
  when it is over 8 KiB). When it uses only slot 4, slot 5 may hold a
  runtime mappable object.
- **Contents are undefined.** Nothing pre-zeroes an object.

**Map tokens.** A token is the latch value that opens the aperture onto an
object or segment. The nucleus issues tokens only for the caller's own
objects and segments in banks 0-5 (P = 2-7), never for bank 6 or 7. The
SDK opens the aperture by writing the token and closes it by restoring the
previous latch.

**Memory initialization.** KonaOS zeroes only what the language or the ABI
requires: the BSS of the primary image and of each segment at load, and the
startup block and page zero at exec. Nothing else is cleared; a single-user,
single-tasking machine gains nothing from clearing memory for isolation.

### 4.9 IPC

Resident drivers export **endpoints**. The host assigns endpoint numbers;
the nucleus keeps a prebuilt endpoint table in fixed data (number, owner
driver, dispatch target, a re-entrant flag, a busy state). Endpoint numbers
are not application ABI.

- **Synchronous call and reply** is the only IPC model. There are no
  queues and no asynchronous messages.
- **Frame:** one 32-byte request/reply frame (24 bytes of payload) for every
  call. The handle's protocol defines what the fields mean.
- **Descriptor:** at most **one** caller buffer per call, named in the call
  block. At entry the nucleus captures it from the caller's saved latch
  (bank and page) as a small id and passes the id in the frame's `desc`
  field. The buffer must lie in `0100h-DFFFh` of the caller's bank or in
  common memory, and is at most 4,096 bytes. Drivers move data only with
  `copy_in(id, offset, buffer, length)` and `copy_out(...)`, which check
  bounds on every use. There are no raw cross-domain pointers.
- **Buffer identity:** a caller address in `8000h-BFFFh` while the caller's
  saved P ≠ 0 names the far bank in the aperture, not the primary bank.
- **Direct copies:** a descriptor id can be passed down a call chain, and
  the driver that holds the data copies straight to the caller. No driver
  stages data for another.
- **Copy paths** (chosen per request; all are correct, A and B are fast):

  | path | when | how |
  |---|---|---|
  | A | caller buffer in `0000h-1FFFh` or in common, driver buffer in the hot tier | mode 11, one block copy |
  | B | driver buffer in bank 6 `8000h-BFFFh`, caller buffer in its bank outside `8000h-BFFFh` | a common stub selects mode 10 over the caller's bank with P = 1, one block copy, latch restored; interruptible |
  | C | everything else | chunked through the common bank-copy gate |

  P = 1 is internal to the nucleus; no program ever receives a bank-6
  token. A block copy is an unrolled `LDI` run (16.6 T-states a byte,
  `kernel/runtime/fastcopy.inc`), as are the bank copy gates
  path C uses.
- **Driver I/O rule:** driver buffers used with `copy_in`/`copy_out` live in
  bank 6 `8000h-BFFFh`, the `DRIVER_IO` area: a driver's io segment, which
  the planner places there. The packer refuses a manifest that puts
  `_DRIVER_IO` in any other segment.
- **No copies inside a bulk-map span:** the caller window shows bank 6
  there (5.2), so every copy primitive refuses until `bulk_unmap`.
- **Validation lives in the nucleus only:** handle, endpoint, descriptor and
  frame bounds are checked once, at the boundary. Drivers trust what the
  nucleus hands them.
- **Nesting:** at most 4 calls deep (`KOS_IPC_MAX_NESTING`). A call into an
  endpoint already on the call stack is refused with `KOS_EBUSY` unless the
  endpoint declares itself re-entrant.
- **Driver-to-driver calls** are host-wired and near-direct: both drivers
  are mapped, so the call dispatches through the endpoint table with the
  frame passed by reference and no bank switch.
- **Cost:** a null application call costs about 15,500 T-states (the frame
  copies dominate); one window crossing costs 242. Protocols are therefore
  coarse-grained: no per-byte operations; writes are buffered, reads are
  read-N.

### 4.10 The namespace and handles

The VFS is a **namespace router**. It owns `/` and routes the first path
component to one of three static nucleus providers:

```text
/
|-- fs       FS provider: forwards to the filesystem driver the build selected
|-- system   ROMFS provider: the recovery store, FILE instances, no driver needed
`-- dev      DEVICE provider: the host-built publication table
```

- Root matching is ASCII case-insensitive; `fs`, `system` and `dev` are the
  canonical spellings. The provider owns every component below its root;
  the VFS never interprets a provider's hierarchy.
- **Paths:** absolute or relative to the CWD; `.`, `..` and repeated `/`
  are resolved; `..` above `/` stays at `/`. Comparison is ASCII
  case-insensitive (`A-Z == a-z`, every other byte exact); each provider
  implements the same rule for the paths it receives.
- **Limits:** `KOS_PATH_MAX` = 128 bytes including the NUL; `KFS_NAME_MAX`
  = 64. The native ABI does not inherit FAT 8.3; the current filesystem
  driver does (section 5.6).
- **Device names** describe roles, never products: `/dev/console`,
  `/dev/video`, `/dev/input/keyboard`, `/dev/serial/0`, `/dev/ioc` and
  `/dev/timer` and `/dev/sound`. One driver may publish several
  names and types.
- **Handles:** one global table of 12 entries, each tagged with its owner
  process, its 8-bit protocol type, its endpoint and the provider's object
  identity. A handle is small and opaque: not a bank, pointer or endpoint
  number.
- **`bind(path, flags)`** resolves a name and returns a handle and the type
  it actually resolved; the caller passes no expected type. A name is a
  class or factory, the handle an instance: binding `/dev/video` acquires a
  permanent object, binding a file opens one. The 16-bit flags are
  interpreted by the provider (open flags for `FILE`, zero for most
  devices).
- **`call(handle, op, frame)`** sends one protocol operation to the
  handle's endpoint. The VFS is not in the data path. The ROMFS provider
  answers in-process through the same path.
- **`unbind(handle)`** releases the instance. For a DEVICE handle the
  release is a notification: the nucleus calls the endpoint with
  `KOS_OP_RELEASE` (`arg0` = the object, `arg1` = 1 for an exclusive
  handle) so the driver can restore what the program changed. Only an
  endpoint whose image declared the release notification is called, and
  its result is ignored.
- **Exclusive handles.** `KOS_BIND_EXCLUSIVE` asks a device for a takeover:
  the nucleus first calls the endpoint with `KOS_OP_ACQUIRE` (`arg0` = the
  object), which the driver answers with 0 or `-KOS_EBUSY` while another
  handle holds the object. A device whose endpoint declared no release
  notification answers `-KOS_ENOTSUP` before any call is made. A program
  writes a device's ports, or reprograms it beyond its protocol, only while
  it holds that device's exclusive handle; the contract and what each
  driver restores are `docs/devices.md` section 14.
- **Standard handles:** every process starts with handles 1 (output) and 2
  (input) bound to `/dev/console`, bound fresh for that process, never
  inherited. `bind` never returns them. A build with no console driver
  starts processes without them.
- **No universal directory protocol.** A slash below a root is provider
  syntax. Enumeration is `ns_enum(path, cookie, buffer)`: batched entries
  with a name, a `container` flag (further components may follow) and, for
  bindable entries, the type. `container` is metadata, not a type.
- **CWD** is a global path per process. `chdir` asks the owning provider
  whether the path is traversable and stores the canonical path; it binds
  nothing. `getcwd` returns it.
- **Device-events subscriptions** live on the handle they were made
  with: `UNBIND` releases the rows of that (process, handle) pair and tells
  the driver through the same `KOS_OP_SUBSCRIBE` call it was told about the
  subscription (the id 0), and process exit drops every row of it.  A
  device whose handle goes away therefore stops its source, and the next
  program can subscribe.
- **Files:** `open`/`close` are SDK words built on `bind`/`unbind` with a
  `FILE` type check. Name operations (`stat`, `mkdir`, `rmdir`, `unlink`,
  `rename`) are filesystem-namespace protocol calls through `/fs`. A write
  under `/system` returns `KOS_EROFS`. A missing card makes `/fs` answer
  with `KOS_ENODEV`; `/system` and `/dev` are unaffected, so a machine
  without a card still boots and recovers.

### 4.11 The loaders

- **The driver loader** makes one pass over `drivers.man` in load order
  (section 3.2). It checks only the bounds that protect the nucleus and the
  fixed bank-6 layout. Driver images carry no relocations: the host linked
  them at their final addresses.
- **The EXE loader** reads the header, makes one up-front feasibility check
  (sizes, segment count, slots, 32-bit offsets), places the primary image
  at `0100h` and each segment, zeroes the BSS areas, and rolls everything
  back on any failure. An executable under `/system` must be an EXE entry
  in the store; a `/fs` file is judged by its header alone. A container
  built for another `abi_version` is refused.

---

## 5. Resident drivers

### 5.1 What a driver is

A **driver** is a resident implementation of one or more protocol types. It
need not correspond to hardware: it may be hardware-facing (IOC, HID,
V9958, SIO), a filesystem (IOC-FAT), or a composition (a `CONSOLE` built
from `VIDEO` + `INPUT`, or from `SERIAL`). Drivers are selected when the
image is built, copied at boot and never unloaded: no revocation, no
restart, no hot replacement.

### 5.2 The driver bank

```text
bank 6 6000h-DFFFh   hot tier (32 KiB): everything a driver runs after boot,
                     and its run-time data; 8000h-BFFFh holds DRIVER_IO first
bank 6 0000h-5FFFh   boot/bulk tier (24 KiB): boot-only code and bulk data
  0000h-00FFh          reserved page zero
  0100h-1FFFh          mapped range (7,936 bytes): data a driver reads and
                       writes directly inside a bulk-map span
bank 6 E000h-FFFFh   unused (common is mapped over it)
```

- The hot tier is reached through the mode-11 window with P = 1. It holds
  every path a user request can reach, including rare ones (for example the
  V9958 display reset behind `ESC c`).
- The boot/bulk tier holds code that runs only during bring-up (font
  upload, filesystem setup) and bulk data drivers reach through nucleus
  copies: `bulk_read` and its mirror `bulk_write`, bound to the calling
  driver's own extent (the V9958 font; the IOC-FAT block cache). The
  loader runs boot code through the mode-10 gate with Q = 6. No code
  there runs after boot.
- While a driver runs in mode 11, `0000h-1FFFh` shows the caller's bank;
  drivers reach caller memory only through descriptors.
- **The bulk-map span** is the one exception. `bulk_map` sets Q = 6, so
  `0000h-1FFFh` shows bank 6's own first 8 KiB and a driver reads and
  writes its **mapped segment** at its linked address (no translation,
  2.2); `bulk_unmap` restores the exact Q it replaced. Inside a span the
  driver touches only its own memory (hot tier, `DRIVER_IO`, its mapped
  segment) and its device ports; the request frame, which a syscall
  passes in the caller window, is out of reach too: `copy_in`, `copy_out`, `bulk_read`,
  `bulk_write`, `call` and `event_post` refuse, and `bulk_map` does not
  nest and is refused to interrupt handlers. A span never outlives the operation that opened it: the driver
  closes it before it returns, and the endpoint return path closes a span
  left open and counts it as a driver fault. Spans are for the hot path's
  read-mostly data (the V9958 font); one costs two primitive calls.
- **Mapped segments** (manifest role `mapped`, image and BSS) are placed
  by the host planner in `0100h-1FFFh`, in load order; the total may not
  exceed 7,936 bytes. Boot code and bulk data are placed above them. The
  reserved page zero keeps bank 6's `0000h-00FFh` free for the page-zero
  contract every bank meets (4.3).

### 5.3 The driver image

A resident driver is a **MOD image** (`docs/module-format.md`) built from
its manifest (`drivers/<dir>/<id>.json`): its hot segment, its `DRIVER_IO`
data, its boot segment and bulk data, its endpoints, its `/dev`
publications (name, type, endpoint, object) and its dependencies. The host
packer links every selected driver at its final bank-6 address, assigns
endpoint numbers, rejects duplicate endpoints and publications, resolves
dependencies and emits the bank-6 layout, the endpoint and publication
tables, the resolved dependency references and the boot manifest
`drivers.man`.

Driver images are a **source and build contract** with the OS tree, not a
binary module ABI: a driver is built together with the image that carries
it.

**Endpoint key versus protocol type.** An endpoint is keyed by
`(class, instance)` (`KOS_CLASS_*` in `abi/kos/protocol.h`), which is how
manifests declare and resolve dependencies. The 8-bit **protocol type** a
`bind` returns (`KOS_TYPE_*` in `abi/kos/object.h`) is a separate
numbering: class HID serves type `INPUT`, and the FILE class serves both
`FILE` and `FS`.

### 5.4 The bootstrap API

A driver's init receives one pointer, its **boot bundle**: the nucleus
table it may use and its resolved dependency array. The table holds:

- `call(dependency_ref, op, frame)`: a near-direct call to a resolved
  dependency;
- `copy_in`, `copy_out` and the bulk-data reader;
- `bulk_map`, `bulk_unmap`: the bulk-map span (5.2);
- `irq_claim`, `irq_release`;
- the few other primitives a driver needs, each named in
  `drivers/include/driver.h`.

There is no general kernel import table. An IRQ handler sees a restricted
subset with no IPC.

### 5.5 Dependencies and composition

A manifest declares dependencies **symbolically**, by protocol type with an
optional instance (`"INPUT"`, `"VIDEO:1"`; a test-only class is written
`"#16"`; a trailing `?` makes it optional). The build resolves each to the
one selected driver that exports it; none or more than one is a build
error. Drivers never bind `/dev` names at run time to find each other.

`/dev/console` is not special. Each build selects one console driver:

```text
console-v9958-hid   CONSOLE over VIDEO (V9958) + INPUT (HID) + SERIAL
                    (the serial rescue path: the ESC gesture, input
                    ownership and the tee)
console-serial      CONSOLE over SERIAL (SIO)
```

There is no run-time fallback between them: a build has exactly the
console it selected. Programs cannot tell which implementation answers.

### 5.6 The drivers

| driver | protocols | notes |
|---|---|---|
| `sio` | `SERIAL` | SIO channels; publishes `/dev/serial/0` |
| `ioc` | `IOC` | the Z80-to-MCU transport (below) |
| `hid` | `INPUT` | keyboard input over IOC; `/dev/input/keyboard` |
| `v9958` | `VIDEO` | 85×26 terminal renderer (6×8 font); `/dev/video` |
| `console-v9958-hid`, `console-serial` | `CONSOLE` | the per-build console |
| `iocfat` | `FILE`, `FS` | the filesystem behind `/fs` |
| `timer` | `TIMER` | the monotonic clock: CTC physical channel 2 (port 41h), 156.25 Hz, `/dev/timer` |
| `sound` | `SOUND` | the Afternoon Blend card (four SN76489 PSGs and a DAC); `/dev/sound` |

- **IOC is transport only.** It owns SIO1 framing, the command and bulk
  lanes, the READY/DONE lifecycle, sequencing, timeouts, recovery and the
  semantic status. It knows nothing of names, FAT, handles, paths or
  caching.
- **IOC-FAT** owns the FS2 commands, file and directory tokens, MCU error
  translation and a one-slot, 512-byte, write-through **file-data cache**
  (keyed by FS2 file, generation and aligned offset). A write changes no
  cached byte until the MCU confirms it. A confirmed write, a truncating
  open, `unlink` or `rename` also invalidates the other open file's slot; a
  generation change or `STALE` reply invalidates everything. FS2 tokens
  never reach programs. The deployed firmware limits card names to **8.3**.
- Caching is the provider's policy. The VFS has no cache, and ROMFS needs
  none.
- **The console's serial rescue path.** The production console owns
  three bytes of driver state - the input owner, the tee flag and the
  ESC count - written only by its dispatch.  Three 1Bh bytes from the
  serial line toggle input ownership between the HID and the serial
  line and arm the tee, which mirrors `WRITE` and `PUTC` to SERIAL
  (VIDEO first; a SERIAL failure does not fail the call) and refuses
  `RENDER` with `-KOS_ENOTSUP`, so `libtui`'s ANSI fallback reaches
  both screens.  Three more ESCs switch ownership back; the tee stays
  armed until the reboot.  Detection is polled: every input op drains
  the serial line through `KSIO_OP_GETC`, so a program that never
  calls the console's input ops does not see the gesture.  Only the
  third ESC of a run is consumed; the first two are delivered as data
  while the serial line owns input, so VT100 key sequences keep their
  lead-in.
- **The V9958 text renderer writes glyphs directly.** Glyph bytes go to
  VRAM through the data port; the command engine does only the fills
  (blanks, erase), scrolling (the R#23 origin) and the attribute-only
  inversion (LMMV/EOR). A GRAPHIC 6 cell is three whole bytes, so no VRAM
  is read and VRAM holds no font. The driver's mapped segment holds:
  - the 1-bit 6×8 font, scanline-major: eight 256-byte pages, page `s`
    holding row `s` of every character, indexed by the character code;
  - the expansion pages, which carry the colours: three 256-byte pages per
    foreground/background pair, page `b` mapping a font row byte to output
    byte `b` of the cell (two pixels). The normal and the reverse pair are
    resident (1,536 bytes); another pair is a rebuilt set of pages.

  A print run (up to 64 glyphs on one row) is drawn scanline by scanline,
  one VRAM address per scanline and three bytes per glyph, inside one
  bulk-map span. The font asset is built from
  `drivers/v9958/assets/font_cp850_6x8.inc` into this layout on the host.
- **Console geometry is reported, not fixed.** `KCONSOLE_OP_INFO` returns
  the text grid (columns, rows) and the attribute bits the console
  supports; the V9958 console forwards it to `KVIDEO_OP_INFO`, and the
  serial console reports the grid its programs draw (85 × 26). The render
  stream is bounded by the reported grid. `libtui` reads it at init and
  refuses a grid above its compiled capacity. `attr` is a bit field
  (bit 0 reverse; the other bits reserved and refused), and INVERT is the
  normal/reverse exchange only, so a colour change is a TEXT redraw.
- **Sound is lifecycle plus a coarse path.** The `sound` driver
  (`KOS_CLASS_SOUND`, type `SOUND`, `kos/ksound.h`) owns the card's reset
  and a batched write. It keeps no copy of the chips' registers, because a
  program holding the device exclusively may write them directly, so one
  reset routine recovers from any state:
  - **The reset** writes `9Fh BFh DFh FFh` to each PSG, which silences all
    four of its channels, and the DAC's neutral code to the DAC. It runs at
    the driver's init, at `KOS_OP_ACQUIRE`, at `KSOUND_OP_RESET` and at the
    release of an exclusive hold.
  - **`KSOUND_OP_INFO`** returns `struct ksound_info` in the reply payload:
    the PSG count and each PSG's port (listed one by one, `E0h-E3h` today;
    the ABI does not assume they are contiguous), the DAC's port (`E4h`),
    the PSG clock (3,579,545 Hz) and the capability flags (direct I/O,
    batched write, DAC).
  - **`KSOUND_OP_WRITE`** takes one IN descriptor holding `{target, value}`
    pairs: target 0-3 a PSG, 4 the DAC. The driver walks them in order
    through a 64-byte `DRIVER_IO` buffer and performs each `OUT`. One call
    carries a whole music tick, because a call costs about 15,500
    T-states (section 7.5) and one per register write would cost more than
    the music. The targets are logical: the IPC path names no port, so
    another card can serve the same protocol.
  - **Direct I/O** is the exclusive holder's: it reads the ports from
    `KSOUND_OP_INFO` and writes them itself (`sound_out` in the SDK),
    with no call per byte. The hold's release resets the card, so the
    next program never inherits a sounding PSG.
  - **No timing in the protocol.** A write happens when it is sent; a
    program paces its music with the timer's events.
  - WRITE and RESET are served on any handle: one process runs at a time
    and a child inherits no handle, so the hold protects the card's
    lifecycle, not each call.

  Sound programs use this driver, not diagnostic I/O (`SYS_DIAG_IO_OUT`).

---

## 6. The native ABI

### 6.1 Layers

1. **The nucleus ABI:** objects, namespace, processes, memory, segments,
   diagnostics (section 6.2).
2. **The IPC envelope:** handle, operation, 32-byte frame, status, one
   optional descriptor.
3. **Protocol types:** `FILE`, `FS` (filesystem namespace), `CONSOLE`,
   `VIDEO`, `INPUT`, `SERIAL`, `IOC`, `TIMER` and `SOUND`. Each defines
   its operations and frame meanings in `abi/kos/protocol.h`.
4. **The SDK:** typed client functions that bind names, check types and
   fill and decode frames (`docs/sdk.md`).

The native ABI is not frozen; until it is, a change carries no
compatibility path.

### 6.2 Syscalls

| number | name | purpose |
|---|---|---|
| `00h` | `EXIT` | end the process with a status |
| `04h` | `GETINFO` | process identity record |
| `05h` | `EXEC` | run a child synchronously |
| `06h` | `BIND` | resolve a name to a handle and type |
| `07h` | `CALL` | one protocol operation |
| `08h` | `UNBIND` | release a handle |
| `09h` | `NS_ENUM` | enumerate a namespace location |
| `0Ah` | `CHDIR` | change the CWD |
| `0Bh` | `GETCWD` | read the CWD |
| `0Ch` | `REBOOT` | warm reboot into the reset path (3.1) |
| `20h`-`25h` | `XMEM_ALLOC`, `FREE`, `READ`, `WRITE`, `INFO`, `MAP` | extended memory |
| `26h`, `27h` | `SEG_INFO`, `SEG_MAP` | the program's own segments |
| `28h`-`2Dh` | `DIAG_*` | recovery and diagnostics (6.9) |

There is no `open`, `close`, generic handle-information call, device lookup
or class query in the nucleus. Unassigned numbers return `KOS_ENOSYS`.

### 6.3 Objects and protocols

Every bound handle carries one 8-bit type; there is no capability bitmap.
Protocol-specific information belongs to the protocol (for example video
information, file metadata). Operations are coarse: a file moves in useful
blocks, a console writes a buffer.

### 6.4 Two result channels

`SYS_CALL` reports in two places, and they mean different things:

- **The syscall result (carry and `A`):** carry set means the request never
  reached the IPC layer: the block was outside the caller window, the frame
  length was wrong, or a reserved byte was set. `A` is the error.
- **The IPC status (`HL` with carry clear, also written into the block):**
  0 or positive is the driver's result; negative is `-KOS_E*`, a refusal by
  the IPC layer or the driver (unknown handle, forged descriptor, copy
  bounds, nesting depth, re-entrancy, or a protocol error).

The SDK folds both into one return value: non-negative on success, `-errno`
on failure. Every other syscall has only the first channel.

### 6.5 Files

- The `FILE` protocol: read, write, seek, metadata, sync, on a bound file.
  I/O is byte-oriented; a zero-byte read is end of file; short reads are
  valid. Sizes and positions are 32-bit; one transfer is 16-bit. A write
  advances the position only after confirmed completion.
- `close` releases the instance and flushes it; `sync` flushes while the
  file stays open. There is no filesystem-wide sync.
- The filesystem-namespace protocol (`FS`): `stat`, `mkdir`, `rmdir`,
  `unlink`, `rename` by name. A rename across providers returns
  `KOS_EXDEV`; the system never turns a rename into a copy.
- **Metadata** (`struct kfs_stat`): type (`KFS_TYPE_FILE`, `KFS_TYPE_DIR`),
  attributes (`READONLY`, `HIDDEN`, `SYSTEM`; no archive bit), 32-bit size,
  `ctime`, `mtime`. The machine has no clock, so times are
  `KFS_TIME_UNKNOWN` (0). No inode numbers, clusters, tokens, owners or
  modes are exposed.

### 6.6 Errors

Error codes are `KOS_E*` in `abi/kos/errno.h`, one numbering for the whole
system. Filesystem conditions have their own codes: `EEXIST`, `ENOTDIR`,
`EISDIR`, `ENOTEMPTY`, `EROFS`, `ESTALE`, `EXDEV`, `EMFILE` (handle-table
or open-file exhaustion, distinct from `ENOSPC`, storage or memory full).
The values and each call's failure list are in `docs/abi.md`.

### 6.7 The executable format (EXE)

An EXE file (`docs/exe-format.md`) holds a header, a primary image that
loads at `0100h`, a segment table and payloads. The header carries magic
`KEX2`, format version 2, `abi_version` and every size the loader needs.

- **Far-code segments** (ids first, 1..k) are linked at `8000h`, at most
  16 KiB, placed at slot 4 of an auxiliary bank (one per bank) and need no
  relocation.
- **Data segments** follow. A plain data segment becomes an
  extended-memory object reached by handle, at most 32 KiB. A data segment
  with the `MAPPABLE` row flag (at most 16 KiB) is placed at slot 4, so it
  sits at `8000h` where it was linked, and can be mapped.
- File geometry is 32-bit, so a container can exceed 64 KiB.
- The host (`tools/exepack.py`) validates geometry, overlap and bounds and
  pre-encodes the container. The loader only reads descriptors, checks the
  bounds that protect the nucleus, copies payloads and zeroes BSS.

### 6.8 Process startup

At entry `HL = 0080h`, the **startup block v1**:

```text
0080h  u8  version = 1
0081h  u8  tail_length (0..125)
0082h  ... the raw tail, then one NUL
```

The block sits below `0100h`, so a program copies the tail before its
first syscall (the SDK's startup code does). The kernel never parses the
tail; the SDK splits it into `argc`/`argv`. A later version may add fields
without changing the launch contract, because the program reads `version`.
The process starts with standard handles 1 and 2 and its parent's CWD.

### 6.9 Diagnostics

`SYS_DIAG_*` reads and writes memory in banks 0-6, reads and writes I/O
ports outside the latch-aliased `00h-0Fh`, and reports bank and system
information. They are explicit recovery interfaces for the monitor and the
diagnostics, not a way around drivers. They never reach bank 7.

---

## 7. Programs and far memory

### 7.1 Ordinary programs

A program is an EXE built from C with the SDK (`docs/sdk.md`). It
includes only `kos.h`, links the startup code and `libkos`, and never
writes entry assembly. A program that uses no far facility has
`0000h-DFFFh` to itself and never sees P ≠ 0.

### 7.2 Far code

- A far-code source is built into segment n with `FARn=` (n = 1..4). Every
  `__banked` function in it is called as plain C: SDCC emits the segment
  id, the address and a call to `___sdcc_bcall_ehl`, and the SDK's
  trampoline maps the segment (caching its token), opens the aperture,
  calls, and restores the caller's latch. Far code may call other segments'
  banked functions.
- `farcall(segment, fn, a, b)` is the primitive under it, for code that
  maps explicitly.
- **Link rule:** code, stack and data used while the aperture is open live
  outside `8000h-BFFFh` of the primary image. Far code may call primary
  code and use pointers into primary memory outside the aperture. A
  pointer into another far segment is not valid.
- Syscalls from far code are ordinary: the gate saves and restores the
  whole latch, P included.

### 7.3 Far data

| kind | build | access |
|---|---|---|
| `DATAn=` sources | `const` tables and zeroed variables, linked at `8000h`, mappable | `farmap(n)` opens it; plain C symbols and pointers until `farunmap` |
| `BLOBS=` files | binary payloads bundled in the EXE | `blob(n)` gives the handle; `xread` copies |
| runtime objects | `xalloc` | `xread`/`xwrite`, or `xmap` plus the aperture if mappable |
| a file loaded at run time | `xload(path, flags)` | as a runtime object |

A data source with an initialized writable object is refused at build time:
far data is either constant or zeroed. A file larger than one object is
streamed, not loaded.

**The one-window rule.** There is one aperture, `8000h-BFFFh`. Far code and
mapped far data cannot be visible at the same time. Primary-image code can
map data and use pointers; far code reaches far data by copy. Each far-code
segment and each mappable data segment occupies one auxiliary bank's
aperture slice, so their number is bounded by the free banks (`run` returns
`KOS_ENOSPC` when the banks run out).

### 7.4 Native programs in the store

- **`shell.exe`** is the command interpreter and the production boot
  program: a read-run loop that splits one line with the shared splitter,
  resolves a built-in or a program through its command path (`.`, `/fs/BIN`,
  `/system`), and reports the child's status.  On every start it prints a
  banner - the title, version and copyright lines plus the KonaOS logo,
  a V9958 GRAPHIC 6 LMMC pixel stream
  (`user/shell/konaos-logo-80x46.lmmc`, one pixel per byte in the low
  nibble) drawn at (8, 10) with one LMMC through the video driver: the
  command block carries the first pixel in R#44 and the rest of the
  stream follows through R#44, paced by the hardware WAIT (a serial
  build prints the text alone) - and leaves the cursor on the prompt's
  row.  Its
  built-ins are `cd pwd
  echo path help exit reboot` and the file commands `ls cat stat cp mv rm
  mkdir rmdir`; `/fs` is shown as `/` and `//` names the real namespace
  root. Its line editor adds CTRL-L (clear the screen and redraw the
  prompt and line) and a history of the last 5 lines on the up and down
  arrows, kept for the session. It is an ordinary program (`user/shell/`): the image's store
  carries it, the launcher runs it, and the monitor it starts on request is
  a child like any other - the acceptance images' launcher starts the
  monitor instead, through `after_exit`.
- **`monitor.exe`** is the recovery tool and the acceptance images'
  fallback: memory
  and I/O inspection (`R`, `D`, `M`, `I`, `O`), running a program from
  `/system` or `/fs` (`X path [tail]`), help and quit. It uses only the
  console and the diagnostic syscalls, so it works with either console
  driver, and it stays small and independent of everything it inspects
  (`docs/monitor.md`).
- **`hwtest.exe`** is the production feature exerciser, run as `hwtest` at
  the shell (or with `X /system/hwtest.exe` at the monitor): extended
  memory and far code, files (writes
  confined to `/fs/HWTEST`, which it removes), the monotonic clock, the
  namespace and handles against the real devices, and nested exec. It
  reports one line per stage and a final `n/m PASS`, with a card and
  without one.
- **`timer.exe`** is the clock's stopwatch program (`tests/timer/`):
  run bare it prints one elapsed second per second for a minute; the
  `stage` tail runs the acceptance checks (INFO, 10,000 monotonic
  readings, a 1 s advance); the `card` tail copies a card file to
  `/fs/TIMERTMP.BIN` in 512-byte calls, deletes it and prints the elapsed
  clock, so a card transfer can be held against a stopwatch.

### 7.5 Costs

At 10 MHz (1 T-state = 0.1 µs):

| operation | cost |
|---|---|
| banked call, token cached | 310 T-states (31 µs) |
| first banked call into a segment | + 85 T-states + one `SYS_SEG_MAP` |
| access through an open aperture | none |
| `xread`/`xwrite`, slots 3-6 | about 21 T-states per byte |
| `xread`/`xwrite`, slots 0-2 | about 287 T-states per byte |
| null IPC call | about 15,500 T-states |
| a 512-byte card read | about 87,000 T-states, about 170 T-states per byte |

The per-call part is the whole story: a 512-byte read amortises one call
and one block over 512 bytes, about 170 T-states each, while the same
file read one byte per call pays the call - measured at about 29,000
T-states per byte by `tests/bench/iobench.c` - every byte.  Everything
the SDK does in quantity is built on that: the console's 128-byte line
buffer and the stream's 512-byte refill (`docs/sdk.md` section 6.10).

---

## 8. The build

### 8.1 One system file per image

Each image is described by `systems/<image>.json`:

```json
{ "schema": "kos-system-1",
  "name": "production",
  "drivers": ["sio", "ioc", "hid", "v9958", "console-v9958-hid", "timer",
              "iocfat"],
  "boot": "/system/shell.exe",
  "after_exit": "relaunch",
  "store": ["shell.exe", "monitor.exe", "hwtest.exe", "timer.exe", "..."],
  "cflags": [] }
```

- `drivers` is the load order. A bare id is `drivers/<dir>/<id>.json`; a
  test-only driver is named by its repository path without `.json`.
- `boot` is the boot program; it must be an EXE in the store under
  `/system`.
- `after_exit` is `relaunch` or `monitor`, the launcher's policy for a
  normal exit; a `monitor` image must carry `monitor.exe` in its store.
- `store` lists the store's own files. The store order is always: the
  driver images in load order, then `drivers.man`, then the `store` files.
- `cflags` are the image's extra compiler flags. A path containing `.exe`
  never goes in a flag, because SDCC rejects any argument that contains it.

`tools/systemgen.py` validates the schemas, resolves every driver and
dependency, checks the boot program and writes `system.mk`, the store
manifest, `drivers.man` and a resolved copy of each driver manifest for the
packer. `make SYSTEM=<name>` builds an image; the named targets
(`fs-selftest` and so on) select their system file.

### 8.2 Host validation

- `tools/mapcheck.py` checks the linked map against the platform
  description: page zero, common placement, the fixed/cold split, the
  `fixed-required` objects, the common ceiling, the stacks, the IM2 page and
  its guard.
- `tools/pack_image.py` checks the byte-level image (size, fill, the reset
  copier, page ownership, the `RST 30h` stub) and emits the image manifest
  with the ledgers.
- `make check` rebuilds from clean, proves the rebuild reproduces the same
  image, and runs the host tests and the production replay.
- The simulator (`tests/z80sim.py`) replays images on the host. Physical
  hardware stays authoritative for banking, the aperture, interrupt
  acknowledgement and timing, serial and IOC behaviour; `tools/serial_smoke.py`
  captures and checks a board's serial output.

### 8.3 The SDK

The in-tree build and the standalone SDK share one rule file,
`mk/kos.mk`. `make sdk` produces `kos-sdk-<version>/` and its tarball: the
headers, `libkos.lib`, `crt0.rel`, the standalone tools, the rules, a
template, examples and the SDK documents. A developer builds a program with
no OS source and runs it from the card. `VERSION` carries the SDK version
and the ABI number.

---

## 9. Budgets and ledgers

Resident bytes are an architectural resource. Every change reports these
ledgers (`docs/memory.md` has the current values):

| ledger | region | capacity | rule |
|---|---|---:|---|
| `nucleus fixed:` | bank 7 `2000h-5FFFh` (code, data, ISR and driver stacks) | 16,384 | keep ≥ 2,048 free |
| `nucleus cold:` | bank 7 `6000h-DFFFh` | 32,768 | keep ≥ 4,096 free |
| `driver hot:` | bank 6 `6000h-DFFFh` | 32,768 | keep ≥ 1,536 free |
| `driver boot/bulk:` | bank 6 `0000h-5FFFh` | 24,576 | — |
| `driver mapped:` | bank 6 `0100h-1FFFh`, inside boot/bulk | 7,936 | — |
| `common:` | packed `_COMMON` + `_COMMON_DATA` | ceiling 3,072 | never raised |
| gate stack | common | 384 | worst case ≤ 360 |

When a ledger does not fit, the remedy is slimming or moving code to a
lower tier (fixed to cold through an upcall shim, driver hot to boot/bulk),
never a third bank.

---

## 10. Protection

The Zephyr-80 has no MMU, and KonaOS is trusted and single-tasking. The
nucleus/driver split gives modularity, ownership rules, controlled bank
crossing and ABI discipline, not protection:

- a defect in resident code can corrupt any memory;
- validation happens once, in the nucleus, at the boundary;
- handles prevent accidental coupling to addresses, but they are not
  enforced capabilities;
- a program can write the latch.

Validation, bounds checks and copied metadata stay anyway: they catch
defects and keep the boundaries replaceable.

---

## 11. Portability

KonaOS is meant to run on more than one machine. The split is:

**Zephyr-80 details** (a port reimplements them):

```text
physical bank numbers, the page field P, map-token values
8 KiB slots, the aperture address and size
modes 10 and 11, the 6000h window, RST 30h
the fixed/cold and hot/boot tier split
the IOC transport and the MCU filesystem
```

**Native concepts** (family-wide):

```text
process and primary address space
endpoint, typed handle, protocol, descriptor, namespace provider
extended-memory object, segment, map and unmap
logical sizes
```

Far memory and the aperture are Zephyr-80 extensions: no public interface
requires a port to emulate a Z80 bank, slot or aperture.

---

## 12. Boundaries

**Personalities.** A later personality (CP/M first) consumes native
facilities: it may synthesize drive letters, USER areas, FCBs and 128-byte
records over native paths, and it builds its own page zero (`0000h` warm
boot, `0005h` BDOS, the default FCBs, `0080h` tail, a `0100h` program).
None of that enters the native ABI. Mode 01 is reserved for personalities.

**Not provided:** preemption or multitasking; asynchronous or queued IPC;
driver unloading, restart or revocation; generic marshalling; shared
writable memory between processes; run-time generated executable memory or
dynamic libraries; virtual memory, swap or memory protection; a third OS
bank; run-time mounting, device registration or hardware probing; drive
letters, symbolic links, permissions, pipes or redirection in the native
system.

---

## 13. Glossary

**aperture**: mode 10's `8000h-BFFFh` when P ≠ 0; shows bank 7 − P.

**auxiliary bank**: an application bank divided into 8 KiB slots owned by
processes as extended memory.

**banked call**: a C call to a `__banked` far function through the SDK
trampoline.

**bind**: resolve a namespace name to a handle and its protocol type.

**boot bundle**: the one pointer a driver's init receives: the nucleus
table and its resolved dependencies.

**boot/bulk tier**: bank 6 `0000h-5FFFh`; driver code that runs only at
boot and bulk driver data.

**cold half**: bank 7 `6000h-DFFFh`; nucleus code and data used only while
the window shows bank 7.

**common memory**: `E000h-FFFFh` of bank 0, visible in modes 10 and 11.

**container** (namespace): an enumeration flag meaning further path
components may follow.

**copy path**: how a descriptor copy moves bytes (A, B or C).

**descriptor**: a caller buffer captured by the nucleus at call entry and
named to the driver by a small id.

**driver**: a resident implementation of one or more protocol types, in
bank 6.

**`DRIVER_IO`**: a driver's copy buffers, in bank 6 `8000h-BFFFh`.

**early console**: the polled SIO channel B path used before drivers and
for panics.

**endpoint**: a driver's IPC entry, keyed by `(class, instance)` and
numbered by the host.

**EXE**: the native executable container.

**extended-memory object**: process-owned memory in auxiliary slots,
reached by handle.

**fixed core**: bank 7 `2000h-5FFFh`; nucleus code and data that must stay
visible whatever the window shows.

**frame**: the 32-byte request/reply block of one call.

**handle**: a small process-owned reference to a bound instance.

**hot tier**: bank 6 `6000h-DFFFh`; driver code and data used after boot.

**latch**: the memory-control register at port `00h` (Q, mode, P).

**map token**: the latch value that opens the aperture onto one object or
segment.

**MOD**: the resident driver image format.

**namespace provider**: one of the three nucleus components that own
`/fs`, `/system` and `/dev`.

**nucleus**: the kernel: bank 7 plus its common-memory parts.

**P (page)**: latch bits D5-D7: the ROM page in mode 00, the window or
aperture page otherwise.

**primary bank**: the bank a process runs in.

**protocol type**: the 8-bit type of a bound handle (`FILE`, `CONSOLE`,
...).

**publication**: a host-built `/dev` entry: name, type, endpoint, object.

**Q**: latch bits D0-D2, the SRAM bank.

**segment**: an auxiliary part of an EXE: far code or data.

**startup block**: the versioned block at `0080h` a process receives.

**store**: the ROM recovery store, read by ROMFS and seen as `/system`.

**system file**: `systems/<image>.json`, the description of one image.

**upcall shim**: fixed-core code that lets cold code call a bank-6 driver.

**window**: mode 11's `6000h-DFFFh`; bank 7 when P = 0, bank 6 when P = 1.

---

## 14. Roadmap

Planned work, in order. None of it changes the contracts above unless it
says so.

1. **The direct text renderer.** The bulk-map span and mapped segments
   (5.2, 5.4), the V9958 direct renderer and reported console geometry
   (5.6): the contract above, which the tree implements next.
2. **Mouse and game controllers** in the HID driver: `/dev/input/mouse`,
   `/dev/input/controller/0` and `/1`. They grow the driver tier inside
   its reserve and need nothing from the nucleus. The mouse pointer is a
   sprite, which the console's sprites-off drawing bracket must not hide
   while a mouse is open.
3. **After that:** compatibility personalities, starting with CP/M.

Diagnostics and benchmarks are user programs; the nucleus gains a
diagnostic interface only where a program cannot measure without it.

---

## Appendix A. Far calls with another toolchain

**What is independent of the toolchain.** Far code is an OS and
executable-format contract: an EXE carries up to four far-code segments,
each linked at `8000h` and at most 16 KiB; `SYS_SEG_MAP` returns a
segment's token; the program opens the aperture with it and restores the
previous latch afterwards; `farcall` is an ordinary C function built on
these calls. The kernel, the loader and the format know nothing about
SDCC, and existing programs keep working whatever builds new ones.

**What is specific to SDCC.**

1. The source syntax: `__banked`, and the bank given when the file is
   compiled (`-bo <n>`).
2. The call convention: for a banked call SDCC loads the segment id (the
   `b_<fn>` symbol) into `E` and the address into `HL`, then calls
   `___sdcc_bcall_ehl`. Arguments travel on the stack; the first argument
   is found 5 bytes above the callee's `SP`, so the trampoline places
   exactly three bytes between its entry and the call. Results come back in
   `A`, `DE` and `DEHL`, which the trampoline preserves. The SDK's
   `user/sdk/src/bcall.asm` implements it.
3. The packer's input: `exepack` reads Intel HEX (common to most
   toolchains) and SDCC's map file (specific) for symbol addresses and area
   sizes.

**Porting to another toolchain** needs three pieces of work and no OS
change:

- **An entry adapter:** the new toolchain's banked-call entry, with the
  same four steps as `bcall.asm`: look up the token, open the aperture,
  call, restore. About 30 to 60 bytes. A toolchain without banked calls
  uses `farcall` explicitly.
- **Source portability:** code meant for several compilers wraps `__banked`
  in a project macro.
- **A map reader:** a reader for the new linker's output that returns the
  symbol addresses, the primary image range and each far segment's range
  and size. The rest of the packer is unchanged.

Any toolchain must be able to link a code section at a fixed address
(`8000h`) separately per segment, produce Intel HEX or a raw image with
known bases, and report symbol addresses.
