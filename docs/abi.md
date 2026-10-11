# ZephyrOS Native Syscall ABI (M2-M6)

This document defines the only stable interface between native ZephyrOS
callers and the kernel.  C names, argument layouts, and SDCC calling
conventions are private implementation details and must never appear in
this interface.  Section 1-6 describe the M1/M2 gate contract; section 7
adds the M2 interrupt-delivery contract, and section 8 adds the M6
native syscall set; section 10 is the M9 native filesystem family
(`SYS_FS_*`, `10h-1Fh`) together with the M9 error values it introduces,
and section 11 is the M9 `SYS_EXEC` with the startup block v1 argument
contract; section 12 is the M10 resident-service IPC pair, and section
12.1 the M10 object and namespace family that replaces it - the
`namespace-selftest` acceptance image builds that family now (work
package 42a) and production switches over in 42b; section 14 is the M10
extended-memory family (`SYS_XMEM_*`, `20h-25h`), its map tokens and the
buffer-identity rule (work package 43).
M9's end-to-end acceptance (the `m9-selftest` image and the
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
    ├── write (N & 07h) | 18h to port 00h   (bank and mode bits only, so
    │                                       the revision-13 page field is 0)
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
- Target: `make subsystem-selftest` builds the current boot path with the
  smoke caller linked in; it runs in application banks 0 and 6, exercises
  the whole contract from a stack above `2000h`, and prints
  `syscall selftest: OK` (or a precise failure).  Capture and verify it on
  hardware with `make smoke-hardware-subsystem SERIAL_PORT=/dev/ttyUSB0`
  (the per-milestone `syscall-selftest` image was retired).
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
  keeps the interrupted bank and mode.  Either way the gate normalizes the
  revision-13 page field: it dispatches with `P = 0` (the full bank-7
  decode) and restores the saved latch exactly on exit, so an interrupted
  window call (architecture 36.10) keeps its page selection.  The
  executive runs handlers on the bank-7 ISR stack (`5F00h-5FFFh`, in the
  fixed core) in mode `11`, with maskable interrupts disabled for the
  entire delivery; there is no nesting.  The executive owns the final
  `RETI`.
- Handlers are bank-7 code registered per logical source
  (`irq_claim`/`irq_release`); unexpected or unclaimed interrupts take a
  compact counter/source record and, for CTC0, a safe quiesce.
- The subsystem acceptance image exercises mode-10 delivery (banks 0 and
  6, stack above `2000h`), mode-11 delivery, repeated delivery, exact `AF`
  restoration, the unclaimed path, and the syscall gate's IFF restore
  (`make subsystem-selftest`, then
  `make smoke-hardware-subsystem SERIAL_PORT=...`; the per-milestone
  `irq-selftest` image was retired).

## 8. M6 native syscalls

M6 implements the first real services behind the unchanged gate.  The
entry/return contract of sections 1-3 applies verbatim; only the set of
syscall numbers and their argument blocks is added.  Package 41 retired
the three console numbers (see below); `SYS_EXIT` and `SYS_GETINFO`
remain as they shipped.

```text
00h  SYS_EXIT            args {status:u16}
01h  reserved  (was SYS_CONSOLE_WRITE)
02h  reserved  (was SYS_CONSOLE_READ)
03h  reserved  (was SYS_CONSOLE_POLL)
04h  SYS_GETINFO         result block written at the argument pointer
```

All argument blocks must lie inside the caller window `0100h-1FFFh`
(checked before any caller byte is read); data buffers may span
`0100h-DFFFh`.  Unimplemented numbers return `ZOS_ENOSYS (01h)` with
carry set.

Work package 41 removed the three console syscalls: a program's console
I/O is a client of the console service now (section 13 below), and the
numbers stay reserved - a stale caller gets `ZOS_ENOSYS` instead of
writing somewhere unexpected.

Per-syscall behavior:

- `SYS_EXIT` does not return.  The kernel records the u16 status,
  releases the process's bank and slot, and resumes the suspended parent
  (initially the kernel launcher) after its `exec` call.  An argument
  block outside the caller window is treated as a fatal user fault:
  the process exits with status `ZOS_EFAULT`.  A raw `RET` from a
  program entry behaves the same way - the common return trap turns it
  into an exit with `ZOS_EFAULT`, so a process can never unwind into
  the kernel or a previous process.
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
`zex_getinfo` and the console functions), which wraps these numbers and
the console service protocol with the private SDCC convention; programs
never call `RST 30h` directly unless they re-implement that convention.

Validation: `make subsystem-selftest` runs `zextest.zex` from ROMFS in two
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

Validation: `make subsystem-selftest` boots the monitor image, runs the
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

- Path fields address 1..127 non-NUL bytes in the caller buffer window
  `0100h-DFFFh`; they are copied into a shared static pathname scratch and
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

Validation: `make fs-selftest` builds the M10 outcome image (the
filesystem driver serving `/FS`, ROMFS at `/SYSTEM`) and replays it in the
Z80 simulator with the host FS2 peer serving the card
(`tests/test_fs_selftest_sim.py`): every outcome line, the pinned check
count, the card-absent run and the handle-release run.  The physical gate
is `make smoke-hardware-fs SERIAL_PORT=/dev/ttyUSB0`, which expects
`fs-selftest: PASS` and the monitor's `Z80> ` prompt.

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

- `path` follows the namespace rules verbatim: 1..127 non-NUL bytes in
  the caller buffer window `0100h-DFFFh`, copied in through the same
  shared pathname scratch and resolved against the calling process' CWD.
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

Validation: the geometry is pinned by `include/zephyros/process.h`
(`ZOS_EXEC_MAX_DEPTH`, `ZOS_EXEC_FRAME_SAVE` with its static asserts) and
by `kernel/process/launch.asm`; the nested exec runs in the M10 outcome
image (`make fs-selftest`), where the boot program `SYS_EXEC`s
`/SYSTEM/fs_child.zex` and checks the child's tail, stdout, exit status
and the release of the handles it leaked
(`tests/test_fs_selftest_sim.py`).  The physical gate is
`make smoke-hardware-fs SERIAL_PORT=/dev/ttyUSB0`.

## 12. M10 native IPC (SYS_IPC_LOOKUP, SYS_IPC_CALL)

```text
06h  SYS_IPC_LOOKUP  args {class:u8, instance:u8, version:u8, reserved:u8}
                     -> HL = handle (1..N) with version filled,
                        HL = 0 when no endpoint is registered
07h  SYS_IPC_CALL    args {handle:u8, op:u8, flags:u8, frame_length:u8,
                           buffer:u16, length:u16, direction:u8,
                           reserved:u8, frame:32, status:i16}
                     -> HL = the IPC status (the same value as `status`)
```

The resident-service substrate (architecture sections 36.7-36.12) is the
nucleus mechanism behind these two numbers: services export *endpoints*
identified by a (class, instance) pair, a client holds an opaque handle
returned by the lookup, and a call is one bounded 32-byte frame copied in
at entry and back on reply.  The numbers, the frame layout, the argument
blocks and the descriptor conventions are pinned in `abi/zos/ipc.h`, which
static-asserts every offset.

Both blocks live in the caller *window*, `0100h-1FFFh`: the P-normalizing
gate leaves only that part of the caller's bank visible while the nucleus
runs, so a block anywhere else (including the caller's own data above
`2000h`) is refused with carry set and `A = ZOS_EINVAL` before the IPC
layer is entered.  A program marshals through a low-window scratch; libzos
exports `zos_scratch` plus its own `zos_ipc_lookup_block`,
`zos_ipc_call_block` and `zos_ipc_raw_req` (docs/sdk.md).

Result conventions:

- `SYS_IPC_LOOKUP` returns the handle in `HL` with carry clear; `HL = 0`
  means "not registered" and `args->version` is filled on success.
  A block outside the window is a carried `ZOS_EINVAL`.
- `SYS_IPC_CALL` returns the IPC *status* in `HL` with carry clear.  The
  status is the service's own result (0 or positive), or a negated
  `ZOS_E*` for an IPC-layer refusal: unknown or stale handle
  (`ZOS_EINVAL`), forged or released descriptor id (`ZOS_EINVAL`),
  `offset + length` past the captured descriptor (`ZOS_EINVAL`),
  re-entrant call into a non-re-entrant endpoint (`ZOS_EBUSY`), nesting
  deeper than `ZOS_IPC_MAX_NESTING` (`ZOS_ENOSPC`).  The reply frame, the
  captured descriptor id in `frame.desc` and `status` are written back
  into the block, so a caller that only reads `status` sees the refusal
  too.  A carried `ZOS_EINVAL` means the request never reached the IPC
  layer: block outside the window, `frame_length` 0 or above 32, or a
  non-zero `reserved` byte.
- The IPC status is a *protocol* result, not a syscall result: a service
  returning a negative value is a normal reply, exactly like a diagnostic
  or filesystem service error, and carry stays clear.

Descriptor convention (one optional caller buffer per call, named by
`buffer`/`length`/`direction`):

- `direction` is `ZOS_IPC_DESC_IN` (caller supplies, 0) or
  `ZOS_IPC_DESC_OUT` (caller receives, 1).  A zero `length` means "no
  buffer" and `frame.desc` is `ZOS_IPC_DESC_NONE` (`FFh`).
- The nucleus captures the buffer at entry from the gate's saved caller
  latch (bank and page), bounds-checks it against
  `ZOS_USER_BUFFER_BASE`/`ZOS_USER_BUFFER_END` (the caller's own bank,
  `0100h-DFFFh`), the common window (`E000h-FFFFh`, which every mapping
  shows at the same address) and `ZOS_IPC_DESC_MAX` (4096), and hands the
  service a small id.  The caller's *raw address is never visible to a
  service*: services reach the buffer only through the restricted copy
  calls (`copy_in`/`copy_out`/`move`), and every use re-checks
  `offset + length` against the captured length.
- A descriptor id is valid only for calls deeper than its capture level,
  so a caller's ids stay live down the call chain while a service can
  never name its own level, another level's id, or a forged one.
- With a non-zero saved page, a buffer wholly inside the mode-10 aperture
  (`8000h-BFFFh`) names the far bank `7 - P` (architecture 36.17); a
  buffer that would cross the aperture boundary is refused at capture.

Copy paths (correction 01 to architecture 36.8).  The nucleus picks the
mapping per request; every accepted descriptor is copied correctly on all
three paths, and only the first two are fast:

| Path | Condition | Mechanism |
|------|-----------|-----------|
| A | caller buffer in `0000h-1FFFh` or in the common window | one `LDIR` in mode 11 |
| B | caller buffer in the caller's bank, clear of `8000h-BFFFh`, service buffer in bank 6's `8000h-BFFFh` | one `LDIR` in mode 10 over the caller's bank with page 1: the aperture shows bank 6 at the service's own address. Exact latch restore, interruptible (a delivery inside the window pushes onto a common reservation, and the IRQ gate restores the interrupted mode-10 latch) |
| C | everything else, including a service buffer outside `8000h-BFFFh` | chunked through the common application-bank copy gate; counted as slow |

Path B's page 1 is nucleus-internal: an application never receives a
latch for bank 6, and the primitive's request block, saved latch/SP and
interrupt-push reservation are all common-resident, because mode 10 does
not map bank 7.  A service that is the source or target of
`copy_in`/`copy_out` should therefore place its I/O buffer, cache or
staging block in bank 6's `8000h-BFFFh`; one that does not still works,
on path C, and the nucleus counts it (the three path counters are read
out of the image by the acceptance replay).

Nesting and re-entrancy: `ZOS_IPC_MAX_NESTING` is 4 (the outermost call
plus three nested levels).  An endpoint registered with
`ZOS_IPC_EP_REENTRANT` may be re-entered; every other endpoint refuses a
call that arrives while it is active with `ZOS_EBUSY`.  A service executes
on the fixed service stack (`ZOS_SERVICE_STACK_BASE`, 512 bytes) with the
service page mapped, so its code, its data and its stack are all fixed or
bank 6; the nucleus keeps its own stack, the common gate stack and the
IRQ vectoring.  An IRQ delivered while a call is in progress runs the
service handler through the restricted API (`ipc_irq_active`; no register,
lookup, call or copy) on the ISR stack and restores the interrupted latch
exactly, so the call's frame and mapping survive.

Native programs use the wrappers in `user/sdk/include/zos/ipc.h`:
`zos_ipc_lookup(class, instance)`, `zos_ipc_lookup_version(...)`,
`zos_ipc_call(handle, struct zos_ipc_request *)` and the raw
`zos_ipc_raw_call(struct zos_ipc_call_req *)` the tests drive directly.
The lookup wrapper caches up to `ZOS_IPC_CACHE_ENTRIES` (8) (class,
instance, handle) triples, so a repeated discovery costs no syscall; the
cache is an optimisation only, never a correctness dependency (the kernel
owns the endpoint table and handles are permanent).

Package 41 moved the substrate from `test_link` into production: the
fixed core links it (2,562 bytes of code and data), the cold half links
the syscall bodies and the resident service loader, and every production
image boots its service set from the store's `services.man`.  The
`ZOS_TARGET_TEST_BUILD` routing seam in `kernel/syscall.c` is gone - the
IPC pair is dispatched like the filesystem family, in
`kernel/syscall/ipc.c`.

Validation: `make ipc-selftest` builds the production boot path with the
substrate linked into the fixed core, the six synthetic bank-6 services
placed by the cold loader and the SDK *probe* program booting as
`/SYSTEM/ipccopy.zex`.  The probe names five caller-buffer addresses
(`0100h`, `3000h`, `7000h`, `C000h`, `9000h`) in the chained 512-byte read
and a single `copy_out`, runs the service-side slow case and a path-B copy
with the timer armed, then starts the conformance client
(`/SYSTEM/ipc.zex`) and folds its 55-check verdict into its own line
(`ipccopy: PASS`).  `tests/test_ipc_selftest_sim.py` boots the image once
and asserts the outcomes: both verdicts and their check counts, every
phase marker on the wire, the resolver's copy-path counters (one per
outcome, so every path provably carried bytes), the reserved-stack
high-water marks, the console bytes of the service-side slow case, the
loader's placement report against the packer's plan, the one-shot boot
segments, and the caller's mapping restored at the end of the run.
Work package 42d replaced the per-instruction tracer that measured
per-case T-states, latch installs and the CTC0 injection framing this way:
those figures are in the package-42b and package-42e records, and the
client and the probe verify every path from the inside before printing
their verdicts.  `tests/test_ipc_selftest.py` pins the ABI numbers, the
residency classification, the client- and probe-window layouts and the
service I/O area rule.  The production image links none of
it before package 41, so it stayed byte-identical until then
(`config/residency.json` classified the objects as `test_link`).

## 12.1 M10 objects and the namespace (package 42a)

The stage-3 lookup disappears with the milestone's typed-namespace model
(architecture 36.13/36.14): a program names what it wants, `bind` returns
a *typed handle*, and endpoint numbers stay private to the nucleus.  The
family is pinned in `abi/zos/object.h` (every offset static-asserted):

```text
06h  SYS_BIND     args {path:u16, length:u16, flags:u16,
                         type:u8 out, reserved:u8 out}
                  -> HL = handle (>= 3) with the 8-bit protocol type
                     written to `type`; negative -ZOS_E* on refusal
07h  SYS_CALL     args {handle:u8, op:u8, flags:u8, frame_length:u8,
                         buffer:u16, length:u16, direction:u8,
                         reserved:u8, frame:32, status:i16}
                  -> HL = the protocol status (the same value as `status`)
08h  SYS_UNBIND   args {handle:u8}
                  -> HL = 0, or -ZOS_EINVAL for a handle the caller
                     does not own, a standard handle or an unbound number
09h  SYS_NS_ENUM  args {path:u16, length:u16, cookie:u16 in/out, out:u16,
                         out_size:u16, count:u16 out}
                  -> HL = entries written; `cookie` is the next start index
```

`07h` is the stage-3 call block, unchanged: the frame, the descriptor
conventions, the copy paths and the nesting rules of section 12 above
apply to a handle exactly as they did to a looked-up endpoint.  What
changes is only what a handle *is*: one global, owner-tagged table (the
extended M9 descriptor pool, `VFS_FD_MAX` entries), where each entry
records the owner PID, the 8-bit protocol type, the endpoint (a bank-6
service endpoint or a nucleus one) and the provider's own object identity.
`bind` never returns `ZOS_HANDLE_STDOUT` (1) or `ZOS_HANDLE_STDIN` (2);
those two are the *standard handles*, bound at process start to the
`/dev/console` publication when the build has one, and re-created for a
child by `SYS_EXEC`, which inherits no bound handle.  Process exit unbinds
every handle the process owns.

Paths are resolved by the root router: `bind`/`ns_enum` normalize the
caller's path (`vfs_path_normalize_at`, relative to the process CWD) and
route the first component, case-insensitively, to one of the three static
nucleus providers - `/FS` (the filesystem driver; `bind` below it returns
`-ZOS_ENODEV` until package 42b), `/SYSTEM` (ROMFS, usable before any
driver) and `/dev` (the host-built publication table).  A provider owns
everything below its root and the VFS does not interpret it.  `/` is a
directory of those three roots, not a provider: a root-level name has no
owner and is refused with `-ZOS_ENOENT`.  There is no mount table, no
provider registration and no root merge.

`ns_enum` returns `ZOS_NS_ENTRY_SIZE` (28) bytes per entry - `type`,
`flags`, `name_length`, `reserved` and a NUL-terminated, zero-padded
24-byte `name` - starting at the index in `cookie`; the cookie is written
back so a caller pages with one entry at a time, and the container flag
`ZOS_NS_ENTRY_CONTAINER` marks a name that is a grouping level, not a
bindable object (`/dev/input`, `/dev/serial`).

A ROMFS bind returns the type `ZOS_TYPE_FILE` and an in-process FILE
handle served by a nucleus endpoint through the same call path as a bank-6
endpoint, with the one ROMFS reader (`ZFILE_OP_READ`, `ZFILE_OP_SEEK`,
`ZFILE_OP_META`; `ZFILE_OP_WRITE` is `-ZOS_EROFS`).  `/dev` names come from
the publications the packer resolves into each service container and the
loader installs (docs/devices.md, docs/module-format.md).

**Build selection.**  `abi/zos/object.h` defaults `ZOS_NS_HANDLES` to 0:
production keeps `SYS_IPC_LOOKUP`/`SYS_IPC_CALL` on 06h/07h, links none of
the namespace objects, and is byte-for-byte the stage-3 image.  The macro
is 1 in the `namespace-selftest` acceptance image (`make ns-selftest`,
work package 42a correction 01), where `kernel/syscall.c` routes 06h-09h
to the object dispatcher, the SDK is built with the bind/unbind/ns_enum
wrappers and the standard handles, and the M9 `FS_*` front end still
served `/SYSTEM` for the legacy path.  Packages 42b/42c then turned the
default over and deleted the M9 machinery: the mount table, the provider
ABI, the `FS_*` front end, the arena and the driver ABI are gone
(`docs/validation/2026-10-09-m10-namespace-handles.md`,
`docs/validation/2026-10-09-m10-prune-nucleus.md`), and SIO plus the
console service are the image's service set.

Validation: the `namespace-selftest` image boots SIO, the console service
and a synthetic publisher whose manifest declares `/dev/input/keyboard`
and `/dev/video`, then an ordinary SDK client (`/SYSTEM/ns.zex`) that
binds `/dev` objects and their grouping names, calls the publisher through
the handle, reads and seeks `/SYSTEM/services.man`, exhausts and releases
the handle table, pages the three namespaces and `SYS_EXEC`s a child that
proves the inherited standard handles.  `tests/test_ns_selftest.py` pins
the ABI, the container publication tables and the residency
classification; `tests/test_ns_selftest_sim.py` replays the boot and
compares the transcript with the packed store.

## 13. M10 console client (no syscall)


A program's console I/O is not a syscall any more: `user/sdk/src/console_service.c`
speaks the protocol in `abi/zos/protocol.h` through the standard handles
every process starts with (work packages 41-42b).

| SDK function | console service operation | descriptor |
| --- | --- | --- |
| `zex_write(buf, len)` | `ZCONSOLE_OP_WRITE` | IN: the caller's buffer |
| `zex_read(buf, len)` | `ZCONSOLE_OP_READ` (non-blocking drain) | OUT: the caller's buffer |
| `zex_poll()` | `ZCONSOLE_OP_POLL` | none |
| `zex_putc(ch)` (used by `zos_putc`) | `ZCONSOLE_OP_PUTC` | none |

The handle is looked up once and cached; a program whose store lists no
console service sees `-ZOS_ENOENT` from every one of them.  `zos_puts`,
`zos_console_write`, `zos_getc` and `zos_read_line` are unchanged on top
of these four.  The early console and the panic path remain nucleus-only
and syscall-free: a boot with no service at all still reports.

The nucleus binds `/dev/console` to handle 1 (stdout) and
handle 2 (stdin) at process start, an exec'd child receives its own pair,
and the console client needs no lookup at all.  Which console driver
answers the handle is the build's selection (work package 42e:
`console-v9958-hid` in production, `console-serial` in the serial
variant); the program cannot tell the difference.

---

## 14. M10 extended memory and the far aperture (package 43)

The application bank map (architecture 36.15) turns the six application
banks into free, primary (one process's bank) or auxiliary memory; an
auxiliary bank carries seven 8 KiB slots at `0000h-DFFFh`.  A process
allocates *extended-memory objects* out of the auxiliary slots and reaches
them by copy or through the mode-10 far aperture (36.16/36.17).  The
family is pinned in `abi/zos/xmem.h` (every offset static-asserted):

```text
20h  SYS_XMEM_ALLOC  args {size:u16, flags:u16}       -> HL = handle (>= 1)
21h  SYS_XMEM_FREE   args {handle:u16}                -> HL = 0
22h  SYS_XMEM_READ   args {handle:u16, offset:u16,
                           buffer:u16, length:u16}    -> HL = bytes moved
23h  SYS_XMEM_WRITE  (same block as read)             -> HL = bytes moved
24h  SYS_XMEM_INFO   args {handle:u16, buffer:u16 out}-> 8-byte record
25h  SYS_XMEM_MAP    args {handle:u16, address:u16 out}
                      -> HL = the aperture token, `address` = the object's
                         address in the aperture
```

`flags` is `ZOS_XMEM_FLAG_MAPPABLE` (at most 16 KiB, placed in the
aperture slice so `SYS_XMEM_MAP` can open it) or `ZOS_XMEM_FLAG_NONE`;
any other bit is `-ZOS_EINVAL`, as are a zero size and a mappable size over
the limit.  A size over the 32 KiB maximum object, an exhausted object
table or a free region without a fitting run is `-ZOS_ENOSPC`.  The size
rounds up to whole 8 KiB slots and one object is contiguous inside one
bank; placement is never visible through the API.

**Contents are undefined**: nothing pre-zeroes an object (36.19), so a
program that needs zeroes writes them.  Every range - `offset + length`
against the allocated size, the buffer against the caller window - is
checked before any byte moves, and a copy-gate failure is `-ZOS_EFAULT`.
`SYS_XMEM_INFO` returns `requested`, `allocated`, `flags` and `type`
(`ZOS_XMEM_TYPE_RUNTIME`), never a bank or slot.

**Map tokens.**  In mode 10 with a nonzero page field P, the caller's
`8000h-BFFFh` names bank 7 - P (36.17).  `SYS_XMEM_MAP` returns the
*execution-latch value* that opens that aperture onto the object together
with the object's aperture address (`8000h` in the lower slice, `A000h` in
the upper one).  The SDK helpers are register-only and take no syscall:
`zos_aperture_open(token)` writes the token to the memory-control port and
returns the previous latch, `zos_aperture_close(saved)` writes that latch
back.  Only the caller's own mappable objects can be mapped - a stale,
foreign or non-mappable handle is `-ZOS_EINVAL` - and every object bank
maps to a page field of 2..7, so no token ever names bank 6 or bank 7.

**Buffer identity** (36.8).  A caller address in `8000h-BFFFh` while the
caller's saved latch has a nonzero page field names the far bank, not the
primary bank: `read`/`write` buffers resolve that way, and every other
syscall that moves caller bytes - descriptor capture included - applies
the same rule.  A buffer that would cross the aperture boundary is refused
with `-ZOS_EINVAL` before anything is copied.

**Handles and lifetime.**  A handle is opaque, owner-tagged and
generation-counted: a freed and reused entry hands out a different
handle, so a stale or foreign handle is refused before any state changes.
A process owns its objects; a child inherits none of them, and `SYS_EXIT`
releases every object of the exiting process after its handles are
unbound (36.13).  Objects are read/write and non-executable.
