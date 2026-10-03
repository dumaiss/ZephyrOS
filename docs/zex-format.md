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

`tests/m6/zextest.json` is the program-side source of truth for the
layout; the linker script and the packer must both agree with it:

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
- the runtime scratch symbol (`zos_runtime_scratch`) must lie inside the
  syscall argument window `0100h-1FFFh`, so it remains accessible in
  mode `10` and in mode `11`.

`tools/zexpack.py` rejects any violation, so `make m6-selftest` cannot
pack a program that the kernel would later refuse.

## 5. Load sequence

`process_exec("name.zex")` performs:

1. allocate a process slot (fails with `ZEX_EXEC_ERR_NOSPACE` if none is
   free) and the lowest free application bank `0-6`;
2. read and validate the header (from ROMFS through the M5 reader);
3. zero the whole process bank `0000h-DFFFh`;
4. write the native page-zero vector at `0030h`: `C3` followed by the
   entry address (the `RST 30h` gateway from `docs/abi.md`);
5. copy the image in 64-byte chunks from ROMFS through the common
   `zos_user_copy_staging` block into the bank at `0100h`;
6. zero the BSS region `[0100h + image_size, + bss_size)`;
7. mark the descriptor RUNNING and enter the common process-launch gate,
   which switches to mode `10 | bank`, installs
   `SP = E000h - stack_size`, and transfers to the entry with `HL = 0`.

A raw `RET` from the entry point lands in a common trap and exits the
process with `ZOS_EFAULT`; programs leave through `SYS_EXIT` only.

Failure at any step releases the slot and the bank reservation before
returning the result code, so a rejected program leaves the process
table, the bank allocator, the caller bank, and the parent context
bit-identical to their pre-call state (verified by the M6 failure image
and the simulator tests).

## 6. Runtime

`user/runtime/zex_runtime.{h,asm}` is the first-party C runtime for ZEX
programs (SDCC `sdcccall(1)` wrappers around `RST 30h`):

```c
void zex_exit(unsigned int status);      /* SYS_EXIT, does not return */
int  zex_write(const char *buf, unsigned int len);  /* SYS_CONSOLE_WRITE */
int  zex_read(char *buf, unsigned int len);         /* SYS_CONSOLE_READ */
int  zex_poll(void);                     /* SYS_CONSOLE_POLL */
int  zex_getinfo(struct zex_info *info); /* SYS_GETINFO */
```

Negative returns are `-errno`.  The wrappers keep their 8-byte argument
scratch (`_zos_runtime_scratch`) inside the linked image, which is why
the scratch symbol must satisfy the window rule above.

## 7. Tooling

```sh
python3 tools/zexpack.py get      tests/m6/zextest.json image_size
python3 tools/zexpack.py pack     tests/m6/zextest.json out.zex \
        --objdir build/zextest --report out.report.json
python3 tools/zexpack.py validate out.zex
python3 tools/zexpack.py dump     out.zex
```

The packer output (image bytes, report JSON) is byte-deterministic; the
report carries the header fields, image/BSS/stack geometry, entry symbol
resolution, and sha256 digests.  `make zex-test` packs, validates, dumps,
and runs the host parity suite that pins `zexpack.py` constants and
result codes to `include/zephyros/zex.h` value-for-value.
