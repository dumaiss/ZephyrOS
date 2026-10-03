# ZephyrOS Boot (M0)

This document describes the Zephyr-80 boot image, the reset/ROM-copy
sequence, the memory mapping at kernel entry, the build output, and how to
validate the result in the emulator and on physical hardware.

## 1. Boot image format

The M0/M1 boot ROM is a single 128 KiB image:

```text
offset        content
00000h-0FFFFh ROM page 0   (copied to SRAM bank 0 by the reset copier)
10000h-1FFFFh ROM page 1   (copied to SRAM bank 7 by the reset copier)
```

Both pages are exactly 64 KiB; every byte that is not linked content is
filled with `FFh`.  ROM page numbering is independent of SRAM bank identity;
page 1 is the ZephyrOS OS page, not "page 7".

Region contract per page (enforced at build time by `tools/pack_image.py`):

```text
ROM page 0:
    0000h-002Fh   stackless reset copier (below 0030h; M1 reserves RST 30h)
    0030h-DFFFh   FFh (unused in bank 0's view)
    E000h-FEFFh   permanent common code/data (physical SRAM bank 0)
    FF00h-FFFFh   FFh (common boot/runtime stack; written at run time only)

ROM page 1:
    0000h-002Fh   byte-identical copy of the reset copier (mirrored)
    0030h-1FFFh   FFh (caller-window gap; bank 7 is invisible there)
    2000h-BFFFh   bank-7 loadable kernel image (must end below C000h)
    C000h-DFFFh   FFh (bank-7 zero-initialized runtime area, never loaded)
    E000h-FFFFh   FFh (not visible in mode 11)
```

The page-1 copier mirror exists because instruction fetch continues while
the ROM-page latch changes: the bytes at the copier addresses must be
identical on both pages, otherwise the CPU executes garbage mid-copy.  The
same reasoning applies after the final latch change to `10h`, when execution
continues from the copy in SRAM bank 0.

## 2. Reset and copy sequence

Reset state: PC = `0000h`, banking latch = `00h` (ROM page 0, mode 00,
bank 0).  In mode `00`, reads (including instruction fetch) come from the
selected ROM page while writes target the selected SRAM bank, which makes a
stackless full-page `LDIR` copy possible.

```text
latch value   meaning                            effect
00h           page 0, mode 00, bank 0            canonicalize; page 0 visible
                                                  (already active at reset)
--            LDIR 64 KiB 0000h->0000h           ROM page 0 -> SRAM bank 0
27h           page 1, mode 00, bank 7            page 1 selected; the next
                                                  fetch comes from the
                                                  mirrored copier in page 1
--            LDIR 64 KiB 0000h->0000h           ROM page 1 -> SRAM bank 7
10h           mode 10, bank 0                    RAM-only application mode;
                                                  execution continues from
                                                  the copier copy in bank 0
--            JP to _boot_common_entry (E000h+)  common region, mode 10
18h           mode 11, bank 0                    OS mode: bank 7 visible at
                                                  2000h-DFFFh, written by
                                                  _boot_common_entry from
                                                  common memory
```

`_boot_common_entry` then:

1. sets `SP = 0000h` (top-exclusive at `10000h`; the stack grows down
   through `FF00h-FFFFh` in permanently mapped bank 0),
2. writes `18h` to port `00h` to enter OS mode over bank 0,
3. reads the latch back and verifies mode `11` over bank 0 (M1 boot-time
   assertion; on failure the common-resident fault path prints
   `PANIC: banking latch assertion failed` and halts),
4. calls `early_console_init()` (common-resident),
5. prepares the SDCC C runtime (below),
6. calls `kernel_main()` in bank 7, which re-checks the latch state,
   exercises the banking primitives, prints the banner, and halts.

Interrupts stay disabled for the entire path; `EI` is never executed.

The implemented linker-enforced memory ownership table is in
`docs/memory.md`.

## 3. Memory map at kernel entry

```text
FFFFh +-------------------------------+
      | bank 0 common        8 KiB    |
FF00h +- - - - - - - - - - - - - - - +  common boot/runtime stack (SP grows down)
      | bank 0 common code/data      |
E000h +-------------------------------+
      | bank 7 OS body      48 KiB    |  mode 11
2000h +-------------------------------+
      | bank 0 (caller)      8 KiB    |
0000h +-------------------------------+
```

- `0000h-1FFFh`: physical SRAM bank 0 (caller window).
- `2000h-DFFFh`: physical SRAM bank 7 (kernel body).
- `E000h-FFFFh`: physical SRAM bank 0 (permanent common region).

The linker script fixes `_CODE` at `2000h`, `_DATA` (zero-initialized
runtime area) at `C000h`, and the common segment at `E000h`.  The loadable
bank-7 image (code, `_HOME`, `_INITIALIZER`, `_GSINIT`, `_GSFINAL`) must fit
below `C000h`; `C000h-DFFFh` is never loaded from ROM.

## 4. SDCC C runtime initialization

Implemented contract (not the "fail if non-empty" alternative):

1. the `_DATA` area (uninitialized globals, function-local statics) is
   zeroed using the linker-synthesized `s__DATA`/`l__DATA` symbols;
2. the `_INITIALIZER` ROM image is copied to `_INITIALIZED`
   (`s__INITIALIZER`/`s__INITIALIZED`/`l__INITIALIZER`);
3. compiler-emitted `_GSINIT` code (static initializers) is executed via
   the `__zos_gsinit` label at the head of the `_GSINIT` area, with the
   `_GSFINAL` `ret` terminating it.

No hosted CRT or standard library is linked; only the five freestanding
objects are.  Calls between C and assembly are internal implementation
details, not a ZephyrOS ABI.

## 5. Early serial console

SIO channel B (data `22h`, control `23h`), 115200 8N1, polled, interrupts
disabled:

```text
WR0 = 18h  channel reset
WR4 = 44h  x16 clock, 1 stop bit, no parity
WR3 = E1h  8-bit RX, RX enabled, Auto Enables on
WR5 = E8h  TX enabled, 8-bit TX, DTR asserted, RTS released
WR1 = 00h  interrupts disabled
```

Transmit polls RR0 until TX-empty (`04h`) and CTS (`20h`) are both set,
with a finite timeout (`0xFFFF` polls) so a missing terminal cannot hang
the kernel.  The whole console path — code and strings — is linked into the
common segment and is therefore available during mode/bank transitions and
kernel failures.

## 6. Expected output

```text
ZephyrOS 0.0
platform: Zephyr-80
kernel entry: OK
serial: OK
HALT
```

Each line is CRLF-terminated.  If `kernel_main()` returns, the boot entry
prints `PANIC: kernel_main returned` and enters the same halt loop.

## 7. Build artifacts

Everything is generated under `build/` (git-ignored):

```text
build/zephyros.ihx   sdldz80 Intel HEX output
build/zephyros.map   linker map (symbols and area placement)
build/page0.bin      ROM page 0, 64 KiB
build/page1.bin      ROM page 1, 64 KiB
build/zephyros.bin   128 KiB boot image (page 0 + page 1, FF-filled)
build/zephyr80.bin   byte-identical copy for MAME
```

`tools/pack_image.py` validates the layout and fails the build if any byte
is linked outside its assigned region or if a key entry point symbol is
outside its required region.  It also verifies the page-zero `RST 30h`
vector (`JP zos_syscall_entry`, exactly three bytes at `0030h`), rejects
conflicting/ambiguous IHX records and any latch-write encoding in bank 7,
and emits `build/zephyros.manifest.json` (sizes, hashes, fill counts,
entry addresses).  `tools/mapcheck.py` runs automatically on every build
as the authoritative post-link layout check, deriving all boundaries from
`platform/zephyr80/memory.inc`; `make check` proves a clean rebuild
reproduces the identical binary.

### Syscall self-test image

`make syscall-selftest` builds a second image
(`build-selftest/zephyros-selftest.bin`, with a `zephyr80-selftest.bin`
MAME alias) that adds the target-side smoke caller.  On hardware it prints
the normal banner followed by `syscall selftest: OK` after exercising
`RST 30h` from application banks 0 and 6 (or a precise failure line
otherwise).

## 8. Serial smoke capture (physical hardware)

`tools/serial_smoke.py` opens the SIO0/B link (115200 8N1, raw) with a
bounded timeout and verifies the expected output:

```sh
make smoke-hardware SERIAL_PORT=/dev/ttyUSB0                 # exact banner
make smoke-hardware-selftest SERIAL_PORT=/dev/ttyUSB0        # + selftest OK
```

The device can also be passed via the `ZOS_SERIAL_PORT` / `ZOS_SERIAL_BAUD`
environment variables.  A missing device or incomplete output fails
clearly; ordinary host-only `make test` never touches the serial port.
`make clean` removes only build artifacts — never serial devices or
captured evidence.

## 9. Emulator validation (MAME)
The currently available Zephyr-80 MAME driver is **not configured for the
ZephyrOS architecture**: its banking decode does not implement the mode
`10`/`11` mapping required here (bank 7 is not presented at `2000h-DFFFh`
in OS mode), so it cannot provide M0 acceptance evidence.  Emulator
validation is deferred until a driver matching this architecture is
available; M0 acceptance therefore rests on the physical test below.

## 10. Physical-ROM test checklist

Prerequisite: EPROM/Flash programmer with a 128 KiB (or larger) image
compatible with the Zephyr-80 ROM socket.

1. `make clean && make && make test` — all host tests pass;
   `build/zephyros.bin` is exactly 131072 bytes.
2. Program `build/zephyros.bin` into the ROM device.
3. Connect a 115200 8N1 serial terminal to SIO0/B (data `22h`).
4. Apply power / assert reset.
5. Expected: the exact six-line banner, then silence (halt loop).
6. Record: image SHA-256, programmer used, serial adapter used, and the
   captured terminal output.

M0 is fully accepted only after the same banner is observed on physical
hardware.  Until then the emulator run is the recorded acceptance evidence.

### Recorded result

- **2026-10-02 — M0 physical test executed: PASS.**  The exact six-line
  banner was observed on the Zephyr-80 hardware and the machine entered
  the halt loop.  M0 is accepted.
- **2026-10-02 — M1 re-flash executed: PASS.**  Same banner on hardware
  after the M1 banking changes (boot assertion, bank primitives, common
  state block) — no boot regression.  M1 memory banking is accepted.
- **2026-10-02 — syscall selftest executed: PASS.**  The self-test image
  printed `syscall selftest: OK` on hardware after the gate fix (caller
  `BC` preservation) and the fail-code reporting fix.  The `RST 30h`
  round trip passes in application banks 0 and 6.  M1 syscall scaffold is
  accepted.
