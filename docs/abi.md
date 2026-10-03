# ZephyrOS Native Syscall ABI (M2)

This document defines the only stable interface between native ZephyrOS
callers and the kernel at M1.  C names, argument layouts, and SDCC calling
conventions are private implementation details and must never appear in
this interface.

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
