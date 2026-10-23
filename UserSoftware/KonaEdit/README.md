# KonaEdit for KonaOS

KonaEdit is a single-document CP850 text editor built on the public KonaOS
TUI, filesystem and XMEM APIs.  Its interface follows the compact style of
MS-DOS Edit/QBasic: a menu bar, full-screen editing area, status line and
keyboard-only dialogs.

## Build and test

From this directory:

```sh
make
make test
```

The Makefile unpacks and uses only `../../build/kos-sdk-0.2.0.tar.gz`.  The
result is `build/user/konaedit.exe`.

## Run and keys

Run `konaedit` for a new document or `konaedit /fs/NAME.TXT` to open one.
F1 focuses the menus; Left/Right chooses a menu, Down or Enter opens it,
Up/Down selects an item, Enter activates it and Escape backs out.  The direct
keys are F2 Save, F3 Open, F4 Find, F5 Find Next, F7 start/clear mark mode, F8
Copy, F9 Paste and F10 Exit.  Insert toggles insert/overwrite.  Arrows,
Home/End and PageUp/PageDown navigate.  Backspace and Delete remove text.

TAB inserts a literal TAB in the editor.  In a dialog TAB moves focus between
the list, filename field and buttons; only the focused control is reversed.
Selection is keyboard mark mode because the public input API cannot represent
Shift+Arrow.

## File and resource rules

Documents are CP850 bytes with LF line endings.  Loading accepts LF, CRLF or
bare CR and normalizes them to LF. A CP/M `1Ah` byte terminates input, so record
padding or directory debris after the logical EOF is ignored. TAB remains byte
`09h` and displays at eight-column stops. NUL before logical EOF is rejected;
there is no binary or UTF-8 mode.
The maximum normalized document is exactly 65,536 bytes, including a single
line of that length.  Longer lines scroll horizontally.

The document is always backed by eight 8-KiB XMEM objects.  Clipboard storage
is two 16-KiB mappable objects (32 KiB), and undo storage is two 32-KiB XMEM
objects.  At most three undo records are retained.  Save uses a same-directory
temporary file, syncs it, preserves an existing destination under a recovery
name, and installs the new file by rename; an unrecoverable cleanup path is
reported rather than silently discarded.

## Owner hardware checklist

- [ ] Edit, save and reopen CP850 text containing literal TABs.
- [ ] Open, edit, save and reopen an exact 64-KiB document.
- [ ] Navigate and edit a one-line file wider than the display.
- [ ] Exercise every menu and every dialog focus stop.
- [ ] Hold a normal typing run near or below 0.1 seconds per displayed key.
- [ ] In Open/Save As, type a filename and move between list rows near or
      below 0.1 seconds per key.
- [ ] Browse a directory with more than eight entries across the page boundary
      and confirm container `/` suffixes remain inside the dialog.
- [ ] Type a filename, activate Open, confirm the chosen document replaces the
      editor contents, and confirm the process remains running.
- [ ] Insert and remove newlines near or below 0.1 seconds per displayed key.
- [ ] Exercise three undo and redo operations plus copy/cut/paste.
- [ ] Scroll both axes and judge key-to-screen responsiveness.
- [ ] Confirm F10 restores the ordinary KonaOS shell display and cursor.

These checks remain pending until performed on the owner hardware; simulator
results do not stand in for them.
