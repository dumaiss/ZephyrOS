# KonaOS Boot

**Scope:** the reset/ROM-copy sequence, memory map, early console, and
image layout in this document are the current, unchanged boot path.  The
M0 acceptance checklist is the historical M0 record; since then
`kernel_main()` continues past the banner into the current boot
(section 2.1).  The retained historical `HALT` banner line was dropped
under the Mn-1 retention rule (work package 25a): the current banner ends
at `serial: OK`.

This document describes the Zephyr-80 boot image, the reset/ROM-copy
sequence, the memory mapping at kernel entry, the build output, and how to
validate the result in the emulator and on physical hardware.

## 1. Boot image format

The KonaOS boot ROM is a single 512 KiB image of eight 64 KiB pages:

```text
offset          content
00000h-0FFFFh   ROM page 0   (copied to SRAM bank 0 by the reset copier)
10000h-1FFFFh   ROM page 1   (copied to SRAM bank 7 by the reset copier)
20000h-7FFFFh   ROM pages 2-7: ROMFS recovery store (read-only, mapped
                on demand; never copied to SRAM)
```

All eight pages are exactly 64 KiB; every byte that is not linked
content or recovery-store data is filled with `FFh`.  ROM page numbering
is independent of SRAM bank identity; page 1 is the KonaOS OS page,
not "page 7".

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
    2000h-data base  bank-7 loadable kernel image (ends at the derived
                     fixed code end; the derived area below is not loaded)
    data base-DFFFh  FFh (bank-7 runtime data plus ISR stack; never loaded)
    E000h-FFFFh   FFh (not visible in mode 11)

ROM pages 2-7 (recovery):
    0000h-BFFFh   logical ROMFS window (see docs/romfs-format.md)
    EF00h-EF07h   mirrored ROM-access stub (mode-00 fetch stream; the
                  common copy is linked at the same address)
    C000h-FFFFh   FFh otherwise (never part of the logical stream)
```

The recovery-page stub mirror exists for the same reason as the page-1
copier mirror: the ROM-access trampoline switches to mode 00, and the next
instruction fetch comes from the selected ROM page, so the bytes at
`EF00h-EF07h` must be identical in every recovery page and in common SRAM.

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
   exercises the banking primitives, and prints the banner.

The reset/copy sequence and the early kernel-entry phase run with
maskable interrupts disabled.  The M2 interrupt executive is
initialized later in `kernel_main()` (section 2.1); the monitor EXE
launch is the first path that enables interrupts (`EI`), so `EI` is not
executed anywhere in the reset path itself.

The implemented linker-enforced memory ownership table is in
`docs/memory.md`.

### 2.1 Current boot (post-banner)

After the four banner lines, `kernel_main()` continues:

1. `irq_init()` establishes the M2 IM2 executive (kernel-owned vectors,
   IRQ stacks, and source registry).
2. `vfs_init()` brings up the three static roots: the ROMFS reader (the
   `/system` FILE endpoint and the store), the handle table and the
   in-process FILE endpoint; a corrupt store produces the
   `romfs_diagnose()` lines and the panic.  Work package 42d folded that
   outcome into `tests/test_romfs_sim.py`, which patches the header of a
   freshly packed store image in place and replays the panic; the
   corrupt-store image and its target are gone.
3. `driver_load_all(KOS_DRIVER_MANIFEST_NAME)` reads `drivers.man`
   with the bootstrap ROMFS reader and, in load order, copies each
   driver image's segments to their host-assigned bank-6 addresses,
   installs the endpoint and publication records and the resolved
   dependency numbers, runs the boot entry once and the hot init once
   (the SIO serial driver loads first, so a later failure still leaves
   the recovery console).
4. the monitor launcher `process_exec("/system/monitor.exe")` starts the
   native monitor and re-executes it after every normal exit; that launch
   is where interrupts are first enabled.

The M9 steps this replaced (`device_registry_init`,
`module_loader_init`, one VFS open per boot module, `vfs_mount_all`) are
recorded in `docs/vfs.md` sections 5 and 13.

Acceptance builds interpose the `TEST-ONLY HOOK` seam here; see
`docs/devices.md` section 3 and `docs/architecture.md` section 30.

## 3. Memory map at kernel entry

```text
FFFFh +-------------------------------+
      | bank 0 common        8 KiB    |
FF00h +- - - - - - - - - - - - - - - +  common boot/runtime stack (SP grows down)
      | bank 0 common code/data      |
E000h +-------------------------------+
      | bank 7 cold half    32 KiB    |  mode 11, window on bank 7 (P = 0)
6000h +-------------------------------+
      | bank 7 fixed core   16 KiB    |  mode 11, always visible
2000h +-------------------------------+
      | bank 0 (caller)      8 KiB    |
0000h +-------------------------------+
```

- `0000h-1FFFh`: physical SRAM bank 0 (caller window).
- `2000h-5FFFh`: physical SRAM bank 7, the fixed core (code that runs
  while the window shows another bank, its data, the driver stack and the
  ISR stack).
- `6000h-DFFFh`: physical SRAM bank 7, the cold half (visible while the
  window shows bank 7): the linked cold code, then the cold runtime data. The driver arena M9 reserved between them is deleted (work package 42c).
- `E000h-FFFFh`: physical SRAM bank 0 (permanent common region).

The linker control file fixes `_CODE` at `2000h` and `_WINDOW` at
`6000h`, and derives the two zero-initialized runtime areas: `_DATA` at
`align_down(KOS_DRIVER_STACK_BASE - runtime_data_size, 512)` and
`_WINDOW_DATA` at `align_up(cold_code_end, 512)`; the common segment
stays at `E000h`.  The loadable bank-7 image (fixed code, `_HOME`,
`_INITIALIZER`, `_GSINIT`, `_GSFINAL`, then the cold `_WINDOW` code) must
fit the window; the derived areas and the runs are never loaded from
ROM.

## 4. SDCC C runtime initialization

Implemented contract (not the "fail if non-empty" alternative):

1. both zero-initialized runtime areas are zeroed using the
   linker-synthesized symbols: `_DATA` (`s__DATA`/`l__DATA`) for the fixed
   core and `_WINDOW_DATA` (`s__WINDOW_DATA`/`l__WINDOW_DATA`) for the
   cold half (uninitialized globals and function-local statics);
2. the `_INITIALIZER` ROM image is copied to `_INITIALIZED`
   (`s__INITIALIZER`/`s__INITIALIZED`/`l__INITIALIZER`);
3. compiler-emitted `_GSINIT` code (static initializers) is executed via
   the `__kos_gsinit` label at the head of the `_GSINIT` area, with the
   `_GSFINAL` `ret` terminating it.

No hosted CRT or standard library is linked; only the five freestanding
objects are.  Calls between C and assembly are internal implementation
details, not a KonaOS ABI.

## 5. Early serial console

SIO channel B (data `22h`, control `23h`), 115200 8N1, polled, interrupts
disabled:

```text
WR0 = 18h  channel reset
WR4 = 44h  x16 clock, 1 stop bit, no parity
WR3 = C1h  8-bit RX, RX enabled, Auto Enables off
WR5 = EAh  TX enabled, 8-bit TX, DTR asserted, RTS asserted
WR1 = 00h  interrupts disabled
```

Transmit polls RR0 until TX-empty (`04h`) is set, with a finite timeout
(`0xFFFF` polls) so a missing terminal cannot hang the kernel; the console
is not gated by the board's FT230X CTS/RTS crossover.  The whole console
path — code and strings — is linked into the common segment and is
therefore available during mode/bank transitions and kernel failures.

## 6. Banner output

```text
KonaOS 0.0
platform: Zephyr-80
kernel entry: OK
serial: OK
```

Each line is CRLF-terminated.  The historical M0 transcript carried a
literal `HALT` line after `serial: OK`; it was a marker only (`kernel_main`
never halted there) and was removed under the Mn-1 retention rule, so
every expected transcript now ends the banner at `serial: OK`.  The live
console then shows the module/console lines and the monitor
(section 2.1).  If `kernel_main()` returns, the boot entry prints
`PANIC: kernel_main returned` and enters the halt loop.

## 7. Build artifacts

Everything is generated under `build/` (git-ignored):

```text
build/recovery.romfs  logical ROMFS recovery stream (pages 2-7 payload)
build/modules/sio.mod  packed SIO driver module (unchanged M4 format)
build/konaos.ihx    sdldz80 Intel HEX output
build/konaos.map    linker map (symbols and area placement)
build/page0.bin ... page7.bin   the eight ROM pages, 64 KiB each
build/konaos.bin    512 KiB boot image (page 0..7, FF-filled)
build/zephyr80.bin    byte-identical copy for MAME
build/konaos.manifest.json  sizes, per-page hashes, ROMFS placement
```

`tools/romfspack.py` builds and validates `build/recovery.romfs` from
`build/store.json`, the store manifest `tools/systemgen.py` generates for
the image's system file (see `docs/romfs-format.md`).
`tools/pack_image.py` validates the layout, places the recovery stream at
logical offset `L` into offset `L % C000h` of page `2 + L / C000h`, keeps
the boot pages untouched, and fails the build if any byte is linked
outside its assigned region, if a key entry point symbol is outside its
required region, or if the recovery stream exceeds the declared capacity.
It also verifies the page-zero `RST 30h` vector (`JP kos_syscall_entry`,
exactly three bytes at `0030h`), rejects conflicting/ambiguous IHX
records and any latch-write encoding in bank 7, and emits
`build/konaos.manifest.json`.  `tools/mapcheck.py` runs automatically on
every build as the authoritative post-link layout check, deriving all
boundaries from `platform/zephyr80/private/memory.inc`; `make check` proves a
clean rebuild reproduces the identical binary.

### Syscall self-test image

`make syscall-selftest` builds a second image
(`build-selftest/konaos-selftest.bin`, with a `zephyr80-selftest.bin`
MAME alias) that adds the target-side smoke caller.  On hardware it prints
the normal banner followed by `syscall selftest: OK` after exercising
`RST 30h` from application banks 0 and 6 (or a precise failure line
otherwise).

## 8. Serial smoke capture (physical hardware)

`tools/serial_smoke.py` opens the SIO0/B link (115200 8N1, raw) with a
bounded timeout and verifies the expected output:

```sh
make smoke-hardware SERIAL_PORT=/dev/ttyUSB0                 # flash build/konaos.bin;
                                                             # on an SIO-fallback
                                                             # bench only: banner,
                                                             # console line, monitor
make smoke-hardware-subsystem SERIAL_PORT=/dev/ttyUSB0       # flash build-subsys-selftest/
                                                             # konaos-subsys-selftest.bin
make smoke-hardware-console-integration SERIAL_PORT=/dev/ttyUSB0              # flash build-console-integration-selftest/
                                                             # konaos-console-integration-selftest.bin
make smoke-hardware-m9-fs SERIAL_PORT=/dev/ttyUSB0           # flash build-m9-fs-selftest/
                                                             # konaos-m9-fs-selftest.bin
make smoke-hardware-m9-exec SERIAL_PORT=/dev/ttyUSB0         # flash build-m9-exec-selftest/
                                                             # konaos-m9-exec-selftest.bin
make smoke-hardware-m9-iocfat SERIAL_PORT=/dev/ttyUSB0       # flash build-m9-iocfat-selftest/
                                                             # konaos-m9-iocfat-selftest.bin
                                                             # (with a writable FAT card)
make smoke-hardware-m9 SERIAL_PORT=/dev/ttyUSB0              # flash build-m9-selftest/
                                                             # konaos-m9-selftest.bin
                                                             # (with a writable FAT card);
                                                             # the whole M9 acceptance
```

Each smoke mode checks one specific image; build it, flash it, then run the
mode (the smoke target prints the exact image and sha256, and `make
hardware-images` builds them all).  The console/subsystem images wait for
the tool's host-sync sequence, so start the tool first and then press RESET.

Every acceptance image retired under the Mn-1 retention rule (M1-M8)
is gone; their mechanism coverage now runs inside the subsystem
acceptance image (`make subsystem-selftest`), the console-integration
image (`make console-integration-selftest`, which carries the IOC, HID,
V9958 and console diagnostics since work package 42d), the
`console-serial` variant (`make console-serial-selftest`) and the M9
acceptance images.

M9's integration acceptance image is `make m9-selftest`: the production
boot path with `/` served by the resident IOC-FAT provider from the card,
running the whole native acceptance (`fstest.exe`, its deep-matrix child
`fsfat.exe`, the `fsdeep.exe` depth probe and the two fault windows) and then
the ordinary monitor launcher.  Its matrix and transcripts are in
[`docs/validation/2026-10-05-m9-filesystem.md`](./validation/2026-10-05-m9-filesystem.md).

The device can also be passed via the `KOS_SERIAL_PORT` / `KOS_SERIAL_BAUD`
environment variables.  A missing device or incomplete output fails
clearly; ordinary host-only `make test` never touches the serial port.
`make clean` removes only build artifacts — never serial devices or
captured evidence.

## 9. Emulator validation (MAME)
The currently available Zephyr-80 MAME driver is **not configured for the
KonaOS architecture**: its banking decode does not implement the mode
`10`/`11` mapping required here (bank 7 is not presented at `2000h-DFFFh`
in OS mode), so it cannot provide M0 acceptance evidence.  Emulator
validation is deferred until a driver matching this architecture is
available; M0 acceptance therefore rests on the physical test below.

## 10. Physical-ROM test checklist

This is the historical M0 bring-up checklist; the current M8 physical
acceptance procedure and evidence live in
`docs/validation/2026-10-03-m8-device-stack.md` and
`docs/devices.md` section 8.2.

Prerequisite: EPROM/Flash programmer with a 512 KiB (or larger) image
compatible with the Zephyr-80 ROM socket.

1. `make clean && make && make test` — all host tests pass;
   `build/konaos.bin` is exactly 524288 bytes.
2. Program `build/konaos.bin` into the ROM device.
3. Connect a 115200 8N1 serial terminal to SIO0/B (data `22h`).
4. Apply power / assert reset.
5. Expected: the four-line banner, then the console-driver line
   `console: dynamic sio0.b OK` (the SIO driver is loaded from the ROMFS
   recovery store through the VFS), and then the monitor launch.
6. Record: image SHA-256, programmer used, serial adapter used, and the
   captured terminal output.

M0 is fully accepted only after the banner is observed on physical
hardware.  Until then the emulator run is the recorded acceptance evidence.

### Recorded result

- **2026-10-02 — M0 physical test executed: PASS.**  The then-current
  five-line banner (ending in the historical `HALT` marker, since removed)
  was observed on the Zephyr-80 hardware and the M0 machine entered the
  halt loop.  M0 is accepted; the current image continues past that
  banner.
- **2026-10-02 — M1 re-flash executed: PASS.**  Same banner on hardware
  after the M1 banking changes (boot assertion, bank primitives, common
  state block) — no boot regression.  M1 memory banking is accepted.
- **2026-10-02 — syscall selftest executed: PASS.**  The self-test image
  printed `syscall selftest: OK` on hardware after the gate fix (caller
  `BC` preservation) and the fail-code reporting fix.  The `RST 30h`
  round trip passes in application banks 0 and 6.  M1 syscall scaffold is
  accepted.
