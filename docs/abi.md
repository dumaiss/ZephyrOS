# ZephyrOS Native Syscall ABI (M2-M6)

This document defines the only stable interface between native ZephyrOS
callers and the kernel.  C names, argument layouts, and SDCC calling
conventions are private implementation details and must never appear in
this interface.  Section 1-6 describe the M1/M2 gate contract; section 7
adds the M2 interrupt-delivery contract, and section 8 adds the M6
native syscall set; section 10 is the M9 native filesystem family
(`SYS_FS_*`, `10h-1Fh`) together with the M9 error values it introduces,
and section 11 is the M9 `SYS_EXEC` with the startup block v1 argument
contract.  M9's end-to-end acceptance (the `m9-selftest` image and the
full matrix) is recorded in
[`docs/validation/2026-10-05-m9-filesystem.md`](./validation/2026-10-05-m9-filesystem.md).

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

Errors use the shared errno values in `abi/zos/errno.h`
(`ZOS_EINVAL`, `ZOS_EFAULT`, `ZOS_ENOSPC`, `ZOS_EBUSY`, ...) as negated
words in `HL`; a bad pointer range is `ZOS_EFAULT`, a bad argument block
or length is `ZOS_EINVAL`.

Native programs use `user/sdk/include/zos/console.h` (`zex_exit`,
`zex_write`, `zex_read`, `zex_poll`, `zex_getinfo`), which wraps these
numbers with the private SDCC convention; programs never call `RST 30h`
directly unless they re-implement that convention.

Implementation note: `SYS_CONSOLE_READ` is the non-blocking drain
described above; an empty drain reports `HL = 0`.  The M6 wording
"blocks for the first byte" predates the shipped drain behavior; the
runtime's blocking `zrt_getc()` polls this call until a byte arrives.

Validation: `make m6-selftest` runs `zextest.zex` from ROMFS in two
application banks and checks the exact exit statuses; the simulator
replays the full image transcript (`tests/test_m6_sim.py`), including
the raw-return fault path and the malformed-ZEX matrix.  The physical
gate is `make smoke-hardware-m6 SERIAL_PORT=/dev/ttyUSB0` with the
transcript expectations pinned in `tools/serial_smoke.py`.

## 9. M7 diagnostic services

M7 adds six recovery/diagnostic services behind the unchanged gate.
They exist for the native monitor (and, later, the M11 diagnostic
suite); ordinary programs keep to the ordinary services and never gain
bank-7 access through them.  The boundary is architectural, not
hardware-enforced (the Z80 has no privilege mode).

```text
28h  SYS_DIAG_MEM_READ     args {bank:u8, reserved:u8, address:u16}
                           -> byte in HL
29h  SYS_DIAG_MEM_WRITE    args {bank:u8, value:u8, address:u16}
2Ah  SYS_DIAG_IO_IN        args {port:u8}        -> byte in HL
2Bh  SYS_DIAG_IO_OUT       args {port:u8, value:u8}
2Ch  SYS_DIAG_BANK_INFO    no args               -> mask:bank in HL
2Dh  SYS_DIAG_SYSTEM_INFO  record written at the argument pointer
                           -> size in HL
```

Argument blocks observe the section 8 window rule (`0100h-1FFFh`); the
load/validation sequence is the same.  Every failure is side-effect
free (no bank content, latch, or process bookkeeping changes) and every
service restores the exact active latch: memory transfers reuse the
checked M6 bank-copy gate, and the port helpers never touch the latch.

Memory access rules:

- `SYS_DIAG_MEM_READ` accepts banks `0-6` and addresses `0000h-DFFFh`
  (page-zero inspection is allowed); `reserved` must be zero.
- `SYS_DIAG_MEM_WRITE` accepts banks `0-6` and addresses
  `0100h-DFFFh`.  The native page zero `0000h-00FFh` and common memory
  are protected.
- Bank `7`, out-of-range addresses, arithmetic wrap, and a nonzero
  reserved field are rejected with `ZOS_EINVAL`.

Port rules:

- `SYS_DIAG_IO_IN` / `SYS_DIAG_IO_OUT` accept ports `10h-FFh` only.
  Ports `00h-0Fh` alias the memory-control latch; an access there would
  be a bank/mode operation in disguise, so `ZOS_EINVAL` is returned
  before anything is executed.

`SYS_DIAG_BANK_INFO` returns `HL = (mask << 8) | bank`:

- `bank` (L) is the calling process' application bank `0-6`; in kernel
  context (no running process) it is the selected latch bank clamped to
  `0-6`;
- `mask` (H) is `7Fh`: bit N set means bank N is a valid application
  bank.  Bit 7 is always clear - bank 7 is kernel-only - so callers
  validate "bank usable" with `(mask >> bank) & 1`.

`SYS_DIAG_SYSTEM_INFO` writes the eight-byte v1 record and returns `8`
in `HL`:

| Offset | Size | Field | Value |
| --- | --- | --- | --- |
| `0` | 1 | `size` | 8 |
| `1` | 1 | `abi` | 1 |
| `2` | 1 | `app_mode` | application mapping mode (`10h`) |
| `3` | 1 | `os_mode` | OS mapping mode (`18h`) |
| `4` | 2 | `common_base` | `E000h` |
| `6` | 1 | `kernel_bank` | 7 |
| `7` | 1 | `romfs_first_page` | 2 |

The packed numbers and block layouts are declared in
`abi/zos/diag.h` (the single public source) and mirrored by the SDK's
`user/sdk/include/zos/diag.h` (`zex_diag_*` gate wrappers in
`user/sdk/src/diaggate.asm`); host tests pin the two headers
and the assembly wrapper constants together.

Validation: `make m7-selftest` boots the monitor image, runs the
kernel-side diagnostic selftest (`tests/target/m7/m7_test.c`) and
replays the scripted interactive session (`tests/test_m7_sim.py`),
including per-class rejections, canary/table stability, and exact
latch/SP/IFF restoration; `make monitor-test` checks the monitor ZEX
layout and its source/map freedom from kernel/serial dependencies.  The
physical gate is `make smoke-hardware-m7 SERIAL_PORT=/dev/ttyUSB0` (see
`docs/monitor.md`).

## 10. M9 native filesystem services

M9 (package 27) exposes the VFS to native programs behind the unchanged
gate.  The family is `10h-1Fh`; argument blocks are declared in
`abi/zos/fs.h` and are packed on the Z80 (every field is pinned
by `_Static_assert` and by host tests).

```text
10h  SYS_FS_OPEN      {path:u16, length:u16, flags:u8}        -> fd in HL
11h  SYS_FS_CLOSE     {fd:u8}
12h  SYS_FS_READ      {fd:u8, buffer:u16, length:u16}         -> count in HL
13h  SYS_FS_WRITE     {fd:u8, buffer:u16, length:u16}         -> count in HL
14h  SYS_FS_SEEK      {fd:u8, whence:u8, offset:i32}          -> position written back
15h  SYS_FS_STAT      {path:u16, length:u16, out:u16}         -> struct zfs_stat out
16h  SYS_FS_OPENDIR   {path:u16, length:u16}                  -> fd in HL
17h  SYS_FS_READDIR   {fd:u8, out:u16}                        -> 1 entry / 0 end
18h  SYS_FS_CLOSEDIR  {fd:u8}
19h  SYS_FS_MKDIR     {path:u16, length:u16}
1Ah  SYS_FS_RMDIR     {path:u16, length:u16}
1Bh  SYS_FS_UNLINK    {path:u16, length:u16}
1Ch  SYS_FS_RENAME    {from:u16, from_length:u16, to:u16, to_length:u16}
1Dh  SYS_FS_CHDIR     {path:u16, length:u16}
1Eh  SYS_FS_GETCWD    {buffer:u16, length:u16}                -> length in HL
1Fh  SYS_FS_SYNC      {fd:u8}
```

Rules:

- Path fields address 1..127 non-NUL bytes in the caller window
  `0100h-1FFFh`; they are copied into a shared static pathname scratch and
  resolved against the calling process' CWD.  A zero length, an over-long
  path, an embedded NUL or a window violation is `ZOS_EINVAL`.
- `buffer`/`out` fields address the user-buffer window `0100h-DFFFh` and
  move only through the checked bank-copy gates.  File data is transferred
  in bounded chunks through the single 512-byte bank-7 staging buffer.
- The descriptor an application names is the global 8-entry VFS pool index
  (`kernel/vfs/`, owner-tagged).  Every fd-taking call verifies range, type
  (`FILE` vs `DIRECTORY`) and owner; a mismatch is `ZOS_EINVAL`, a full pool
  is `ZOS_EMFILE`.  Provider handles are never exposed.  Descriptors are not
  `stdin`/`stdout`, are not inherited across `exec`, and there is no dup.
- CWD is a VFS concept; `CHDIR` stores the canonical path and crosses mounts
  transparently, and `GETCWD` writes the NUL-terminated canonical path and
  returns its length excluding the NUL.  A too-small buffer is `ZOS_EINVAL`.
- `READ`/`WRITE` return the confirmed byte count in `HL`; a failure after at
  least one confirmed chunk returns the confirmed count, otherwise the
  error.  `SEEK` writes the resulting 32-bit position back into the
  `offset` field of the block and returns `0` in `HL`.
- `READDIR` returns `HL = 1` with a `zfs_dirent` at `out`, or `HL = 0` at
  end of stream.
- M9 has no `SYS_FS_STATFS` and no truncate-to-arbitrary-size; truncation to
  zero is `SYS_FS_OPEN` with `ZFS_O_TRUNC`.
- Before the VFS READY phase every `FS_*` call returns `ZOS_EBUSY`; every
  service validates its complete block before any state change.

Errors use the shared values in `abi/zos/errno.h`; M9 adds
`ZOS_EEXIST (10h)`, `ZOS_ENOTDIR (11h)`, `ZOS_EISDIR (12h)`,
`ZOS_ENOTEMPTY (13h)`, `ZOS_EROFS (14h)`, `ZOS_ESTALE (15h)`,
`ZOS_EXDEV (16h)` and `ZOS_EMFILE (17h)`.  `ZOS_EMFILE` reports
descriptor-pool or provider open-handle exhaustion, distinct from
`ZOS_ENOSPC` (storage full).

Native programs use the wrappers in `user/sdk/include/zos/fs.h`
(`zos_open`, `zos_close`, `zos_read`, `zos_write`, `zos_seek`, `zos_stat`,
`zos_opendir`, `zos_readdir`, `zos_closedir`, `zos_mkdir`, `zos_rmdir`,
`zos_unlink`, `zos_rename`, `zos_chdir`, `zos_getcwd`, `zos_sync`).  They
take NUL-terminated strings, reject a string over 127 bytes (or an
empty/null one) locally with `-ZOS_EINVAL`, and return the negated native
error code on failure.

Validation: `make m9-fs-selftest` builds the SIO-only acceptance image
(ROMFS at `/SYSTEM`, the test-only `ramfs` at `/` and `/R2`), runs the
native `fstest.zex` suite and the `fstest_l.zex` second-exec cleanup check,
and replays the whole boot and both programs in the Z80 simulator
(`tests/test_syscall_sim.py`).  The physical gate is
`make smoke-hardware-m9-fs SERIAL_PORT=/dev/ttyUSB0`, which expects
`fstest: PASS`, `fstest: launcher OK` and `m9 fs selftest: OK`.

## 11. M9 native process exec (SYS_EXEC)

```text
05h  SYS_EXEC   args {path:u16, length:u16, args:u16, args_length:u16}
                -> child exit status in HL
```

`SYS_EXEC` (package 28, extended by package 28b) runs a child program to
completion and returns the child's u16 exit status.  It is a thin
composition of the existing process machinery - the same slot, bank, ZEX
loader, page-zero, VFS-hook and exit path the kernel launcher uses - plus
a bounded continuation stack for the shared syscall gate.  There is no
second process manager and no asynchronous spawn: the call is
synchronous, there is no `wait`, and no descriptor or environment is
passed.  A raw command tail is transported for the child.

Rules:

- `path` follows the `FS_*` rules verbatim: 1..127 non-NUL bytes in the
  caller window `0100h-1FFFh`, copied in through the same shared pathname
  scratch and resolved against the calling process' CWD.
- `args` addresses `args_length` raw bytes in the caller buffer window
  `0100h-DFFFh` (no wrap).  The kernel copies them byte-exact into the
  child's startup block and never splits, quotes or interprets them;
  user space owns parsing (`user/sdk/include/zos/process.h`).  `args_length` 0
  means no arguments and `args` is ignored; a length above 125 or a
  pointer outside the window is `ZOS_EINVAL`, checked in the preflight
  before anything is allocated.  An argument block outside `0100h-1FFFh`,
  like a path one, is also `ZOS_EINVAL`.
- The child inherits the parent's CWD and nothing else: the descriptor
  pool is not inherited (a child's fd numbers belong to the child), and
  the parent's open files keep their own offsets and data.  The child's
  `chdir` never affects the parent.
- The child is an ordinary process in a fresh application bank: it is
  never bank 7, it gets its own page zero, and a raw `RET` from its entry
  is a fault reported as an exit status of `ZOS_EFAULT`, exactly like a
  program launched by the kernel.
- Nesting is bounded by `ZOS_EXEC_MAX_DEPTH` (4) user processes in one
  synchronous chain; the kernel launcher is not one of them.  A
  suspension needs at most `ZOS_EXEC_FRAME_SAVE` gate-stack bytes
  (`include/zephyros/process.h`), which the launch gate copies out of the
  single 256-byte common gate stack and restores before the parent's
  `SYS_EXEC` returns.
- Past the suspension point nothing can fail: every allocating or
  fallible step (path copy-in, tail copy, slot/bank allocation, CWD start,
  ZEX load/validate, page zero, startup block) happens in
  `process_prepare()` before the child is entered, with the executable
  source closed before it returns.  `process_launch()` only performs the
  transition.

## 11.1 Startup block v1 and HL

The launch gate enters every native program with `HL = 0080h`, pointing
at its own page zero's startup block v1 (architecture section 26.3):

```text
0080h  u8   version = 1
0081h  u8   tail_length  0..125
0082h  u8   tail[126]    tail_length raw bytes, then one NUL
```

Every program - including the kernel-launched one - gets a valid block;
a program with no arguments sees version 1 and `tail_length` 0.  The
kernel never parses the tail.  The block lives below the `0100h` syscall
buffer window, so a program must copy the tail into its own data before
passing it to a syscall (the runtime `zos_args()` accessor returns the
block, and `zos_split()` parses a copy).  Programs that ignore `HL` are
unaffected.  The entry glue that preserves `HL` is the SDK's crt0
(`user/sdk/src/crt0.asm`, entry symbol `zos_entry`): it stores the block
for `zos_args()` and never modifies it, so the kernel's copy stays valid.

Error mapping (carry set, `A` = `ZOS_E*`, nothing allocated):

| Condition | Error |
| --- | --- |
| missing file | `ZOS_ENOENT` |
| directory | `ZOS_EISDIR` |
| malformed or oversize ZEX | `ZOS_EINVAL` |
| bad argument block, tail over 125 bytes, or tail pointer outside the window | `ZOS_EINVAL` |
| no free slot or bank | `ZOS_ENOSPC` |
| depth limit or frame too large | `ZOS_EBUSY` |
| provider read error | passed through unchanged |

On success carry is clear and `HL` holds the child's exit status; the
result is not a file descriptor and there is no result block.

Native programs use `user/sdk/include/zos/process.h`
(`zos_exec(const char *path, const char *args)`), which takes the
NUL-terminated path and raw tail, rejects a path over 127 bytes or a tail
over 125 bytes (or an empty/null path) locally with `-ZOS_EINVAL`, and
returns the child's status or the negated native error code.  A NULL or
empty `args` means no arguments.

Implementation notes:

- The syscall gate's transition state (`zos_gate_saved_sp/latch/iff`) is
  single-depth in common RAM, so every syscall of the running process
  overwrites it.  The launch gate therefore shadows the launcher's copy
  per depth and the resume puts it back, which is what lets a *resumed*
  parent return through the gate with its own stack pointer, bank and
  interrupt state rather than the child's.
- One continuation record (`saved_sp`, `saved_latch`, `saved_iff`) exists
  per depth, plus `ZOS_EXEC_MAX_DEPTH - 1` gate-frame save areas: the
  outermost process is launched by the kernel from the boot stack and
  needs no copy.  All of it is common-resident
  (`kernel/process/launch.asm`) because the resume gate reads it *while*
  it changes the mapping and switches stacks; the `SYS_EXEC` path buffer
  is bank-7 `_DATA` instead, since it is only ever touched in mode 11.
  Common memory is not an isolated region - mode 10 maps it just as
  mode 11 does - so nothing is placed there for safety.

Validation: `make m9-exec-selftest` builds the SIO-only image that runs
the extended `fstest.zex` `exec` case and the recursive `fsdeep.zex` depth
probe (which must stop at exactly `ZOS_EXEC_MAX_DEPTH` with `ZOS_EBUSY`);
`tests/test_m9_exec_sim.py` replays it and checks the restored gate frame
byte-for-byte at every depth, and `tests/test_m9_exec.py` pins the ABI,
the geometry and the production image.  The physical gate is
`make smoke-hardware-m9-exec SERIAL_PORT=/dev/ttyUSB0`, which observed
`fstest: exec OK`, `fstest: PASS 10/10` and `m9 exec selftest: OK` on the
bench (2026-10-05; `docs/validation/2026-10-05-m9-nested-exec.md`).
