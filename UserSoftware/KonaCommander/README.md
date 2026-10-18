# KonaCommander for KonaOS

KonaCommander is a native two-pane file manager for KonaOS.  The
left pane starts on the homebrew computer's local `/fs`; the right pane shows
the PC directory exported by the host tool.  It uses the V9958/HID 85x26
console for a Commander-style interface and `/dev/serial/0` for the checked PC
link.  F3 opens either local or PC files in the built-in text/hex viewer; F5
copies a file to the opposite pane.  Neither viewing nor copying requires a
whole file to fit in Z80 RAM.

The implemented milestone includes phases 1-8's bidirectional single-file
copy while leaving rename, delete, and directory mutation out of scope.

## Build and test

```sh
make test
make
```

The Makefile hashes and unpacks the current SDK at
`../../../../ZephyrOS/build/kos-sdk-0.1.0.tar.gz`.  The target is:

```text
build/user/konacmd.exe
```

## Run

Install pyserial on the PC, choose a directory to expose, and start:

```sh
python3 host/kcommander_host.py --port /dev/ttyUSB0 --root "$HOME/Documents" --write
```

The default 4 ms target-bound byte pacing is required by KonaOS's polled SIO
application interface; sending a response as one full-speed 115200-baud burst
can overrun the hardware FIFO while the application re-enters the driver.
Use `--trace` when diagnosing a link.  A successful connection prints `KCP1
RX HELLO` followed by `KCP1 TX HELLO`.  `--pace-ms` is tunable, but lowering it
should be validated on hardware.

Omit `--write` for a read-only PC export.  PC-to-KonaOS copy still works in
that mode; KonaOS-to-PC copy reports that the host must be restarted with
`--write`.  Host uploads are written to a temporary file in the destination
directory and atomically replace the final name only after COMMIT.

Then run `konacmd` on a KonaOS image built with the V9958/HID console.  Do not
use the serial-console build: its console and `/dev/serial/0` share SIO0/B.
KonaCommander starts even when the PC server or cable is absent.  The local
pane remains usable while the PC pane is empty; start the server/connect the
cable and press `r` to connect.  A failed reconnect leaves the UI
running.

Keys: arrows move, PageUp/PageDown change directory pages, Tab changes pane,
Enter descends, Backspace goes to the parent, F3 or `v` views, F4 or `x`
toggles text/hex in the viewer, F5 copies the selected file to the opposite
pane, `r` refreshes or reconnects, F1 or `h` shows help, and F10 or `q` exits.
Escape returns from the viewer.  Copy always asks for confirmation because an
existing destination file will be replaced.

PC-to-KonaOS copy first writes a `KCTMPn.$$$` file in the destination
directory.  The old destination is removed only after the complete temporary
file has been synced; KonaOS then renames the temporary file into place.  If
the final rename fails, the temporary file is deliberately retained for
recovery.

Ordinary Up/Down selection changes redraw only the old and new rows plus the
detail line.  Full-screen redraws are reserved for directory changes, pane
switches, and returning from the viewer.

## Hardware smoke test

1. Start the host service with a root containing subdirectories, a text file,
   a binary file, and a file larger than 64 KiB.
2. Connect the PC adapter to the SIO0/B serial channel at 115200 8N1.
3. Run `konacmd`; confirm the left pane lists KonaOS `/fs`, the right pane
   lists the PC's virtual `/`, and no host absolute path is shown.
4. Navigate each pane independently, including PageDown and Backspace.
5. View the text and binary files, toggle modes, and page beyond offset 65535.
6. Copy a PC file to KonaOS with F5, compare it byte-for-byte, then copy a
   different local file to the PC and compare it.  Repeat over an existing
   filename and verify the confirmation and replacement behavior.
7. Stop the host, press `r`, and confirm a bounded timeout with the UI still
   running and the local pane usable; restart the host, press `r` again, and
   confirm the PC pane recovers.
8. Quit and confirm KonaOS restores its ordinary terminal state.

The automated tests do not substitute for this on-board serial test.
