# ZephyrOS M7 native monitor

`monitor.zex` is the first useful native ZephyrOS program and the
interactive recovery/diagnostic environment used before the M10 shell.
It is an **ordinary ZEX v1 executable**, stored in the ROMFS recovery
store and launched through the unchanged M6 `process_exec()` path; it is
not linked into the kernel and has no special executable format.  When
the shell arrives, the same monitor remains available as a diagnostic
program.

The monitor is deliberately device-agnostic: ordinary input/output goes
through `SYS_CONSOLE_*` (the bound normal console service) and every
memory/port/system operation crosses the M7 `SYS_DIAG_*` boundary
(`docs/abi.md` section 9).  It contains no SIO/V9958/HID knowledge, no
CP/M dependency, and no private kernel symbols, so the identical binary
keeps working when the console device is rebound in M8.

## Boot and launch policy

```text
reset
  -> kernel boot (banner, device registry)
  -> ROMFS + dynamic SIO module
  -> normal console bound ("console: dynamic sio0.b OK")
  -> optional milestone acceptance hook (acceptance images)
  -> native boot launcher (ZOS_BOOT_PROGRAM, production default
     /SYSTEM/monitor.zex)
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
`ZOS_BOOT_PROGRAM` build setting (`config/system.mk`); the default is
`/SYSTEM/monitor.zex`, and M10 points it at the shell.

Because every cycle is an ordinary exec/exit, the launcher's repeated
exit line is a live proof that no process slot or bank leaked: after a
normal `Q` the line reads `active 0 free-banks 7`.

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
| `Q` | exit to the launcher |

Restrictions enforced by the monitor and re-checked by the kernel:
writes require `0100h-DFFFh` (native page zero protected), reads allow
`0000h-DFFFh`, bank arguments must be valid application banks (the
kernel-reported `7Fh` mask, so bank 7 is never selectable), and ports
`00h-0Fh` - the memory-latch alias range - are refused without being
executed.  `R`, `I`, `M`, and `O` report their results as:

```text
ZephyrOS native monitor
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
monitor: exit status 0x0000 active 0 free-banks 7
```

The banner's `scratch=XXXX` is the address (printed as four hex digits;
`0F19` in the hash-pinned build) of a reserved one-byte operator scratch
variable in the monitor's BSS: `M <scratch> vv` and `D <scratch> 1`
exercise the memory write/readback path without touching live monitor
state (the physical smoke parses this address from the banner).

Deliberately deferred, not emulated: Intel HEX load/export (`L`/`X`),
`G`, `APP`, and CP/M register-snapshot semantics.  `R` reports native
system/bank/process identity instead.

## Program support (the user SDK)

Since package 28c the monitor is an ordinary SDK program: it is built by
one `zex_program` line (`user/sdk/zex.mk`) from the public ABI in
`abi/zos/` and the shared user-space library, so it writes no entry
assembly and no hand-written manifest.  Later diagnostic programs reuse
the same layers (`docs/sdk.md` is canonical):

- libzos (`user/sdk/`) - crt0 (startup block, `argv`, `SYS_EXIT`), the
  console wrappers (`zex_write`, `zex_read`, `zex_poll`, `zex_getinfo`,
  `zex_exit`) and the M7 diagnostic wrappers (`zex_diag_mem_read`,
  `_write`, `_io_in`, `_io_out`, `_bank_info`, `_system_info`) in
  `user/sdk/include/zos/{console,diag}.h` with their gate stubs in
  `user/sdk/src/{console,diaggate}.asm`; one shared low-window scratch;
- zlibc (`user/sdk/src/zlibc/`) - `strlen`/`strcmp`/`mem*`/`strl*` and the
  integer/`printf` formatters, one archive member each;
- `user/monitor/` - the monitor's own helpers: `hexfmt.c` / `.h` (hex
  byte/word parsing and formatting), `hexdump.c` / `.h` (16-byte row
  formatting with address, hex and ASCII columns) and `status.c` / `.h`
  (`OK`, `ERR: ARG`, `ERR: COMMAND` and error-code output).

These components link into the ZEX (`user/monitor/`), never into the
kernel.

## Build, test, and acceptance

```sh
make monitor-test          # pack/validate monitor.zex + tests/test_monitor.py
make m7-selftest           # build the M7 image + tests/test_m7_sim.py replay
make smoke-hardware-m7 SERIAL_PORT=/dev/ttyUSB0
                           # start it first, then power-cycle/RESET the
                           # board so the boot log is captured
```

`make monitor-test` proves the ZEX v1 layout (entry `0100h`, exact
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
