# The KonaOS Monitor

`monitor.exe` is the interactive recovery and inspection tool, and the
default boot program. It is an ordinary EXE in the ROM store: no special
format, no kernel linkage, no private symbols. It uses only the console
(through its standard handles) and the diagnostic syscalls (`docs/abi.md`
section 9), so the same binary works with either console driver.

## 1. Launch

```text
reset -> kernel boot -> drivers loaded -> "console: driver OK"
  -> boot launcher runs the boot program (production: /system/shell.exe)
     -> the shell prompts; `monitor` runs this program in its own bank
        -> Q -> SYS_EXIT -> the shell prompts again
     -> an acceptance image's monitor policy starts it after that program
        -> the monitor runs in its own primary bank
        -> Q -> SYS_EXIT -> "boot: /system/<program>.exe exit 0"
        -> the launcher runs the boot program again
```

Every start begins on a console reset (`KCONSOLE_OP_RESET`): the terminal
starts on a clean screen and no input the previous program never read
reaches the new one. The same launcher starts the boot program again after
a warm reboot (`B` above), so a reboot and a `Q`/restart look identical
from the console.

The boot program is the `boot` field of the image's system file and the
system file's `after_exit` decides what follows a normal exit (`relaunch`
keeps the program, `monitor` hands the machine to `/system/monitor.exe`,
which an acceptance image uses to end at the monitor after one run).
Whatever the setting, a run that cannot start prints
`boot: <path> failed <n>` and falls back to the monitor; only a monitor
that cannot start reaches the controlled halt. When the launcher started
the monitor itself, its exit returns to the boot program; a monitor run as
the shell's child (or `X`'s) is that program's business instead.

## 2. Commands

The prompt is `Z80> `. Lines hold up to 80 characters and are echoed.
Backspace (`08h`) and DEL (`7Fh`) edit; a character past the limit rings
the bell; CR or LF ends the line (a CRLF's LF is swallowed); other control
bytes are ignored. Command letters and hex digits are case-insensitive. A
missing, malformed, out-of-range or extra argument is answered `ERR: ARG`.

| command | action |
|---|---|
| `H`, `?` | help |
| `R` | system, process, bank and interrupt information |
| `D addr len` | dump the monitor's own bank, 16 bytes per row |
| `DB bank addr len` | dump bank 0-6 |
| `M addr value` | set one byte in the monitor's own bank |
| `MB bank addr value` | set one byte in bank 0-6 |
| `I port` | read I/O port `10h-FFh` |
| `O port value` | write I/O port `10h-FFh` |
| `X path [tail]` | run a program from `/system` or `/fs` and print its exit status |
| `B` | reboot the machine (`SYS_REBOOT`, `docs/abi.md` section 5.5) |
| `Q` | exit to the launcher |

The `R` line `irq:` is the interrupt executive's dispatch report:
`unexpected` counts interrupts that reached the gate with no source the
executive could name, `unclaimed` those whose source no driver had
claimed (they are quiesced, `docs/devices.md`), and `last` is the source
id of the most recent dispatch - `IRQ_CTC2` (`02`) when the clock is
running, and the executive's "nothing yet" value `07` before any
interrupt.  It is the line to read when a handler that should run does
not.

`X` runs the program through the ordinary `SYS_EXEC` path, waits for it,
prints `exec: exit status 0xssss` (or `ERR: <code>` when the exec fails)
and prompts again. The tail reaches the child byte-exact.

Limits, checked by the monitor and again by the kernel: writes need
`0100h-DFFFh` (page zero is protected), reads allow `0000h-DFFFh`, banks
must be in the kernel-reported mask (`7Fh`, so bank 7 is never
selectable), and ports `00h-0Fh` (the latch alias) are refused without
being touched.

```text
KonaOS native monitor
bank=00 pid=0 scratch=0F19
Z80> R
system: abi 01 app 10 os 18 common E000 kernel-bank 07 romfs 02
process: pid 0 bank 0 flags 0001
bank: current 00 mask 7F
irq: unexpected 0000 unclaimed 0000 last 02
Z80> D 0100 10
0100: 7C B5 20 0D CD 4D 06 21 03 02 CD E5 0D F3 76 18  |. ..M.!......v.
Z80> DB 0 0030 1
0030: C3                                               .
Z80> M 0F19 A5
OK
Z80> D 0F19 1
0F19: A5                                               .
Z80> I 20
IN=FF
Z80> O 00 18
ERR: ARG
Z80> DB 7 0100 1
ERR: ARG
Z80> Q
/>
```

(The last line is the shell's prompt: the monitor was found in `/system`,
`Q` returned from it, and the shell prompted again. When the launcher
started the monitor itself - an acceptance image, or the boot program
being the monitor - the launcher prints `boot: /system/monitor.exe exit 0`
instead and starts it again.)

The banner's `scratch=` is the address of a one-byte scratch variable in
the monitor's own data: `M` and `D` on it exercise the write and readback
path without touching live state (the smoke tool reads the address from
the banner).

Not provided: Intel HEX load and export, `G`, and CP/M register snapshots.

## 3. The serial command lane

When the image publishes `/dev/serial/0`, the monitor binds it through the
SERIAL protocol and polls it beside the console. A line can be typed from
either source; the console is polled first, and each byte is echoed back to
the source it came from. So:

- on the serial-console build (both sources are the same stream) each byte
  appears once;
- on the production image (HID input, V9958 output) a terminal or a host
  tool can type commands over serial and see them echoed there, while the
  output appears on the V9958. The monitor writes nothing else to the lane.

An image without `/dev/serial/0` is console-only.

## 4. The production exerciser

`X /system/hwtest.exe` runs, in order: extended memory and far code; files
(every write confined to `/fs/HWTEST`, which it creates and removes, even
after a failure); the monotonic clock (its INFO constants, a 100 ms wait
that is bounded against a stopped clock, and a rate check against CPU time
of a calibrated length, so a wrong control word fails on the board too);
the namespace and handles against the real devices.

```text
Z80> X /system/hwtest.exe
hwtest: begin
hwtest: xmem+far PASS
hwtest: files PASS                 # or: card absent / card not writable
hwtest: timer PASS
hwtest: namespace+handles PASS
hwtest: 4/4 PASS                   # + " (card absent)" with no card
exec: exit status 0x0000
```

Everything it runs is in the store, so the card is only the medium the
files stage writes to; running without one is an expected outcome.
`hwtest:` lines go to the console and to the serial lane, so
`make smoke-hardware-hwtest SERIAL_PORT=...` can type the command and check
the stages. The stage programs' own detail lines appear on the console.

## 5. Build and tests

The monitor is one `exe_program` line (`user/monitor/`), built only from
the public ABI and the SDK:

- the SDK: startup, the console functions, `getinfo`, `exit` and the
  diagnostic wrappers (`kos/diag.h`), and `klibc` for strings and
  formatting;
- `user/monitor/`: hex parsing and formatting (`hexfmt`), 16-byte dump
  rows (`hexdump`), and the `OK`/`ERR:` replies (`status`).

```sh
make monitor-test     # pack and validate monitor.exe, run tests/test_monitor.py
```

`tests/test_monitor.py` checks the EXE layout (entry `0100h`, image, BSS
and stack geometry, the low-window scratch) and that the link map holds
only monitor and SDK modules: no kernel, driver or early-console symbol.
The subsystem image replays a scripted session (`tests/test_subsys_sim.py`)
and `make smoke-hardware-subsystem` runs it on a board.
