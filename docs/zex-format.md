# ZEX v1 executable format

ZEX ("Zephyr EXecutable") is the native, single-application-bank program
format introduced by M6.  A ZEX file is stored in the ROMFS recovery
store (see `docs/romfs-format.md`), loaded by the kernel into one
application bank `0-6`, and entered in mode `10`.  The loader's
authoritative implementation is `kernel/process/exec.c`; the host-side
packer (`tools/zexpack.py`) mirrors every validation boundary so a bad
link cannot reach the ROMFS.

## 1. Logical load address

Every ZEX is linked as a flat image whose first byte executes at logical
address `0100h`:

```text
0100h                 start of the image
0100h + image_size    start of BSS (zero-filled by the loader)
...                   free process memory
E000h - stack_size    initial SP (stack grows downward from E000h)
E000h                 permanent common memory begins
```

The logical addresses are independent of the physical application bank:
the same bytes run unchanged in bank 0, 1, ..., 6.  There is no base
relocation.

## 2. Header

The header is 20 bytes, little-endian for all 16-bit fields:

| Offset | Size | Field | Value / meaning |
| --- | --- | --- | --- |
| `00h` | 4 | `magic` | ASCII `ZEX1` (`5A 45 58 31`) |
| `04h` | 1 | `format_version` | `1` |
| `05h` | 1 | `abi_version` | `1` |
| `06h` | 1 | `flags` | `0` (no flags defined) |
| `07h` | 1 | `reserved0` | `0` |
| `08h` | 2 | `header_size` | `20` |
| `0Ah` | 2 | `image_size` | bytes of loaded image, `> 0` |
| `0Ch` | 2 | `bss_size` | zero-initialized bytes following the image |
| `0Eh` | 2 | `entry_offset` | entry = `0100h + entry_offset` |
| `10h` | 2 | `stack_size` | initial stack size, `>= 2` |
| `12h` | 2 | `reserved1` | `0` |

The file length must be exactly `header_size + image_size`; a truncated
or padded file is rejected (`ZEX_EXEC_ERR_FILESIZE`).

## 3. Geometry rules

Validated in this order (first violation wins); the codes are also the
`process_exec()` result values (`include/zephyros/zex.h`):

| Order | Check | Failure code |
| --- | --- | --- |
| 1 | magic is `ZEX1` | `ZEX_EXEC_ERR_MAGIC` |
| 2 | `format_version == 1` | `ZEX_EXEC_ERR_FORMAT` |
| 3 | `abi_version == 1` | `ZEX_EXEC_ERR_ABI` |
| 4 | `flags == 0` | `ZEX_EXEC_ERR_FLAGS` |
| 5 | `reserved0 == 0` | `ZEX_EXEC_ERR_RESERVED0` |
| 6 | `header_size == 20` | `ZEX_EXEC_ERR_HEADER` |
| 7 | `reserved1 == 0` | `ZEX_EXEC_ERR_RESERVED1` |
| 8 | `image_size > 0` | `ZEX_EXEC_ERR_IMAGE` |
| 9 | file size == `20 + image_size` | `ZEX_EXEC_ERR_FILESIZE` |
| 10 | `entry_offset < image_size` | `ZEX_EXEC_ERR_ENTRY` |
| 11 | `stack_size >= 2` | `ZEX_EXEC_ERR_STACK` |
| 12 | `0100h + image_size <= E000h` | `ZEX_EXEC_ERR_IMAGE_END` |
| 13 | `image_end + bss_size <= E000h` | `ZEX_EXEC_ERR_BSS_END` |
| 14 | `stack_base >= 0100h` and `stack_base >= bss_end` | `ZEX_EXEC_ERR_COLLISION` |

The loader adds these result codes around the geometry checks:
`ZEX_EXEC_ERR_ARG` (null argument / invalid request),
`ZEX_EXEC_ERR_ROMFS` (name not found), `ZEX_EXEC_ERR_TYPE` (ROMFS entry
is not a ZEX), `ZEX_EXEC_ERR_NOSPACE` (no free process slot or bank),
`ZEX_EXEC_ERR_READ` (ROMFS read / bank write failed).  All arithmetic is
done in 32 bits so 16-bit wrap can never mark an out-of-range geometry
valid.

## 4. Link layout contract

Every ZEX is linked by the SDK's generic rule (`user/sdk/zex.mk`, section
8 of `docs/sdk.md`), which derives the manifest from the link map; the
linker script and the packer must both agree with the resulting map:

- `_CODE` is linked at `0100h`; the linked bytes must be entirely inside
  `[0100h, 0100h + image_size)`;
- `_DATA` is linked at `0100h + image_size` and must satisfy
  `s__DATA == 0100h + image_size` (`s__DATA` proves where BSS starts) and
  `l__DATA <= bss_size`;
- initialized writable data is not supported by ZEX v1:
  `l__INITIALIZER` and `l__INITIALIZED` must be zero, so every static
  starts zeroed;
- the declared entry symbol must resolve to exactly
  `0100h + entry_offset`;
- the runtime scratch symbol (`zos_scratch`) must lie inside the
  syscall argument window `0100h-1FFFh`, so it remains accessible in
  mode `10` and in mode `11`.

The SDK links crt0 and the shared runtime object first (`crt0.rel
runtime.rel <program>`, then `libzos`), so the scratch block always sits
at the start of `_DATA`, ahead of the program's own statics: the whole
program, its statics and the scratch stay below `2000h` even when the
program's BSS is large (`fstest.zex` is the worst case).  The manifest
`scratch` field names that one symbol per program; a program linked
without crt0 (the raw-return test child) declares no scratch at all.

`tools/zexpack.py` rejects any violation, so the generic rule cannot pack
a program that the kernel would later refuse.

## 5. Load sequence

`process_exec("name.zex")` (the kernel launcher) and `SYS_EXEC`
(`docs/abi.md` section 11, an ordinary process) both run the same two
steps: `process_prepare()` loads, then `process_launch()` enters.

`process_prepare()` performs:

1. allocate a process slot (fails with `ZEX_EXEC_ERR_NOSPACE` if none is
   free) and the lowest free application bank `0-6`;
2. read and validate the header (from ROMFS through the M5 reader);
3. zero the whole process bank `0000h-DFFFh`;
4. write the native page-zero vector at `0030h`: `C3` followed by the
   entry address (the `RST 30h` gateway from `docs/abi.md`);
5. copy the image in 64-byte chunks from ROMFS through the common
   `zos_user_copy_staging` block into the bank at `0100h`;
6. zero the BSS region `[0100h + image_size, + bss_size)`;
7. close the executable source, then build startup block v1 (below) and
   write it into the child's page zero at `0080h`;
8. return the prepared child.

`process_launch()` then marks the descriptor RUNNING and enters the common
process-launch gate, which saves the launcher's continuation, switches to
mode `10 | bank`, installs `SP = E000h - stack_size`, and transfers to the
entry with `HL = 0080h` (the startup block pointer).  It performs no check
that can fail: every fallible step (including the SYS_EXEC depth,
frame-size and argument preflight) happened before the suspension point.

### 5.1 Startup block v1

Every native process, including the kernel launcher's, receives a valid
startup block in its own page zero:

```text
0080h  u8   version = 1
0081h  u8   tail_length  0..125
0082h  u8   tail[126]    tail_length raw bytes, then one NUL
```

`SYS_EXEC` copies the caller's raw tail (`args`, `args_length`) here
byte-exact; the kernel launcher passes an empty tail.  The block lives in
the caller-visible low window so it is readable in modes `10` and `11`,
but it is *below* the `0100h` syscall buffer window, so a program must
copy the tail into its own data before handing it to a syscall.

`user/sdk/` is the first-party user-space runtime for it: crt0
(`user/sdk/src/crt0.asm`, entry symbol `zos_entry`) receives the block in
`HL`, stores the pointer for `zos_args()` before anything clobbers `HL`,
runs `zos_split()` over the raw tail, calls `int main(int argc, char
**argv)` and exits through `SYS_EXIT` with the returned status.  A
program writes no entry assembly; `zos_args()` returns the kernel's
page-zero block, and crt0 copies the tail into the SDK's own buffer
before `zos_split()` parses it *in place* there, so `argv` stays valid
after the first syscall resets page zero.

#### Tail splitting (runtime policy)

`zos_split(char *tail, char **argv, int max)` splits the NUL-terminated
tail in place and returns the total word count, storing at most `max`
pointers.  The rules are runtime policy, not an ABI, and may change
without any kernel work:

- words are separated by whitespace: one or more spaces (`0x20`) or
  tabs (`0x09`); leading and trailing separators are ignored;
- a double quote (`"`) toggles a quoted section.  Separators inside a
  quoted section are literal; the quote characters themselves are
  removed;
- adjacent quoted and unquoted runs join into one word (so `ab"c d"e`
  is the single word `abc de`);
- an empty quoted word (`""`) is an empty argument;
- an unterminated quote runs to the end of the tail;
- a NUL byte ends the tail; there is no escape character and no other
  metacharacter;
- `argv[0]` is the first argument: the kernel does not pass a program
  name, so there is no `argv[0]` convention for it.

The transport is proven end to end by the M9 exec acceptance: `fstest`
runs `fschild` with no arguments, one word, several words with a quoted
space, and a trailing separator, and the simulator replay asserts the
exact echoed `argv`; the kernel preflight refuses a 126-byte tail and a
tail pointer outside the window with `ZOS_EINVAL` before allocating.

A raw `RET` from the entry point lands in a common trap and exits the
process with `ZOS_EFAULT`; programs leave through `SYS_EXIT` only.

Failure inside `process_prepare()` releases the slot, the bank
reservation, the child's CWD slot and any descriptor it opened, and closes
the source before returning the result code, so a rejected program leaves
the process table, the bank allocator, the caller bank, and the parent
context bit-identical to their pre-call state (verified by the M6 failure
image, the M9 exec failure modes and the simulator tests).

A child started by `SYS_EXEC` is a full ordinary process: a fresh
application bank, its own page zero, its own VFS process state (it
*inherits* the parent's CWD and *not* the parent's descriptors), and its
exit status handed back as the `SYS_EXEC` result.  Up to
`ZOS_EXEC_MAX_DEPTH` (4) user processes may be suspended in one chain; the
suspension cost is the gate-stack frame described in `docs/memory.md` and
`docs/abi.md`.

## 6. Runtime

`user/sdk/` (libzos: `zos/console.h` with the gate stubs in
`user/sdk/src/console.asm`) is the first-party C runtime for ZEX programs
(SDCC `sdcccall(1)` wrappers around `RST 30h`):

```c
void zex_exit(unsigned int status);      /* SYS_EXIT, does not return */
int  zex_write(const char *buf, unsigned int len);  /* SYS_CONSOLE_WRITE */
int  zex_read(char *buf, unsigned int len);         /* SYS_CONSOLE_READ */
int  zex_poll(void);                     /* SYS_CONSOLE_POLL */
int  zex_getinfo(struct zex_info *info); /* SYS_GETINFO */
```

Negative returns are `-errno`.  The wrappers share one argument scratch
(`zos_scratch`, in `user/sdk/src/runtime.asm`) inside the linked image,
which is why the scratch symbol must satisfy the window rule above; the
SDK links the runtime object first so the scratch always lands at the
start of BSS, ahead of a program's own data.

`user/sdk/include/zos/fs.h` adds the M9 filesystem wrappers
(`zos_open`, `zos_read`, ...; gate stub `user/sdk/src/fsgate.asm`) and
`user/sdk/include/zos/process.h` adds
`zos_exec(const char *path, const char *args)` on top of them: the same
low-window argument block and gate stub, the same 127-byte local path
check plus a 125-byte tail check, returning the child's exit status or
the negated native error code (`docs/abi.md` section 11).  A NULL or
empty `args` is "no arguments"; the raw tail reaches the child byte-exact
through the startup block (`zos_args`/`zos_split` above).

## 7. Tooling

```sh
python3 tools/zexmanifest.py build/user/zextest/zextest.map \
        build/user/zextest/zextest.json --name zextest.zex \
        --entry zos_entry --stack 256 --scratch zos_scratch:16
python3 tools/zexpack.py get      build/user/zextest/zextest.json image_size
python3 tools/zexpack.py pack     build/user/zextest/zextest.json out.zex \
        --objdir build/user/zextest --report out.report.json
python3 tools/zexpack.py validate out.zex
python3 tools/zexpack.py dump     out.zex
```

`tools/zexmanifest.py` derives `image_size`, `bss_size` and
`entry_offset` from the link map, so no size is ever hand-entered; the
packer output (image bytes, report JSON) is byte-deterministic; the
report carries the header fields, image/BSS/stack geometry, entry symbol
resolution, and sha256 digests.  `make zex-test` packs, validates, dumps,
and runs the host parity suite that pins `zexpack.py` constants and
result codes to `include/zephyros/zex.h` value-for-value.
