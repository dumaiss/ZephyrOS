# KonaOS Memory Map

What the build places where, and how it is checked. The rules are in
`docs/architecture.md` (sections 2, 4.2, 4.8 and 9); this document is the
reference for addresses, linker areas and ledgers.

## 1. Regions

Mode 11 (the nucleus's view) with the window on bank 7:

| region | content |
|---|---|
| `0000h-00FFh` | the caller's page zero: the reset copier (`0000h-0026h`, in the boot image) and the `RST 30h` vector at `0030h-0032h` (`JP kos_syscall_entry`) |
| `0100h-1FFFh` | the caller window; syscall argument blocks must lie here |
| `2000h`-fixed code end | **fixed core** loadable code: `_CODE`, `_HOME`, `_INITIALIZER`, `_GSINIT`, `_GSFINAL` |
| fixed data base-`5CFFh` | fixed-core runtime data (`_DATA`, `_INITIALIZED`), zeroed at boot, never loaded from ROM |
| `5D00h-5EFFh` | the 512-byte **driver stack** (`kos_driver_stack`), used by every driver call |
| `5F00h-5FFFh` | the 256-byte **ISR stack** (`_IRQ_ISR_STACK`) |
| `6000h`-cold code end | **cold half** loadable code (`_WINDOW`): the syscall dispatcher and bodies, process management, the VFS, the driver loader, boot |
| cold data base-`DFFFh` | cold-half runtime data (`_WINDOW_DATA`), zeroed at boot |
| `E000h-FCFFh` | common code and data (`_COMMON`, `_COMMON_DATA`) |
| `EEFEh-EF07h` | the ROM-access transition block, inside the common range |
| `FD00h-FDFFh` | the IM2 vector page (`I = FDh`) |
| `FE00h` | the vector-`FFh` guard byte |
| `FE01h-FE07h` | IRQ transition state |
| `FE08h-FE87h` | the 128-byte IRQ transition stack |
| `FE88h-FFFFh` | the common boot and runtime stack (initial `SP = 0000h`, grows down) |

Mode 10 (a program's view) maps the program's bank at `0000h-DFFFh` and
common memory at `E000h-FFFFh`; with P ≠ 0, `8000h-BFFFh` shows bank 7 − P.
The full decoder table is in architecture section 2.2.

### 1.1 Banks

| bank | role |
|---|---|
| 7 | the nucleus; never a process bank |
| 6 | the resident driver domain (section 3); never a process bank |
| 0-5 | primary banks and auxiliary (extended-memory) banks; the allocator hands out these only (`PROCESS_BANK_COUNT`) |
| 0 | also supplies common memory |

Bank 6 stays addressable: the mode-11 window shows it at P = 1, and the
diagnostic calls can read it.

### 1.2 ROM

The ROM is eight 64-KiB pages. Pages 0 and 1 are the boot pages, copied to
SRAM banks 0 and 7 at reset (`docs/boot.md`). Pages 2-7 hold the recovery
store: only `0000h-BFFFh` of each page is store data
(`KOS_ROMFS_WINDOW_SIZE`, 48 KiB), so the store holds at most 288 KiB
(`KOS_ROMFS_CAPACITY`). `C000h-FFFFh` of each store page carries a mirror of
the ROM-access stub and is never file data.

## 2. Linker areas

`linker/zephyr80.lk` is a template; the build substitutes the derived data
bases into `build/zephyr80.lk`.

```text
_CODE        = 2000h            fixed loadable code
_DATA        = <derived base>   fixed runtime data
_WINDOW      = 6000h            cold loadable code
_WINDOW_DATA = <derived base>   cold runtime data
_COMMON      = E000h            common code and data
```

Unbased areas follow their based area in first-appearance order, fixed by
the declarations at the top of `boot/start.asm`:

```text
_CODE -> _HOME -> _INITIALIZER -> _GSINIT -> _GSFINAL     fixed loadable
_DATA -> _INITIALIZED                                     fixed runtime
_WINDOW -> _WINDOW_DATA                                   cold
_COMMON -> _COMMON_DATA                                   common
```

| area | region | holds |
|---|---|---|
| `_RESET` | `0000h-0026h` | the stackless reset copier (absolute) |
| `_PAGE_ZERO` | `0030h-0032h` | the `RST 30h` vector (absolute) |
| `_CODE`, `_HOME`, `_INITIALIZER`, `_GSINIT`, `_GSFINAL` | fixed core | fixed code, string literals, initialized-data images, static initializers |
| `_DATA`, `_INITIALIZED` | fixed core | fixed runtime data |
| `_WINDOW` | cold half | cold code (compiled with `--codeseg WINDOW`) |
| `_WINDOW_DATA` | cold half | cold runtime data (`--dataseg WINDOW_DATA`) |
| `_COMMON`, `_COMMON_DATA` | `E000h-FCFFh` | gates, IRQ stubs, early console, panic, copy primitives, transition state |
| `_ROM_ACCESS_STUB` | `EEFEh-EF07h` | the ROM-access trampoline and mirrored stub |
| `_IRQ_ISR_STACK` | `5F00h-5FFFh` | the ISR stack |
| `_IRQ_VECTOR` | `FD00h-FE00h` | the IM2 page and its guard |
| `_IRQ_COMMON_STATE` | `FE01h-FE87h` | IRQ state and the transition stack |

Every area exports `s__<area>` and `l__<area>`. `__kos_crt0_init`
(`boot/start.asm`) copies `_INITIALIZER` to `_INITIALIZED` and zeroes
`_DATA` and `_WINDOW_DATA`.

### 2.1 Derived geometry

Nothing inside bank 7 has a fixed boundary except the stacks. The build
links twice:

1. **Probe link:** the object list links with a placeholder
   `kernel/memory/bank7_end.asm`.
2. **Derive:** `tools/mapcheck.py geometry derive` measures the probe map
   and writes `build/geometry/memory.inc` and `bank7_end.inc`:

   ```text
   fixed_code_end    = end of the fixed loadable areas
   data_base         = align_down(KOS_DRIVER_STACK_BASE - fixed data size, 512)
   __kernel_code_end = end of _WINDOW
   window_data_base  = align_up(__kernel_code_end, 512)
   ```

3. **Final link** with the derived bases.

So fixed runtime data ends just below the driver stack, and cold runtime
data starts just after the cold code.

### 2.2 Residency classification

`config/residency.json` classifies every production object as
`fixed-required` (code that runs, or data that is touched, while the window
shows another bank) or `cold`. `tools/mapcheck.py` fails the build if:

- an object is missing from the classification;
- a `fixed-required` object links at or above `6000h`;
- an object's areas disagree with its cold placement.

Objects that are not required in the fixed core but do not fit in the cold
half may link in the fixed core; the ledger reports them as `overflow`.
`cold_half` lists the objects the build compiles into the cold areas, and
`test_link` holds objects only acceptance images link, checked by the same
rules.

The VFS workspace (`vfs_workspace`), the buffer through which every path
reaches a provider, is `fixed-required` (the buffer rule, architecture
4.2).

**Upcall shim** (`kernel/memory/upcall.asm`): cold code that calls into
bank 6 fills a request block (`page`, `entry`, `args`, `result`). The shim
checks the page (0 or 1) and the entry (at or above `6000h`), saves the
latch, selects the page, calls and restores the latch. The driver loader
uses it to run each driver's hot init.

## 3. The driver bank (bank 6)

| region | tier | content |
|---|---|---|
| `6000h-DFFFh` | hot (32 KiB) | driver code and data used after boot; `8000h-BFFFh` holds the `DRIVER_IO` buffers first |
| `0000h-5FFFh` | boot/bulk (24 KiB) | boot-only driver code and bulk data |
| `E000h-FFFFh` | unused | common memory is mapped over it |

The packer (`tools/modpack.py`) places every segment; the loader only
checks bounds (`docs/module-format.md`). The bank-6 driver set is not linked
into bank 7, so no bank-7 geometry depends on it.

## 4. Ledgers

`tools/pack_image.py` and `tools/modpack.py plan` print the ledgers and
emit them as JSON (`build/konaos.manifest.json` `nucleus`, and
`drivers.plan.json`):

```text
nucleus fixed: required <bytes> | overflow <bytes> | stacks <bytes> | room <bytes>
nucleus cold: code <bytes> end <addr> | data <bytes> at <addr> | room <bytes>
driver hot: <bytes> of <capacity>
driver boot/bulk: <bytes> of <capacity>
common: used <bytes> of <capacity> bytes (code <n>, data <n>) | ceiling <n>
```

- **fixed:** `required` is the fixed content of the `fixed-required`
  objects, `overflow` the rest that links there, `stacks` the two stacks
  (768), and `room = 16384 - required - overflow - stacks`.
- **cold:** the cold code and its end, `_WINDOW_DATA` and its derived base,
  and `room = 32768 - code - data`.
- **driver hot, driver boot/bulk:** the bank-6 tiers, from the driver plan.
- **common:** the packed `_COMMON` + `_COMMON_DATA` run. `capacity` is
  `E000h-FCFFh` (7,424 bytes); `ceiling` is `KOS_COMMON_CEILING` (3,072),
  which `used` may not exceed and which is never raised. The absolute IRQ
  reservations at `FD00h-FE87h` are not part of the run.

Current production figures:

```text
nucleus fixed: required 8363 bytes | overflow 2 bytes | stacks 768 | room 7251 bytes
nucleus cold: code 23861 bytes end BD35h | data 1527 bytes at BE00h | room 7177 bytes
driver hot: 22578 bytes of 32768
driver boot/bulk: 2186 bytes of 24576
common: used 2373 of 7424 bytes (code 1526, data 847) | ceiling 3072
```

The gate stack is 384 bytes; the deepest measured chain uses 275, and the
worst case must stay at or below 360. The driver stack's measured
high-water mark is 278 of 512 bytes.

## 5. Common memory

### 5.1 Routines

Everything that changes the mapping, or runs right after a change, is
common code:

```text
_boot_common_entry        boot entry, mode 10 -> 11
__kos_crt0_init           C runtime: copy initialized data, zero BSS, static init
__kos_mode_fault          latch assertion failure
kos_enter_os_mode         mode 10 -> 11
kos_leave_os_mode         mode 11 -> 10
kos_os_continuation       continuation after a transition
kos_syscall_entry         the RST 30h gate
irq_ctc0_stub             IM2 source stub
irq_ff_unexpected         the vector-FFh and default target
irq_common_entry          the common IRQ gate
_irq_arch_lock/_unlock    IFF snapshot and restore
_irq_arch_init            fills the IM2 page, I = FDh, IM2, EI
_platform_memctl_*        latch read and write
_platform_bank_get/_set   bank-field read and write
_early_console_*          the early serial console
_panic                    the fatal path
_rom_access_copy          the ROM-access primitive
_kos_process_launch       process entry (depth-indexed continuation)
_kos_process_resume       parent resume
_kos_process_continuation_size   gate-stack bytes in use (exec preflight)
kos_process_return_trap   a program that returns instead of exiting
the copy primitives       the path-B aperture stub and the window copy
```

The syscall dispatcher (`kos_syscall_dispatch`) is cold code; only the
three-byte vector is in page zero. The early console and panic path,
strings included, are entirely common, so they work from any mapping.

### 5.2 Transition state

The syscall gate (single depth):

```text
kos_gate_saved_sp      2 bytes    the caller's SP
kos_gate_saved_latch   1 byte     the caller's whole latch
kos_gate_result_hl     2 bytes    result staging
kos_gate_result_a      1 byte     error staging
kos_gate_result_c      1 byte     carry staging
kos_gate_stack       384 bytes    the gate stack
```

Nested exec makes the process side depth-indexed, because a child runs on
the same gate stack:

```text
kos_proc_cont          8 bytes    launcher SP per depth (2 x 4)
kos_proc_ix            8 bytes    launcher IX per depth
kos_proc_latch         4 bytes    launcher latch per depth
kos_proc_iff           4 bytes    launcher IFF per depth
kos_proc_gate_sp       8 bytes    shadowed gate SP per depth
kos_proc_gate_latch    4 bytes    shadowed gate latch per depth
kos_proc_gate_iff      4 bytes    shadowed gate IFF per depth
kos_proc_frames      144 bytes    3 x 48-byte saved gate frames
```

`KOS_EXEC_FRAME_SAVE` is 48 bytes against a measured 32-byte frame; the
exec preflight refuses a deeper frame before anything is allocated. This
state is common because the resume gate restores it while it changes the
mapping and switches stacks.

The ROM-access block:

```text
_rom_access_staging     128 bytes  bounded copy staging
rom_access_saved_latch    1 byte   the caller's exact latch
rom_access_saved_iff      1 byte   the caller's interrupt state
rom_access_page/offset/length      the validated request
rom_access_busy           1 byte   non-reentrancy flag (.db 0: .ds bytes are FFh in ROM)
```

The ROM-access block has no stack, because mode 00 reads ROM at every
address. Its transition is the fixed mirrored block:

```text
rom_access_trampoline   EEFEh   OUT (00h),A: enter mode 00
rom_access_sync         EF00h   LDIR ROM -> staging; EX AF,AF'; OUT (00h),A
rom_access_sync_end     EF08h   end of the stub mirrored into pages 2-7
```

Acceptance images that need it add a small self-test block to common data;
production carries none.

## 6. IRQ reservations

```text
_IRQ_VECTOR        FD00h-FE00h   the IM2 page and the guard byte; the word at
                                 FDFFh-FE00h must equal irq_ff_unexpected
_IRQ_COMMON_STATE  FE01h-FE07h   saved latch, saved SP, common SP, active source
                   FE08h-FE87h   the IRQ transition stack
_IRQ_ISR_STACK     5F00h-5FFFh   the handler stack; initial SP = 6000h
```

`irq_arch_init()` fills the page at run time: every vector targets
`irq_ff_unexpected` except the claimed sources' stubs.

## 7. Banking primitives

`kernel/memory/banking.asm`, common code:

```c
uint8_t platform_memctl_read(void);            /* the latch                  */
void    platform_memctl_write(uint8_t value);  /* raw write, gates only      */
uint8_t platform_bank_get(void);               /* the bank field             */
void    platform_bank_set(uint8_t bank);       /* keeps D3-D7; rejects bank 7 */
```

Mode changes are assembly gates with common continuations, never C
functions: a C return address in `2000h-DFFFh` would disappear when the
mapping changes.

## 8. Checks

- `tools/pack_image.py`: the 512 KiB image size, `FFh` fill, out-of-region
  bytes, the reset-copier mirror, the ROM-access stub and its mirrors in
  pages 2-7, entry-point regions, the `FFh`-vector guard identity, and no
  latch writes from bank 7 outside the upcall shim.
- `tools/mapcheck.py`: area ownership, both derived bases, the residency
  classification, common placement of the gates, console, panic and IRQ
  code, the stack and gate-stack sizes, the ROM-access placement, the IM2
  page, guard and state addresses, page zero, and the common ceiling.
- `tests/test_source_layout.py`: constant parity between the C and assembly
  descriptions of the platform, and that `platform/zephyr80/` holds no
  executable code.
- At boot the kernel checks that it runs in mode 11 over bank 0 (an
  assembly read-back and a C check in `kernel_main()`); a failure prints on
  the early console and halts.

## 9. Constants

Every boundary and latch bit is defined once for C and once for assembly,
with parity checks:

| file pair | holds |
|---|---|
| `platform/zephyr80/public/hardware.h`, `.inc` | ports, register constants, latch fields |
| `platform/zephyr80/private/memory.h`, `.inc` | region geometry, the stacks, the ROM and store geometry |
| `platform/zephyr80/private/irq.h`, `.inc` | the IM2 page and the IRQ transition wiring |

`platform/zephyr80/config.mk` carries the include paths and file names.
