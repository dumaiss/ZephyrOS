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

## Starting

```
konaedit                 a new, unnamed document
konaedit /fs/NAME.TXT    open a file
```

The screen has three parts:
- the **menu bar** on the top row (File, Edit, Search, Help);
- the **editing area**;
- the **status line** on the bottom row.

The status line shows, in order:
- the file name (`Untitled` for a new document);
- `*` when there are unsaved changes;
- `Ln` and `Col`: the cursor's line and on-screen column, counting a TAB
  as the columns it takes;
- `INS` or `OVR`;
- `MARK` while a selection is being marked.

## Keys

### Moving

| Key | Action |
|---|---|
| Left / Right | One character; a TAB is one character |
| Up / Down | One line, keeping the column where the line allows |
| Home / End | Start / end of the line |
| PageUp / PageDown | One screen of lines; the cursor keeps its place on the screen |

Lines wider than the screen scroll sideways as the cursor moves along them.

### Editing

| Key | Action |
|---|---|
| any character | Insert it at the cursor; in overwrite mode, replace the character under the cursor (a line end is never overwritten: the character is inserted before it) |
| Enter | Split the line at the cursor |
| TAB | Insert a TAB character; it shows as spaces to the next multiple of eight |
| Backspace | Delete the character before the cursor; at a line start, join the line to the one above |
| Delete | Delete the character under the cursor; at a line end, join the next line to this one |
| Insert | Switch between insert (`INS`) and overwrite (`OVR`) |

### Selecting and the clipboard

The console's input API doesn't report Shift+arrow, so selections use a mark
mode:

| Key | Action |
|---|---|
| F7 | Start marking at the cursor (`MARK`); move the cursor to extend the selection; F7 again ends mark mode |
| Escape | Clear the selection |
| F8 | Copy the selection |
| F9 | Paste at the cursor; a selection is replaced |
| Delete | Delete the selection |
| any character | Replace the selection |

Cut, Delete and Select All are in the Edit menu. The clipboard holds up to 32
KiB.

### Direct keys

| Key | Action |
|---|---|
| F1 | Open the menu bar |
| F2 | Save |
| F3 | Open |
| F4 | Find |
| F5 | Find Next |
| F7 | Mark mode |
| F8 / F9 | Copy / Paste |
| F10 | Exit |

## Menus

- **Opening:** F1 opens the menu bar. Left/Right chooses a menu, and
  Down or Enter opens it.
- **Choosing:** Up/Down moves between items, and Enter runs the item.
  Left/Right moves to the next menu while one is open.
- **Closing:** Escape closes the open menu, and a second Escape leaves the
  menu bar.

| Menu | Items |
|---|---|
| File | New, Open (F3), Save (F2), Save As, Exit (F10) |
| Edit | Undo, Redo, Cut, Copy (F8), Paste (F9), Delete, Select All |
| Search | Find (F4), Find Next (F5), Replace, Go To Line |
| Help | Keys, About |

The menu bar is shown in reverse, with the chosen menu's name in normal
video. Items that can't apply right now are disabled: their gaps show as
middle dots (`Copy········F8`), and Enter on them does nothing. They are:
- Undo or Redo with nothing to undo or redo;
- Cut, Copy and Delete without a selection;
- Paste with an empty clipboard;
- Find Next and Replace before a first Find.

## Working with files

- **Open:** File > Open or F3 shows a file dialog.
  - **The list:** it starts in `/fs` and shows eight entries at a time.
    Directories end in `/`.
  - **Moving in the list:** Up/Down moves the selection.
    PageUp/PageDown, or moving past the first or last row, changes page.
  - **Directories:** Enter on a directory enters it. The first row of
    every directory except the root is `..`: Enter on it, or Backspace
    anywhere in the list, goes up one level.
  - **Choosing:** Enter on a file opens it.
  - **Focus:** TAB moves focus from the list to the Name field, the
    Open/Save button and Cancel, and only the focused control is shown in
    reverse. To open by name, type it in the Name field and press Enter
    twice (once to reach the button, once to activate it). Escape cancels.
- **Save:** File > Save or F2 saves under the current name. A new document
  asks for a name first.
- **Save As:** saves under a new name. Saving over an existing file asks
  for confirmation.
- **Safety:** see *File and resource rules* below. The old copy is kept
  until the new one is safely in place.
- **Unsaved changes:** New, Open and Exit ask first: YES saves, NO
  discards, CANCEL returns to the editor.

## Searching

- **Find** (F4): asks for the text, then whether case must match. The
  match is selected and the cursor moves past it. When the end of the
  document is reached, Find offers to wrap around to the start.
- **Find Next** (F5): repeats the last search from the current match.
- **Replace:** finds the next match of the last search, then asks for the
  replacement text and replaces that one match. Repeat it for further
  matches.
- **Go To Line:** asks for a line number and moves to that line, or to the
  last line if the document is shorter.

## Undo

Undo and Redo are in the Edit menu, and the last three edits can be undone:
- a run of typing counts as one edit, and so does a run of Backspace or
  Delete presses;
- a cut, a paste or one replacement is one edit each;
- making a new edit after an undo discards what could have been redone.

## Exiting

F10 or File > Exit returns to the KonaOS shell, leaving a clear screen with
the cursor at the top left. With unsaved changes, it asks first.

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

## Known issues

- **Dialog text can run past the box.** A message box is 36 columns wide,
  and its message is drawn with `ke_label` without wrapping or clipping. A
  longer message, such as "YES saves, NO discards, CANCEL returns to the
  editor." in the Modified document box, writes over the right border and
  beyond. The fix is to wrap the message to the box width, or size the box
  to the message.
- **Enter makes the screen jitter briefly.** A newline or a join moves part
  of the screen by a hardware scroll (`v9958_render_scroll` in the V9958
  driver). The driver first copies the rows that must stay put (the menu
  bar, the status row, and the rows on the side that does not move) into
  their new places in the circular bitmap, clears the exposed row, and only
  then moves the display origin (R#23).
  - **The cause:** while those VDP copies run, the old origin is still on
    screen, so the rows being rewritten are visible half-moved. None of it
    is synchronised to the vertical blank.
  - **The fix:** in the driver, wait for the vertical blank before the
    origin write, or arrange the copies so they only touch rows the old
    origin doesn't show. Either costs some speed.

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
