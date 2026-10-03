# ZephyrOS Native Syscall ABI (M2-M6)

This document defines the only stable interface between native ZephyrOS
callers and the kernel.  C names, argument layouts, and SDCC calling
conventions are private implementation details and must never appear in
this interface.  Section 1-6 describe the M1/M2 gate contract; section 7
adds the M2 interrupt-delivery contract, and section 8 adds the M6
native syscall set.

## 1. Entry

Native entry is the `RST 30h` instruction.  The page-zero vector at
`0030h` is exactly `JP zos_syscall_entry`; `zos_syscall_entry` is the
common-memory gate and the dispatcher lives in bank 7.

```text
RST 30h
```

Entry state:

```text
A  = syscall number
HL = pointer to a syscall-specific argument block
```

M1 restriction: until later staging/copy APIs exist, the argument block
must lie in the preserved caller window `0000h-1FFFh`.  The gate never
reads caller memory above `1FFFh` after entering mode `11`.

## 2. Return

```text
Carry clear = success
Carry set   = error
HL          = primary result
A           = error code when carry is set
```

At M1 the dispatcher returns `A = ZOS_ENOSYS (01h)`, `HL = 0000h`, carry
set for every syscall number.

## 3. Register contract

| Register | Contract |
| --- | --- |
| `A` | error code on error; unspecified on success |
| `HL` | primary result |
| `BC`, `DE`, `IX`, `IY` | preserved |
| `SP` | restored to the caller value |
| Selected bank | restored (bank identity unchanged across the call) |
| Mode | restored to mode `10` |
| Other flags | unspecified |
| Alternate registers | not part of the M1 contract |

## 4. Reentrancy and interrupts

- The syscall gate remains single-entry and non-reentrant; its common
  transition state is single-depth.
- On entry the common gate snapshots IFF2 with `LD A,I` and executes `DI`
  before changing stack or mapping state.  It performs the entire mode
  crossing with maskable interrupts disabled, then restores the caller's
  prior enabled/disabled state immediately before `RET`.
- The M2 IRQ executive uses separate common transition state and separate
  common/bank-7 stacks, so it can never observe a partially changed syscall
  transition.

## 5. How the crossing works

```text
application, mode 10 over bank N
    │ RST 30h        (return PC pushed on the caller stack)
    ▼
0030h: JP zos_syscall_entry        page zero
    ▼
common memory, still mode 10
    ├── save A, snapshot IFF2, and DI
    ├── save caller SP without a push   (the stack may live in 2000h-DFFFh
    │                                   and disappears above 1FFFh in mode 11)
    ├── validate mode 10 and bank 0-6; anything else -> fatal path
    ├── save the full caller latch
    ├── switch SP to the common gate stack
    ├── push BC, DE, IX, IY
    ├── write 18h|N to port 00h        (only the mode bits change)
    ▼
mode 11, bank 7 visible at 2000h-DFFFh
    └── zos_syscall_dispatch (bank-7 boundary -> internal C dispatcher)
    ▼
return path (common memory):
    ├── stash HL/A/carry in common state
    ├── pop BC, DE, IX, IY
    ├── write 10h|N to port 00h
    ├── restore the exact caller SP
    ├── rebuild A, HL, carry
    ├── restore the caller's prior IFF state
    └── RET                             (pops the caller's RST-pushed PC)
```

## 6. Validation

- Host: vector bytes/address/target checks, common placement of the full
  gate path, bank-7 placement of the dispatcher, absence of direct
  C-symbol addresses in the vector (`make test`, `make check`), plus a
  Z80 simulation that replays the real image bytes for banks 0 and 6.
- Target: `make syscall-selftest` builds an image whose smoke caller runs
  in application banks 0 and 6, exercises the whole contract from a stack
  above `2000h`, and prints `syscall selftest: OK` (or a precise failure).
  Capture and verify it on hardware with
  `make smoke-hardware-selftest SERIAL_PORT=/dev/ttyUSB0`.
- The dated acceptance record is `docs/validation/`.

## 7. M2 interrupt delivery contract

M2 adds a kernel-owned IM2 interrupt executive that is transparent to
interrupted code.  This is not a syscall interface, but it is a documented
kernel contract because an interrupt may be delivered at any instruction
boundary once the foreground enables maskable interrupts:

- Entry: IM2 vector `I=FDh`, vector page `FD00h-FDFFh` plus the `FE00h`
  guard byte.  Every enabled vector resolves to common-memory entry code;
  vector `FFh` resolves to `irq_ff_unexpected` (build-verified identity
  `word_at(FDFFh) == address_of(irq_ff_unexpected)`).
- Preserved across delivery: `AF BC DE HL IX IY`, alternate
  `AF' BC' DE' HL'`, `SP`, the selected bank and mode (exact port-`00h`
  latch restored), and `PC` (via `RETI`).  The `I` register stays `FDh`.
- Entry from mode `10` crosses to `18h|saved_bank`; entry from mode `11`
  retains the mapping.  The executive runs handlers on the bank-7 ISR
  stack (`DF00h-DFFFh`) in mode `11`, with maskable interrupts disabled
  for the entire delivery; there is no nesting.  The executive owns the
  final `RETI`.
- Handlers are bank-7 code registered per logical source
  (`irq_claim`/`irq_release`); unexpected or unclaimed interrupts take a
  compact counter/source record and, for CTC0, a safe quiesce.
- The `irq-selftest` image exercises mode-10 delivery (banks 0 and 6,
  stack above `2000h`), mode-11 delivery, repeated delivery, exact `AF`
  restoration, the unclaimed path, and the syscall gate's IFF restore
  (`make irq-selftest`, then `make smoke-hardware-irq SERIAL_PORT=...`).

## 8. M6 native syscalls

M6 implements the first real services behind the unchanged gate.  The
entry/return contract of sections 1-3 applies verbatim; only the set of
syscall numbers and their argument blocks is added.

```text
00h  SYS_EXIT            args {status:u16}
01h  SYS_CONSOLE_WRITE   args {buffer:u16, length:u16}
02h  SYS_CONSOLE_READ    args {buffer:u16, length:u16}
03h  SYS_CONSOLE_POLL    no args
04h  SYS_GETINFO         result block written at the argument pointer
```

All argument blocks must lie inside the caller window `0100h-1FFFh`
(checked before any caller byte is read); console/data buffers may span
`0100h-DFFFh`.  Unimplemented numbers return `ZOS_ENOSYS (01h)` with
carry set.

Per-syscall behavior:

- `SYS_EXIT` does not return.  The kernel records the u16 status,
  releases the process's bank and slot, and resumes the suspended parent
  (initially the kernel launcher) after its `exec` call.  An argument
  block outside the caller window is treated as a fatal user fault:
  the process exits with status `ZOS_EFAULT`.  A raw `RET` from a
  program entry behaves the same way - the common return trap turns it
  into an exit with `ZOS_EFAULT`, so a process can never unwind into
  the kernel or a previous process.
- `SYS_CONSOLE_WRITE` copies from the caller bank through the common
  staging block in bounded chunks and writes through the normal console
  service (the registered dynamic SIO driver; the early console is
  reserved for panic/fatal paths).  `HL` returns the number of bytes
  written; a partial count or a negative errno is possible.
- `SYS_CONSOLE_READ` is non-blocking with an availability gate: it
  returns `0` immediately when no byte is pending; after the first byte
  arrives it returns as soon as no further byte is immediately
  available, up to `length`.  Received bytes are staged through the
  common buffer and copied back into the caller bank.  `HL` returns the
  byte count or a negative errno.
- `SYS_CONSOLE_POLL` returns `0` (no input) or `1` in `HL`; a negative
  value reports the poll errno through carry.
- `SYS_GETINFO` writes a six-byte versioned record at the argument
  pointer and returns `6` in `HL`:

  | Offset | Size | Field |
  | --- | --- | --- |
  | `0` | 1 | `size` = 6 |
  | `1` | 1 | `abi` = 1 |
  | `2` | 1 | `pid` (process slot `0-6`) |
  | `3` | 1 | `bank` (application bank `0-6`) |
  | `4` | 2 | `flags`: bit 0 `PROCESS_INFO_FLAG_KERNEL_PARENT` |

Errors use the shared errno values in `include/zephyros/errno.h`
(`ZOS_EINVAL`, `ZOS_EFAULT`, `ZOS_ENOSPC`, `ZOS_EBUSY`, ...) as negated
words in `HL`; a bad pointer range is `ZOS_EFAULT`, a bad argument block
or length is `ZOS_EINVAL`.

Native programs use `user/runtime/zex_runtime.h` (`zex_exit`,
`zex_write`, `zex_read`, `zex_poll`, `zex_getinfo`), which wraps these
numbers with the private SDCC convention; programs never call `RST 30h`
directly unless they re-implement that convention.

Validation: `make m6-selftest` runs `zextest.zex` from ROMFS in two
application banks and checks the exact exit statuses; the simulator
replays the full image transcript (`tests/test_m6_sim.py`), including
the raw-return fault path and the malformed-ZEX matrix.  The physical
gate is `make smoke-hardware-m6 SERIAL_PORT=/dev/ttyUSB0` with the
transcript expectations pinned in `tools/serial_smoke.py`.
