# KonaOS Native Syscall ABI

The interface between native programs and the nucleus. C names, layouts
inside the kernel and SDCC calling conventions are not part of it. Every
argument block below is declared in `abi/kos/` and pinned there with
static assertions; programs normally use the SDK wrappers (`docs/sdk.md`)
instead of calling the gate directly.

## 1. Entry and return

```text
entry   RST 30h        (page-zero vector at 0030h: JP kos_syscall_entry)
        A  = syscall number
        HL = argument block

return  carry clear: success, HL = result
        carry set:   failure, A = KOS_E* error code
```

| register | contract |
|---|---|
| `A` | the error code on failure; unspecified on success |
| `HL` | the result |
| `BC`, `DE`, `IX`, `IY` | preserved |
| `SP` | restored |
| the latch | restored exactly, bank and page included |
| other flags, alternate registers | unspecified |

**Argument blocks** must lie in the caller window `0100h-1FFFh`, which
stays visible while the nucleus runs; a block anywhere else is refused with
carry set and `KOS_EINVAL` before any of it is read. **Data buffers** named
inside a block may lie anywhere in `0100h-DFFFh` of the caller's bank.
Unassigned numbers return `KOS_ENOSYS`.

## 2. How the gate crosses

```text
program, mode 10 over bank N
  RST 30h                        the return address is pushed on the caller's stack
  JP kos_syscall_entry           page zero
common memory, still mode 10:
  save A, snapshot IFF2, DI
  save the caller SP without a push   (the caller's stack may be in 2000h-DFFFh,
                                       which disappears in mode 11)
  check mode 10 and a bank 0-6; anything else is fatal
  save the whole caller latch
  switch SP to the common gate stack; push BC, DE, IX, IY
  write (N & 07h) | 18h              mode 11, page field cleared
  EI when the caller had interrupts on
mode 11: kos_syscall_dispatch         the body runs with the caller's state
return, in common memory:
  DI                            masked again before the gate's own state
  stage HL, A and carry; pop BC, DE, IX, IY
  write the saved latch back         (bank and page restored exactly)
  restore the caller SP; rebuild A, HL, carry
  restore the caller's interrupt state
  RET
```

The gate is single-entry: one syscall is in flight at a time. Every step
that touches the crossing's own state - the saved registers, the stack
switch, the mapping, the result cells - runs masked; the **body** runs
with the caller's interrupt state, because a masked body would hold the
CTC's one pending request until its next tick was already lost. Masking
inside a body is therefore explicit and bounded: the copy primitives, ROM
access, the IOC frame exchange and bulk receive (architecture 4.5). The
IRQ executive keeps its own state and stacks, so it never sees a
half-made crossing, and a delivery taken anywhere in the body - on the
gate stack, in nucleus code or in a driver call - saves into the IRQ
transition reservation and restores the exact latch.

## 3. Interrupt delivery

Interrupts are not a syscall interface, but they can arrive at any
instruction boundary once a program enables them, so their contract is
part of the ABI:

- IM2 with `I = FDh`; vector page `FD00h-FDFFh` plus the guard byte at
  `FE00h`; vector `FFh` resolves to `irq_ff_unexpected`.
- Preserved across delivery: `AF BC DE HL IX IY`, the alternate set, `SP`,
  the exact latch (bank, mode and page) and `PC` (through `RETI`). `I`
  stays `FDh`.
- Delivery from mode 10 enters mode 11 over the interrupted bank; from mode
  11 it keeps that mode. Either way it restores the saved latch exactly on
  exit, so an interrupted aperture or window is unchanged.
- Handlers run on the ISR stack with interrupts masked; there is no
  nesting. The executive executes the final `RETI`.
- A syscall **body** runs with the caller's interrupt state (section 2),
  so a handler can arrive inside one: it is reentrant with respect to the
  gate because it keeps its own state and stacks and restores the exact
  latch. The registry it dispatches through is mutated under
  `irq_arch_lock`, so a claim or release from a body cannot be seen
  half-made.

## 4. Syscalls

| number | name | block (in) | result |
|---|---|---|---|
| `00h` | `EXIT` | `{status:u16}` | does not return |
| `04h` | `GETINFO` | record at the block | `HL` = 6 |
| `05h` | `EXEC` | `{path:u16, length:u16, args:u16, args_length:u16}` | `HL` = the child's exit status |
| `06h` | `BIND` | `{path:u16, length:u16, flags:u16, type:u8 out, reserved:u8}` | `HL` = handle; `type` written |
| `07h` | `CALL` | `{handle:u8, op:u8, flags:u8, frame_length:u8, buffer:u16, length:u16, direction:u8, reserved:u8, frame:32, status:i16}` | `HL` = the protocol status |
| `08h` | `UNBIND` | `{handle:u8}` | `HL` = 0 |
| `09h` | `NS_ENUM` | `{path:u16, length:u16, cookie:u16 in/out, out:u16, out_size:u16, count:u16 out}` | `HL` = entries written |
| `0Ah` | `CHDIR` | `{path:u16, length:u16}` | `HL` = 0 |
| `0Bh` | `GETCWD` | `{buffer:u16, length:u16}` | `HL` = length without the NUL |
| `0Ch` | `REBOOT` | none | does not return |
| `0Dh` | `NOTIFY` | `{handle:u8, reserved:u8, handler:u16, trampoline:u16}` | `HL` = 0 |
| `0Eh` | `EVENT_RETURN` | none | does not return |
| `20h` | `XMEM_ALLOC` | `{size:u16, flags:u16}` | `HL` = object handle |
| `21h` | `XMEM_FREE` | `{handle:u16}` | `HL` = 0 |
| `22h` | `XMEM_READ` | `{handle:u16, offset:u16, buffer:u16, length:u16}` | `HL` = bytes moved |
| `23h` | `XMEM_WRITE` | as `XMEM_READ` | `HL` = bytes moved |
| `24h` | `XMEM_INFO` | `{handle:u16, buffer:u16}` | 8-byte record at `buffer` |
| `25h` | `XMEM_MAP` | `{handle:u16, address:u16 out}` | `HL` = map token |
| `26h` | `SEG_INFO` | `{segment:u8, reserved:u8, buffer:u16}` | 8-byte record at `buffer` |
| `27h` | `SEG_MAP` | `{segment:u8, reserved:u8}` | `HL` = map token |
| `28h`-`2Dh` | `DIAG_*` | section 9 | |

Paths in every block are 1..127 bytes with no NUL, in `0100h-DFFFh`,
resolved against the process's CWD. A zero length, an over-long path, an
embedded NUL or a buffer outside the window is `KOS_EINVAL`.

## 5. Processes

### 5.1 `EXIT`

Records the status, unbinds every handle the process owns, frees its
extended-memory objects, its segments and its bank, and resumes the parent
after its `EXEC`. A block outside the window ends the process with status
`KOS_EFAULT`. A program that returns from its entry with `RET` instead of
exiting is caught by a common trap and ends the same way: a process never
unwinds into the kernel or into another process.

### 5.2 `GETINFO`

Writes a six-byte record and returns 6:

| offset | size | field |
|---|---|---|
| 0 | 1 | `size` = 6 |
| 1 | 1 | `abi` = 1 |
| 2 | 1 | `pid`, the process slot 0-6 |
| 3 | 1 | `bank`, the primary bank |
| 4 | 2 | `flags`: bit 0 = the parent is the kernel launcher |

### 5.3 `EXEC`

Runs a child to completion and returns its exit status. It is synchronous:
there is no `wait` and no asynchronous spawn.

- `path` is resolved through the namespace. It is staged in the fixed-core
  VFS workspace, because the owning provider (possibly a bank-6 driver)
  reads it.
- `args` is a raw tail of `args_length` bytes (0..125) in `0100h-DFFFh`,
  copied byte-exact into the child's startup block. The kernel never
  splits or interprets it. Length 0 means no arguments.
- The child gets a fresh primary bank, its own page zero, standard handles
  1 and 2, a copy of the parent's CWD, and nothing else. Its `chdir` never
  affects the parent. It starts with P = 0, and the parent's latch,
  including P, is restored when it exits.
- At most 4 user processes form one chain; the next `EXEC` is refused.
- Every fallible step happens before the child is entered; a failure frees
  everything allocated for it.

| condition | error (carry set) |
|---|---|
| missing file | `KOS_ENOENT` |
| a directory | `KOS_EISDIR` |
| no card behind `/fs` | `KOS_ENODEV` |
| malformed, oversize or wrong-ABI EXE, or not an EXE entry in `/system` | `KOS_EINVAL` |
| bad block, tail over 125 bytes, tail outside the window | `KOS_EINVAL` |
| no free slot, bank or auxiliary space | `KOS_ENOSPC` |
| depth limit reached, or the gate frame too large | `KOS_EBUSY` |
| a provider read error | passed through |

### 5.4 The startup block

Every program is entered with `HL = 0080h`, its startup block v1:

```text
0080h  u8   version = 1
0081h  u8   tail_length (0..125)
0082h  u8   tail[126]: tail_length bytes, then one NUL
```

Every program gets a valid block; with no arguments `tail_length` is 0.
The block is below `0100h`, so a program copies the tail into its own data
before passing it to a syscall. The SDK's startup code stores the block for
`startup()` and never modifies it; `splitargs()` parses a copy.

### 5.5 The warm reboot

`REBOOT` (`0Ch`) restarts the machine from the beginning: the kernel masks
interrupts, stops every CTC channel it may have left programmed
(`platform_ctc_quiesce_all`: the tick, the event timer, and the two
channels no image programs), writes the banking latch for the reset mode
and jumps to `0000h`. The ROM reset vector
then re-copies the operating system from ROM exactly as at power-on, and
the boot path runs from the first instruction. Nothing in RAM is trusted
across the jump — every buffer, driver state and open object belongs to the
machine that is going away.

No peripheral gets a hardware reset: a warm reboot is invisible to the
V9958, the SIO, the CTC, the IOC and the keyboard controller. Each driver's
bring-up therefore programs its device fully from any state rather than
from power-on defaults, which is what makes the reboot indistinguishable
from a cold start (`docs/devices.md` section 14).

The SDK wrapper is `void reboot(void)`; it does not return. The monitor's
`B` command and the shell's `reboot` built-in call it.

## 6. Objects and the namespace

### 6.1 `BIND`

Resolves a name and binds an instance: `HL` is the handle (3 or more),
`type` is the 8-bit protocol type actually resolved (`KOS_TYPE_*` in
`abi/kos/object.h`). The caller passes no expected type. `flags` is
interpreted by the provider: open flags for files (`KFS_O_READ`, `WRITE`,
`CREATE`, `EXCL`, `TRUNC`), zero for most devices. Failure is `-KOS_E*`
in `HL`: `KOS_ENOENT` for an unknown name, `KOS_EMFILE` when the 12-entry
handle table is full, `KOS_ENODEV` for `/fs` without a card.

A DEVICE bind defines one flag: `KOS_BIND_EXCLUSIVE` (`8000h`,
`abi/kos/object.h`). It asks the device for a takeover — the caller may
then write the device's ports or reprogram it beyond the protocol — and
two failures come with it: a device whose driver declares no release
notification answers `-KOS_ENOTSUP`, and a device another handle already
holds exclusively answers `-KOS_EBUSY`. The takeover contract is
`docs/devices.md` section 2. Files and the other providers define no
exclusive flag, so `KOS_BIND_EXCLUSIVE` there is `-KOS_EINVAL`.

Paths are routed by the first component, case-insensitively, to the three
roots: `/fs` (the filesystem driver), `/system` (ROMFS) and `/dev` (the
publication table). `/` itself lists the three roots and is not bindable.

### 6.2 `CALL`

One protocol operation on a handle. The frame is 32 bytes; its meaning is
the handle's protocol (`abi/kos/protocol.h`).

**Two result channels.**

- **Carry set, `A` = `KOS_EINVAL`:** the request never reached the IPC
  layer: the block was outside the window, `frame_length` was 0 or above
  32, or a reserved byte was not zero.
- **Carry clear, `HL` = the status** (also written to `status`, and the
  reply frame written back into the block): 0 or positive is the driver's
  result. Negative is `-KOS_E*`, either a driver's protocol error or a
  refusal by the IPC layer:

  | refusal | error |
  |---|---|
  | unknown, stale or foreign handle | `KOS_EINVAL` |
  | forged or released descriptor id | `KOS_EINVAL` |
  | `offset + length` past the descriptor | `KOS_EINVAL` |
  | re-entry into a non-re-entrant endpoint | `KOS_EBUSY` |
  | nesting deeper than 4 | `KOS_ENOSPC` |

A negative status with carry clear is a normal reply, not a syscall
failure.

**The descriptor.** One optional caller buffer per call, named by `buffer`,
`length` and `direction` (`KOS_IPC_DESC_IN` = 0, the caller supplies data;
`KOS_IPC_DESC_OUT` = 1, the caller receives it). `length` 0 means no buffer,
and the frame's `desc` is `KOS_IPC_DESC_NONE` (`FFh`). Otherwise:

- the buffer must lie in `0100h-DFFFh` of the caller's bank or in common
  memory, and be at most `KOS_IPC_DESC_MAX` (4,096) bytes;
- the nucleus captures it at entry from the caller's saved latch and hands
  the driver a small id; the driver never sees the address;
- drivers reach it only through `copy_in`/`copy_out`, which check
  `offset + length` on every use;
- an id is valid only below the level that captured it, so ids flow down a
  call chain and a driver can never forge one;
- with a non-zero saved page, a buffer wholly inside `8000h-BFFFh` names the
  far bank in the aperture; a buffer that crosses the aperture's edge is
  refused.

**Copy paths.** The nucleus picks one per request; all are correct:

| path | when | how |
|---|---|---|
| A | caller buffer in `0000h-1FFFh` or in common | one `LDIR` in mode 11 |
| B | caller buffer in its bank clear of `8000h-BFFFh`, driver buffer in bank 6 `8000h-BFFFh` | one `LDIR` in mode 10 over the caller's bank with page 1; the latch is restored exactly; interruptible |
| C | anything else | chunked through the common bank-copy gate (slow) |

Page 1 is internal: no program ever receives a bank-6 latch.

**Nesting.** At most 4 levels. An endpoint marked `KOS_IPC_EP_REENTRANT` may
be re-entered; any other refuses a call while it is active. A driver runs on
the fixed driver stack with its page mapped. An interrupt during a call
runs the owning driver's handler with a restricted API (no calls, no
copies) on the ISR stack and restores the latch exactly.

### 6.3 `UNBIND`

Releases a handle the caller owns. A foreign handle, an unbound number or
a standard handle (1 or 2) is `-KOS_EINVAL`.

A released DEVICE handle tells the driver its instance went away, and an
exclusive one tells it that the takeover ended, so the driver can restore
what the program changed. The notification is the driver's own business:
its result is ignored and `UNBIND` still answers 0.

### 6.4 Standard handles

`KOS_HANDLE_STDOUT` (1) and `KOS_HANDLE_STDIN` (2) are bound to
`/dev/console` when each process starts, if the build has a console
driver. `BIND` never returns them. A child gets its own pair; no other
handle passes to a child. Process exit unbinds every handle.

### 6.5 `NS_ENUM`

Writes up to `out_size / 28` entries of `KOS_NS_ENTRY_SIZE` (28) bytes,
starting at the index in `cookie`, and writes the next index back:

```text
u8   type          the protocol type, or 0 for a container
u8   flags         KOS_NS_ENTRY_CONTAINER: further components may follow
u8   name_length
u8   reserved
char name[24]      NUL-terminated, zero-padded
```

Below `/fs` the filesystem driver supplies the entries. `/dev/input` and
`/dev/serial` are containers.

### 6.6 `CHDIR`, `GETCWD`

`CHDIR` asks the owning provider whether the path is a traversable
location and stores the canonical path; it binds nothing. `GETCWD` writes
the NUL-terminated path and returns its length; a buffer too small is
`KOS_EINVAL`.

### 6.7 `NOTIFY` and `EVENT_RETURN`

A program subscribes to events on a device handle it holds (`NOTIFY`), and
the nucleus delivers them when it is the program's turn to run again.

**`NOTIFY`.** One subscription per (process, handle) pair. `handle` is a
bound device handle, `handler` the signal handler and `trampoline` the
libkos entry the nucleus `RETI`s to (`kos/event.h`'s `notify()` fills it
in; a program never writes them itself). A second `NOTIFY` on the same
handle replaces the handler; `trampoline` 0 or a `reserved` byte that is
not zero is `-KOS_EINVAL`. `-KOS_ENOSPC` when the four subscription slots
are taken, `-KOS_ENOTSUP` when the endpoint does not serve events. The
nucleus tells the driver through `KOS_OP_SUBSCRIBE` on the same handle,
with the subscription id in `arg0` (0 unsubscribes); a driver that refuses
makes `NOTIFY` fail.

**Delivery.** The nucleus checks for a deliverable subscription at exactly
two points, and only for the process the check is returning to:

| the process ... | delivery |
|---|---|
| is running its own code (an interrupt ends) | now, before the interrupted instruction |
| is inside its own syscall or `CALL` (the call ends) | now, before the result registers are rebuilt |
| is inside its own handler | held: never nested |
| has a child running | dropped, counted in `missed` |
| has exited or unbound the handle | the subscription is gone |

Events coalesce: the handler receives `count` (the events that arrived
since the last delivery, saturating at `FFFFh`) and `missed` (the events
dropped while it was not running). Nothing is queued, and the handler
returns through `EVENT_RETURN`, which is the trampoline's job.

**`EVENT_RETURN`.** Its own number, in `A`, with no argument block, called
by the libkos trampoline after the handler returns - not by programs. The
nucleus either hands out the next pending event through the same frame or
restores the exact interrupted state (latch, registers, PC, SP) and returns
to it. Calling it without a delivery in progress is `-KOS_EINVAL`.

Because the delivery frame is built on the program's own stack and the
handler runs below it, the frame and the handler's own use must both fit in
the stack the program reserved (`docs/sdk.md`); the reserved stack never
sits in the `8000h-BFFFh` aperture, which the frame may not either.

## 7. Console

Console I/O is not a syscall: the SDK speaks the `CONSOLE` protocol
through the standard handles.

| SDK function | operation | descriptor |
|---|---|---|
| `console_write(buf, len)` | `KCONSOLE_OP_WRITE` | in: the caller's buffer |
| `console_read(buf, len)` | `KCONSOLE_OP_READ` (returns what is pending) | out: the caller's buffer |
| `console_poll()` | `KCONSOLE_OP_POLL` | none |
| `putchar(ch)` | `KCONSOLE_OP_PUTC` | none |
| — | `KCONSOLE_OP_RESET` (5) | none |
| `console_render(stream, len)` | `KCONSOLE_OP_RENDER` (6) | in: one versioned render stream |
| `console_info(&info)` | `KCONSOLE_OP_INFO` (7) | none; the reply payload is `struct kconsole_info` |

The version-1 render stream (`kos/console_render.h`) carries absolute TEXT,
INVERT and STATE records plus SCROLL. SCROLL moves one full-width region by
one logical row (`UP` or `DOWN`) and blanks the exposed row with its supplied
attribute. A backend validates the complete descriptor before changing the
display.

`KCONSOLE_OP_RESET` puts the terminal back to its start state and empties
the input queue; a console whose output driver owns the display forwards
the display reset to it. The kernel runs it before every start of the
boot program, so no program inherits a terminal another program left
behind (`docs/devices.md` section 14).

`getchar`, `console_puts` and `console_readline` are built on these. A
process without standard handles gets `-KOS_EINVAL`. The build decides which
console driver answers; the program cannot tell. The early console and the
panic path are nucleus code and need no driver.

## 8. Extended memory and segments

### 8.1 `XMEM_*`

- `ALLOC`: `flags` is `KOS_XMEM_FLAG_NONE` or `KOS_XMEM_FLAG_MAPPABLE` (at
  most 16 KiB, placed in the aperture slice). Any other bit, a zero size or
  a mappable size over 16 KiB is `-KOS_EINVAL`. A size over 32 KiB, a full
  object table or no fitting run is `-KOS_ENOSPC`. The size rounds up to
  whole 8 KiB slots; placement is never visible.
- **Contents are undefined.** Nothing is pre-zeroed.
- `READ`, `WRITE`: every range is checked before a byte moves; a copy-gate
  failure is `-KOS_EFAULT`.
- `INFO` writes `requested`, `allocated`, `flags` and `type`
  (`KOS_XMEM_TYPE_RUNTIME`, or `KOS_XMEM_TYPE_SEGMENT` for a data segment),
  never a bank or slot.
- `FREE` refuses a segment's handle: segments are released at exit.
- `MAP` returns the token that opens the aperture onto a mappable object,
  and the object's address there (`8000h` or `A000h`). A stale, foreign or
  non-mappable handle is `-KOS_EINVAL`. Tokens name only banks 0-5
  (P = 2-7).
- Handles are owner-tagged and generation-counted: a freed and reused entry
  yields a different handle. A child inherits no object.

The SDK opens and closes the aperture without a syscall:
`aperture_open(token)` writes the token and returns the previous latch;
`aperture_close(previous)` writes it back.

### 8.2 `SEG_INFO`, `SEG_MAP`

Segments are the program's own container segments, ids `1..segment_count`
in table order (far-code segments first). `SEG_INFO` writes:

```text
offset 0  u16  size     image + BSS
offset 2  u8   kind     KOS_SEG_KIND_FAR_CODE (1) or KOS_SEG_KIND_DATA (2)
offset 3  u8   state    KOS_SEG_STATE_LOADED
offset 4  u16  handle   a data segment's object handle; 0 for far code
offset 6  u16  flags    KOS_XMEM_FLAG_MAPPABLE for a mappable data segment
```

- A data segment is an extended-memory object of type `SEGMENT`; `XMEM_READ`
  and `XMEM_WRITE` take its handle, and a mappable one is opened with
  `XMEM_MAP`.
- `SEG_MAP` returns the token of a far-code segment, which is always at
  `8000h`. A data segment's id, an unknown id or a bad block is
  `-KOS_EINVAL`.
- A syscall made from far code may name a buffer inside the open aperture
  (the buffer identity rule).
- Every segment is placed at load or the exec fails, so a live segment is
  always `LOADED`.

The SDK wrappers are `seginfo`, `segmap`, `farcall`, the `__banked`
trampoline and `farmap`/`farunmap` (`docs/sdk.md`).

## 9. Diagnostics

For the monitor and the diagnostic tools; ordinary programs use drivers.
Every failure changes nothing, and every call restores the exact latch.

```text
28h  DIAG_MEM_READ     {bank:u8, reserved:u8, address:u16}  -> byte in HL
29h  DIAG_MEM_WRITE    {bank:u8, value:u8, address:u16}
2Ah  DIAG_IO_IN        {port:u8}                            -> byte in HL
2Bh  DIAG_IO_OUT       {port:u8, value:u8}
2Ch  DIAG_BANK_INFO    no block                             -> HL = mask << 8 | bank
2Dh  DIAG_SYSTEM_INFO  record at the block                  -> HL = 8
```

- `MEM_READ`: banks 0-6, `0000h-DFFFh`; `reserved` must be 0.
- `MEM_WRITE`: banks 0-6, `0100h-DFFFh` (page zero and common memory are
  protected).
- Bank 7, out-of-range addresses and wrap-around are `KOS_EINVAL`.
- `IO_IN`, `IO_OUT`: ports `10h-FFh` only. Ports `00h-0Fh` alias the latch,
  so they are refused before anything runs.
- `BANK_INFO`: `L` is the caller's bank; `H` is `7Fh` (bit n set means bank
  n is reachable by these calls; bit 7, the nucleus bank, is always clear).
- `SYSTEM_INFO`, the eight-byte v1 record:

  | offset | size | field | value |
  |---|---|---|---|
  | 0 | 1 | `size` | 8 |
  | 1 | 1 | `abi` | 1 |
  | 2 | 1 | `app_mode` | `10h` |
  | 3 | 1 | `os_mode` | `18h` |
  | 4 | 2 | `common_base` | `E000h` |
  | 6 | 1 | `kernel_bank` | 7 |
  | 7 | 1 | `romfs_first_page` | 2 |

The SDK wrappers are in `kos/diag.h`, which `kos.h` does not include.

## 10. Errors

One numbering for the whole system (`abi/kos/errno.h`):

| value | name | meaning |
|---|---|---|
| `01h` | `KOS_ENOSYS` | no such syscall |
| `02h` | `KOS_EINVAL` | invalid argument, handle or descriptor |
| `03h` | `KOS_ENODEV` | no device, or no media |
| `04h` | `KOS_EBUSY` | busy, owned, or a limit on nesting |
| `05h` | `KOS_ENOENT` | not found |
| `06h` | `KOS_ENOSPC` | out of space, memory or table entries |
| `07h` | `KOS_ENOTSUP` | not supported by this object |
| `08h` | `KOS_ETIMEDOUT` | a bounded wait expired |
| `09h` | `KOS_EAGAIN` | nothing available yet |
| `0Ah` | `KOS_EIO` | corrupt, truncated or failed I/O |
| `0Bh` | `KOS_EINIT` | initialization refused |
| `0Ch` | `KOS_EFAULT` | a process fault (bad block at exit, raw return) |
| `10h` | `KOS_EEXIST` | already exists |
| `11h` | `KOS_ENOTDIR` | not a directory |
| `12h` | `KOS_EISDIR` | is a directory |
| `13h` | `KOS_ENOTEMPTY` | directory not empty |
| `14h` | `KOS_EROFS` | read-only |
| `15h` | `KOS_ESTALE` | stale object identity |
| `16h` | `KOS_EXDEV` | across providers |
| `17h` | `KOS_EMFILE` | handle table or open-file limit reached |

Syscalls return the code in `A` with carry set; `CALL` statuses and SDK
wrappers return it negated.
