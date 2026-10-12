# KonaOS M7 native monitor

`monitor.exe` is the first useful native KonaOS program and the
interactive recovery/diagnostic environment used before the M10 shell.
It is an **ordinary EXE v2 executable**, stored in the ROMFS recovery
store and launched through the unchanged M6 `process_exec()` path; it is
not linked into the kernel and has no special executable format.  When
the shell arrives, the same monitor remains available as a diagnostic
program.

The monitor is deliberately device-agnostic: ordinary input/output goes
through `SYS_CONSOLE_*` (the bound normal console driver) and every
memory/port/system operation crosses the M7 `SYS_DIAG_*` boundary
(`docs/abi.md` section 9).  It contains no SIO/V9958/HID knowledge, no
CP/M dependency, and no private kernel symbols, so the identical binary
keeps working when the console device is rebound in M8.  Command input
has one optional second source: the monitor binds the namespace name
`/dev/serial/0` (a serial object a resident driver publishes) and polls
it beside the console, so the same binary is drivable from the serial
lane on a build whose console is the V9958/HID one - see "Serial command
lane" below.

## Boot and launch policy

```text
reset
  -> kernel boot (banner, device registry)
  -> ROMFS + dynamic SIO module
  -> normal console bound ("console: dynamic sio0.b OK")
  -> optional milestone acceptance hook (acceptance images)
  -> native boot launcher (KOS_BOOT_PROGRAM, production default
     /system/monitor.exe)
       -> process_exec(kernel_boot_program, &status)
       -> monitor runs interactively in its application bank
       -> Q -> SYS_EXIT -> launcher resumes
       -> launcher prints
          "monitor: exit status 0xssss active A free-banks F"
       -> re-exec the boot program
```

A launch failure (for example a missing or rejected boot program) is
reported on the early console as `<configured path>: launch failed 0xNN`
and takes the controlled halt path.  The boot program is the
`KOS_BOOT_PROGRAM` build setting (`config/system.mk`); the default is
`/system/monitor.exe`, and M10 points it at the shell.

Because every cycle is an ordinary exec/exit, the launcher's repeated
exit line is a live proof that no process slot or bank leaked: after a
normal `Q` the line reads `active 0 free-banks 6` (bank 6 left the
application pool in M10; the process allocator hands out banks 0-5,
`PROCESS_BANK_COUNT` in `kernel/include/process.h`).

## Command surface

Lines are limited to 80 characters; the prompt is `Z80> `.  Input is
echoed.  Backspace (`08h`) and DEL (`7Fh`) edit the current line;
characters beyond the limit ring the terminal bell without entering the
buffer; CR or LF ends the line (a CRLF's LF is swallowed); other control
bytes are ignored.  Command letters and hex digits are case-insensitive.
Any missing, malformed, oversized, out-of-range, or trailing argument is
answered with `ERR: ARG`.

| Command | Action |
| --- | --- |
| `H`, `?` | help |
| `R` | system/bank/process information |
| `D addr len` | dump the monitor's own bank, 16 bytes per row |
| `DB bank addr len` | dump application bank 0-6 |
| `M addr value` | modify one byte in the monitor's own bank |
| `MB bank addr value` | modify one byte in application bank 0-6 |
| `I port` | read I/O port `10h-FFh` |
| `O port value` | write I/O port `10h-FFh` |
| `X path [tail]` | run a EXE from `/system` or `/fs`, print its exit status |
| `Q` | exit to the launcher |

`X` runs the named program through the launcher's own `SYS_EXEC` path -
no new privilege and no kernel or driver state - waits for it, prints
`exec: exit status 0xssss` (or `ERR: <code>` when the exec itself fails)
and prompts again.  The optional tail travels to the child byte-exact,
which is how the `stage` option reaches the outcome programs the
exerciser starts.

> **A card executable is runnable again (45b correction 01).**  Work
> package 45b found that `X /fs/<name>.exe` failed with `ERR: 02`
> (`KOS_EINVAL`) before the filesystem provider was called at all:
> `SYS_EXEC` staged the caller's path in bank-7 `_DATA` (`0xC0DD` in
> that build), which lies in the `6000h-DFFFh` region a bank-6
> provider's window replaces, so the `/fs` provider could not read it
> (`/system` worked because the ROMFS reader is in-process).  Correction
> 01 stages the path the way `SYS_BIND` does, in the VFS workspace in
> the fixed core, so the program on the card loads, runs and reports its
> exit status like any store program; everything the exerciser needs is
> still in the store, so no acceptance row depends on card execution.
> The fix, the guard and the replay are in
> `docs/validation/2026-10-11-m10-hardware-acceptance-correction-01.md`.

```text
Z80> X /system/hwtest.exe
... the exerciser's own lines (see "Serial command lane") ...
exec: exit status 0x0000
Z80>
```

Restrictions enforced by the monitor and re-checked by the kernel:
writes require `0100h-DFFFh` (native page zero protected), reads allow
`0000h-DFFFh`, bank arguments must be valid application banks (the
kernel-reported `7Fh` mask, so bank 7 is never selectable), and ports
`00h-0Fh` - the memory-latch alias range - are refused without being
executed.  `R`, `I`, `M`, and `O` report their results as:

```text
KonaOS native monitor
bank=00 pid=0 scratch=0F19
Z80> R
system: abi 01 app 10 os 18 common E000 kernel-bank 07 romfs 02
process: pid 0 bank 0 flags 0001
bank: current 00 mask 7F
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
monitor: exit status 0x0000 active 0 free-banks 6
```

The banner's `scratch=XXXX` is the address (printed as four hex digits;
`0F19` in the hash-pinned build) of a reserved one-byte operator scratch
variable in the monitor's BSS: `M <scratch> vv` and `D <scratch> 1`
exercise the memory write/readback path without touching live monitor
state (the physical smoke parses this address from the banner).

Deliberately deferred, not emulated: Intel HEX load/export (`L`),
`G`, `APP`, and CP/M register-snapshot semantics.  `R` reports native
system/bank/process identity instead, and `X` runs a EXE from the store
or a card.

## Serial command lane

When the store publishes a serial object at `/dev/serial/0`, the monitor
binds that name through the ordinary typed protocol - no port I/O, no
driver symbol, and no fallback: an image without the object is
console-only, exactly as before - and polls it beside the console.  A
command line can then be typed from either source; the console is polled
first, and every byte is echoed back to the source it came from, so

- the serial-console build (both sources are the same stream) renders
  each typed byte exactly once, and
- on the production image - whose console input is HID and whose console
  output is the V9958 - the lane carries a complete command path: a
  terminal or a host tool types the command, the monitor echoes it on the
  same lane, and the output appears on the V9958 display.  The monitor
  writes nothing to the lane on its own.

The line-editing rules are the SDK's `console_readline` semantics: BS/DEL
erase the last character, CR or LF ends the line (a CRLF's LF is
swallowed), an overflow rings the bell instead of entering the byte, and
other control bytes are ignored.

The production store carries the exerciser this lane exists for:
`/system/hwtest.exe` (work package 45b) runs, in order, the extended
memory and far-code suite, the filesystem suite with every write confined
to `/fs/HWTEST` (created and removed by the runner, also after a
failure), and a namespace/handle stage against the real publications -
one line per stage and a verdict:

```text
Z80> X /system/hwtest.exe
hwtest: begin
hwtest: xmem+far PASS
hwtest: files PASS                 # or: card absent / card not writable
hwtest: namespace+handles PASS
hwtest: 3/3 PASS                   # + " (card absent)" when no card is fitted
exec: exit status 0x0000
```

Nothing has to be copied to the card for this: the exerciser and the
outcome programs its stages run are part of the recovery store, so they
arrive with the ROM image.  The card is only the medium the files stage
writes to - and running without one is a supported outcome (the absent
branch above), not a failure.

Every `hwtest:` line goes to the console and to the lane, so
`make smoke-hardware-hwtest SERIAL_PORT=/dev/ttyUSBx` (or
`tools/serial_smoke.py --mode hwtest`) can type the command at the
monitor over the lane and check the stages and the verdict; run it once
with a card and once without.  The stage programs' own outcome lines
(`xmem-selftest: 61 checks, 0 failures`, `xmem-far: 19 checks,
0 failures`, `fs-selftest: 37 checks, 0 failures` - or 8 without a card -
and the nested execs' `xmem-child`/`fs-child` rows) appear on the
console, i.e. on the V9958 display.

## Program support (the user SDK)

Since package 28c the monitor is an ordinary SDK program: it is built by
one `exe_program` line (`user/sdk/exe.mk`) from the public ABI in
`abi/kos/` and the shared user-space library, so it writes no entry
assembly and no hand-written manifest.  Later diagnostic programs reuse
the same layers (`docs/sdk.md` is canonical):

- libkos (`user/sdk/`) - crt0 (startup block, `argv`, `SYS_EXIT`), the
  console wrappers (`console_write`, `console_read`, `console_poll`, `getinfo`,
  `exit`) and the M7 diagnostic wrappers (`diag_mem_read`,
  `_write`, `_io_in`, `_io_out`, `_bank_info`, `_system_info`) in
  `user/sdk/include/kos/{console,diag}.h` with their gate stubs in
  `user/sdk/src/{console,diaggate}.asm`; one shared low-window scratch;
- klibc (`user/sdk/src/klibc/`) - `strlen`/`strcmp`/`mem*`/`strl*` and the
  integer/`printf` formatters, one archive member each;
- `user/monitor/` - the monitor's own helpers: `hexfmt.c` / `.h` (hex
  byte/word parsing and formatting), `hexdump.c` / `.h` (16-byte row
  formatting with address, hex and ASCII columns) and `status.c` / `.h`
  (`OK`, `ERR: ARG`, `ERR: COMMAND` and error-code output).

These components link into the EXE (`user/monitor/`), never into the
kernel.

## Build, test, and acceptance

```sh
make monitor-test          # pack/validate monitor.exe + tests/test_monitor.py
make m7-selftest           # build the M7 image + tests/test_m7_sim.py replay
make smoke-hardware-m7 SERIAL_PORT=/dev/ttyUSB0
                           # start it first, then power-cycle/RESET the
                           # board so the boot log is captured
```

`make monitor-test` proves the EXE v2 layout (entry `0100h`, exact
image/BSS/stack geometry, low-window scratch) and that the linked map
contains only the monitor/runtime modules - no kernel, SIO, early
console, or CP/M symbols.  `make m7-selftest` builds an image whose
kernel-side selftest checks the diagnostic boundary (exact results,
every rejection class, canary/latch/table stability) and whose
simulator replay scripts console input through the emulated serial link:
help, `R`, dumps, scratch modify/readback, port input, rejected
latch-port output, invalid bank 7, bad syntax, line overflow with bell
and recovery, backspace editing, CRLF handling, two `Q`/relaunch
cycles, and the two identical launcher count lines.

The physical smoke (`--mode m7`) scripts the same session on hardware
over SIO0/B (115200 8N1) and checks the markers verbatim.  Start the
tool first, then power-cycle the board (or press RESET) while it
waits: the boot log (M0 banner, dynamic console line, `m7 selftest:
OK`) is printed once at boot, so a board that is already running
cannot provide it - a warm-board run refuses to start the session
(sends no commands) and fails with an explicit reset hint.  Each
command is then sent only after the monitor's trailing prompt (so no
input arrives while output is being printed) and the command bytes are
paced at typed-input speed: the polled console receive register has no
burst buffering, so a fast burst sent mid-output overruns and drops
bytes.  A human interactive pass (help, dump, modify/readback, I/O
read, rejected requests, two `Q`/relaunch cycles) is part of M7
acceptance; the dated record is `docs/validation/`.
