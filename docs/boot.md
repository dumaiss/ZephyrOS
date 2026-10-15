# KonaOS Boot

The boot image, the reset and copy sequence, the mapping at kernel entry,
the boot sequence, and how to check a board. The rules are in
`docs/architecture.md` section 3.

## 1. The boot image

One 512 KiB image of eight 64 KiB pages:

```text
00000h-0FFFFh   ROM page 0   copied to SRAM bank 0 at reset
10000h-1FFFFh   ROM page 1   copied to SRAM bank 7 at reset
20000h-7FFFFh   ROM pages 2-7: the recovery store (read in place, never copied)
```

Every byte that is not linked content or store data is `FFh`. Page numbers
are independent of SRAM bank numbers: page 1 is the OS page.

Per page (checked by `tools/pack_image.py`):

```text
ROM page 0:
    0000h-002Fh   the stackless reset copier (below the RST 30h vector)
    0030h-DFFFh   FFh
    E000h-FEFFh   common code and data (physical SRAM bank 0)
    FE88h-FFFFh   FFh (the common stack, written only at run time)

ROM page 1:
    0000h-002Fh   a byte-identical copy of the reset copier
    0030h-1FFFh   FFh (the caller window; bank 7 is not visible there)
    2000h-...     the bank-7 loadable image: fixed code, then cold code
    ...-DFFFh     FFh (runtime data and stacks, never loaded)
    E000h-FFFFh   FFh (not visible in mode 11)

ROM pages 2-7:
    0000h-BFFFh   the store's logical window (docs/romfs-format.md)
    EEFEh-EF07h   the mirrored ROM-access stub
    C000h-FFFFh   FFh otherwise
```

The page-1 copier mirror exists because instruction fetch continues while
the ROM-page latch changes: the copier's bytes must be identical on both
pages. The store pages' stub mirror exists for the same reason, because
the ROM-access trampoline switches to mode 00 and the next fetch comes from
the selected page.

## 2. Reset and copy

At reset `PC = 0000h` and the latch is `00h` (page 0, mode 00, bank 0). In
mode 00 every read, including instruction fetch, comes from ROM while
writes go to SRAM, so a stackless full-page `LDIR` copies a page.

```text
latch   meaning                     effect
00h     page 0, mode 00, bank 0     page 0 visible (already so at reset)
--      LDIR 64 KiB                 ROM page 0 -> SRAM bank 0
27h     page 1, mode 00, bank 7     the next fetch comes from page 1's mirror
--      LDIR 64 KiB                 ROM page 1 -> SRAM bank 7
10h     mode 10, bank 0             execution continues in SRAM bank 0
--      JP _boot_common_entry       common memory, mode 10
18h     mode 11, bank 0             bank 7 visible at 2000h-DFFFh
```

`_boot_common_entry` (common code):

1. sets `SP = 0000h` (the stack grows down through `FE88h-FFFFh`);
2. enters mode 11 over bank 0;
3. reads the latch back and checks mode 11 over bank 0; on failure it
   prints `PANIC: banking latch assertion failed` and halts;
4. initializes the early console;
5. runs the C runtime setup (section 4);
6. calls `kernel_main()` in bank 7, which re-checks the latch, prints the
   banner and continues the boot.

Interrupts stay disabled through all of this. The first `EI` happens when
the boot program is launched.

If `kernel_main()` returns, the boot entry prints `PANIC: kernel_main
returned` and halts.

## 3. The boot sequence

The banner:

```text
KonaOS 0.0
platform: Zephyr-80
kernel entry: OK
serial: OK
```

Each line ends with CRLF. Then `kernel_main()`:

1. **`irq_init()`**: the IM2 executive (the vector page, the IRQ stacks,
   the IRQ source table).
2. **`vfs_init()`**: the ROMFS reader validates the store and the `/system`
   provider is usable; the handle table and the in-process FILE endpoint are
   set up. A corrupt store prints the `romfs_diagnose()` lines and panics.
3. **`driver_load_all("drivers.man")`**: reads the boot manifest with the
   ROMFS reader and, in load order, copies each driver's segments to their
   bank-6 addresses, installs its endpoint, publication and dependency
   records, runs its boot entry once and its hot init once. The SIO driver
   loads first, so a later failure still has the serial console. A driver
   failure is a boot failure on the early console.
4. **The boot launcher** resets the console
   (`KCONSOLE_OP_RESET`: terminal defaults, a clean screen, input dropped)
   and runs the configured boot program (the system file's `boot`,
   compiled in through the generated `boot_program.h`). The system file's
   `after_exit` setting decides what follows a normal exit:

   | event | action |
   |---|---|
   | the boot program exits with a status | print `boot: <path> exit <n>`; `relaunch` starts it again, `monitor` starts `/system/monitor.exe` |
   | the run cannot start (missing, rejected, fault) | print `boot: <path> failed <code>` and start `/system/monitor.exe` |
   | the monitor exits | start the boot program again |
   | the monitor itself cannot start | the controlled halt |

   Production boots the shell and relaunches it; the acceptance images run
   their program once and end at the monitor. A warm reboot re-enters this
   sequence at step 1 with the console reset.

Acceptance images may add test code at a single marked seam in this
sequence (`TEST-ONLY HOOK`); production compiles none of it.

## 4. Mapping at kernel entry

```text
FFFFh +-------------------------------+
      | bank 0 common                 |  FE88h-FFFFh: the common stack
E000h +-------------------------------+
      | bank 7 cold half    32 KiB    |  visible while the window shows bank 7
6000h +-------------------------------+
      | bank 7 fixed core   16 KiB    |  always visible in mode 11
2000h +-------------------------------+
      | bank 0 (caller)      8 KiB    |
0000h +-------------------------------+
```

The linker places `_CODE` at `2000h`, `_WINDOW` at `6000h` and common at
`E000h`, and derives the two runtime-data bases (`docs/memory.md` section
2.1). The loadable image never includes the runtime data.

## 5. C runtime setup

1. Zero both runtime areas: `_DATA` (fixed core) and `_WINDOW_DATA` (cold
   half).
2. Copy the `_INITIALIZER` image to `_INITIALIZED`.
3. Run the compiler's static initializers (`_GSINIT`, ended by `_GSFINAL`).

No hosted runtime or library is linked. Calls between C and assembly are
internal, not a KonaOS ABI.

## 6. The early serial console

SIO channel B (data `22h`, control `23h`), 115,200 baud 8N1, polled,
interrupts off:

```text
WR0 = 18h   channel reset
WR4 = 44h   x16 clock, 1 stop bit, no parity
WR3 = C1h   receiver on, 8 bits
WR5 = EAh   transmitter on, 8 bits, DTR, RTS
WR1 = 00h   interrupts off
```

Transmit polls RR0 for TX-empty (`04h`) with a finite timeout, so a missing
terminal cannot hang the kernel. The console does not depend on the board's
hardware flow control. All of it, strings included, is common memory, so it
works during mapping changes and kernel failures.

## 7. Build outputs

Everything is generated under `build/` (or the image's own build
directory):

```text
konaos.ihx              linker output
konaos.map              the link map
page0.bin ... page7.bin the eight ROM pages
konaos.bin              the 512 KiB image
zephyr80.bin            an identical copy for MAME
konaos.manifest.json    sizes, page hashes, store placement, ledgers
recovery.romfs          the store stream
store.json              the store manifest (from tools/systemgen.py)
modules/*.mod           the packed driver images
modules/drivers.man     the boot manifest
```

`tools/systemgen.py` generates the store manifest from the image's system
file. `tools/romfspack.py` packs the store. `tools/pack_image.py` places
logical store offset `L` at offset `L % C000h` of page `2 + L / C000h`,
leaves the boot pages alone, and fails the build on any byte linked outside
its region, a key entry point outside its region, a store over capacity,
a malformed `RST 30h` vector, conflicting IHX records or a latch write
from bank 7 outside the upcall shim. `tools/mapcheck.py` runs after every
link. `make check` proves a clean rebuild reproduces the image.

## 8. Checking a board

`tools/serial_smoke.py` opens SIO channel B (115,200 8N1) with a bounded
timeout and checks the transcript of one image:

| command | image | checks |
|---|---|---|
| `make smoke-hardware` | production `build/konaos.bin` | banner, drivers, shell prompt |
| `make smoke-hardware-hwtest` | production | types `hwtest` at the shell, checks every stage |
| `make smoke-hardware-subsystem` | `subsystem-selftest` | the subsystem acceptance |
| `make smoke-hardware-console-serial` | `console-serial-selftest` | the serial console build |
| `make smoke-hardware-fs` | `fs-selftest` | the file outcome image (needs a writable card) |
| `make smoke-hardware-shell` | `console-serial-selftest` | `pwd`, `ls /system`, `hwtest`, `exit` at the shell |

Each target prints the image path and its SHA-256: flash that image, start
the tool, then press RESET (the acceptance images wait for the tool's
host-sync sequence). `make hardware-images` builds every image.
`SERIAL_PORT=` names the device (or `KOS_SERIAL_PORT` and
`KOS_SERIAL_BAUD`). Host-only targets never touch a serial port.

Manual check of a new ROM:

1. `make clean && make check`; `build/konaos.bin` is 524,288 bytes.
2. Program it into the ROM.
3. Connect a 115,200 8N1 terminal to SIO channel B.
4. Reset: the banner, the driver lines and the shell prompt appear.
5. Record the image SHA-256, the programmer, the serial adapter and the
   transcript.

## 9. Emulators

The physical board is the reference. The simulator (`tests/z80sim.py`)
replays images for the host tests. The current Zephyr-80 MAME driver does
not implement the mode-10/11 mapping KonaOS needs, so it cannot run the
image yet; `zephyr80.bin` is produced for when it can.
