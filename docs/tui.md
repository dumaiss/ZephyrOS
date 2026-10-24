# The KonaOS TUI SDK

`libtui` is the optional text-screen layer of the KonaOS SDK.  It owns a
logical cell map of the grid the console reports, up to the capacity the
program is built for, decodes the console's VT100 key vocabulary, and
renders only when the application flushes.  `libtui_widgets` is a second,
optional archive of procedural controls built on the same public core.
Applications never write terminal escape sequences or CP850 box bytes.

Build a core program with `LIBS=tui` and include `tui.h`.  Build a widget
program with `LIBS=tui_widgets:tui` and include `tui_widgets.h`; the widget
archive comes first because it depends on `libtui`.  See the [SDK
guide](sdk.md) for `exe_program` and the standalone package.

A program's capacity is a build option: `TUI_GRID=128x35` in its
`exe_program` options links `libtui_128x35.lib` instead of the 85 x 26
`libtui.lib` (`TUI_GRID=85x26`, the default).  Choose the largest grid the
program wants to use; a larger capacity costs static storage (below).

```make
$(eval $(call exe_program,my_ui,.,my_ui.c,STACK=512,LIBS=tui))
$(eval $(call exe_program,my_form,.,my_form.c,STACK=512,\
	LIBS=tui_widgets:tui))
```

## Lifecycle

```c
if (tui_init() < 0) { /* report and exit */ }
/* draw, read keys, flush */
tui_shutdown();
```

`tui_init()` makes the terminal and the model agree: normal rendition, a
blank cell map, home cursor, no automatic wrapping and a visible cursor.
It is safe again after `tui_shutdown()`.  A second `tui_init()` while
already initialized returns `-KOS_EINVAL` and leaves the screen untouched.
`tui_shutdown()` is idempotent: it flushes pending cells best effort, restores
normal rendition, re-enables automatic wrapping and shows the cursor.  Every
normal exit path should call it.

Before initialization, and after shutdown until the next successful init:

| call | result |
|---|---|
| `tui_putc`, drawing, window, style, clear, save/restore | `-KOS_EINVAL` |
| `tui_flush`, `tui_end` | `-KOS_EINVAL` |
| `tui_key_pending` | `-KOS_EINVAL` |
| `tui_pollkey`, `tui_getkey` | `TUI_KEY_NONE` (no busy-spin) |

## Geometry and coordinates

`tui_get_size()` reports the screen: the text grid the console reports
(`KCONSOLE_OP_INFO`, asked once by `tui_init`), clamped to the program's
capacity - 85 columns by 26 rows on both of today's consoles.  Query it
instead of hard-coding numbers.  On a console larger than the capacity
the program uses the top-left part of the screen; a smaller grid is used
as reported.

```c
struct tui_size size;
if (tui_get_size(&size) < 0) { /* error */ }
```

Coordinates are 0-based.  Valid cursor positions are `0 .. cols-1` and
`0 .. rows-1`; drawing at the right edge is clipped, not wrapped.  A cursor
may finish just past the right edge after text reaches the edge.  The origin
is the top-left of the **active window**, not necessarily the screen.

`tui_get_window()` reports the active window's rectangle when a layer needs
to own an edge, such as a status bar that fills the bottom row.

## Text and control bytes

```c
tui_putc('A');
tui_puts("text");
tui_write(buffer, length);
tui_putc_xy(x, y, 'A');
tui_puts_xy(x, y, "text");
tui_put_text(x, y, glyphs, length, attr); /* literal row, one attribute */
tui_printf("%u", value);       /* optional formatter member */
tui_printf_xy(x, y, "%s", text);
```

Ordinary text is clipped to the active window.  The `_xy` forms use a local
drawing cursor and leave the logical cursor alone; the other forms advance
the logical cursor.  `tui_put_text()` stores `length` literal glyph bytes
with one attribute, exactly like `length` calls to `tui_putc_xy()` with that
attribute, but every byte is stored as itself: control bytes have no cursor
effect, the cursor does not move, and cells past the window's right edge are
clipped.  Control bytes follow these rules:

| byte | effect |
|---|---|
| BS (08h) | move left, clamped at column 0 |
| TAB (09h) | move to the next eight-column stop, clamped to the window's last column |
| LF (0Ah) | move down one row, clamped at the window's last row; the column is preserved |
| CR (0Dh) | move to column 0 |
| other C0 | ignored |

There is no implicit scrolling.  A printable byte beyond the right edge is
consumed and changes no other row.  Text return counts are input-byte counts,
even when clipping prevents a cell change.

## Windows and clipping

```c
tui_window(x, y, w, h);          /* absolute rectangle, relative drawing */
tui_reset_window();              /* full screen */
tui_push_window(&rect);          /* bounded nested window stack */
tui_pop_window();
```

`tui_window()` and `tui_push_window()` require a nonempty rectangle fully
inside the screen.  Installing a window homes the logical cursor to its
top-left.  The stack depth is four; overflow and underflow return
`-KOS_EINVAL` without changing state.  Pop restores the previous window and
homes the cursor.  Every text, cursor, clear, style, primitive and
save/restore operation clips to the active window.

Clear and line operations affect only the active window:

```c
tui_clear();       /* whole window, cursor home */
tui_clreol();      /* cursor through the window's right edge */
tui_clrline();     /* the cursor's whole window row */
tui_insert_line(); /* shift rows inside the window, blank at the cursor row */
tui_delete_line(); /* shift rows up inside the window, blank the last row */
tui_scroll_rows(top, height, TUI_SCROLL_UP); /* absolute full-width region */
```

Insert/delete shift only rows inside the window.  Blank cells use the
current resolved attribute. `tui_scroll_rows` is the hardware-assisted form:
it flushes pending changes, moves an absolute full-width screen region by one
row, blanks the exposed row, and shifts the logical cell map without dirtying
the retained rows. The current window and cursor are preserved. A console
without semantic scroll support returns `-KOS_ENOTSUP` without changing the
logical map, so an application can repaint its viewport as the fallback.

## Transactions and flushing

Drawing changes the logical cell map and its dirty bitmap only.  Nothing is
sent until a flush:

```c
tui_begin();
/* many drawing calls */
if (tui_end() < 0) { /* error */ }
```

`tui_begin()` has bounded nesting; `tui_end()` flushes only when it closes
the outermost transaction.  An unmatched `tui_end()` is `-KOS_EINVAL`.
`tui_flush()` always flushes, even inside a transaction. On the V9958 route it
coalesces adjacent changes into absolute TEXT records and attribute-only
normal/reverse changes into INVERT records, then submits a bounded render
stream as one console transaction. INVERT is performed in place, so moving
selection focus does not redraw either label. A semantic SCROLL record moves
one full-width row region up or down; the V9958 changes its circular display
origin, restores only rows outside the region, and clears the exposed row.
If the console reports
`-KOS_ENOTSUP`, as the serial console does, the library remembers that result
and uses the ANSI dirty-run renderer: one cursor position per run, SGR only
when the run's attribute differs from the tracked physical attribute.

Successful split render requests commit only the cells carried by that
request; uncommitted cells remain pending after a later failure. The ANSI
fallback retains all pending state on failure. A redraw that changes no cell
and no cursor state emits nothing.

## Styles, attributes and frames

Semantic styles keep application code independent of the terminal's color
model:

```c
TUI_STYLE_NORMAL
TUI_STYLE_TITLE
TUI_STYLE_SELECTED
TUI_STYLE_DISABLED
TUI_STYLE_ERROR
TUI_STYLE_INPUT
TUI_STYLE_STATUS
TUI_STYLE_ACCENT
```

v1 resolves NORMAL, DISABLED, ERROR, INPUT and ACCENT to normal video, and
TITLE, SELECTED and STATUS to reverse video.  `tui_set_style()` affects later
drawing only; it does not recolor existing cells.  `tui_set_attr()` is the
low-level escape hatch and accepts only 0 (normal) or 1 (reverse).

```c
tui_fill(&rect, ' ', TUI_STYLE_NORMAL);
tui_hline(x, y, width, TUI_FRAME_SINGLE);
tui_vline(x, y, height, TUI_FRAME_DOUBLE);
tui_frame(&rect, TUI_FRAME_SINGLE, TUI_STYLE_TITLE);
tui_frame_title(&rect, TUI_FRAME_DOUBLE, TUI_STYLE_TITLE, "Panel");
```

`TUI_FRAME_NONE` emits no border; `tui_frame_title` still renders a title for
a nonempty rectangle.  A zero-area titled frame is a successful no-op.  A
title is clipped inside the top edge with one blank before and after it where
room permits, and never writes outside the rectangle.  All primitives clip to
the active window and handle width or height 1 and 2 without duplicate
corners.

## Keys

`libtui` decodes the console's exact VT100 vocabulary.  Ordinary nonzero
bytes keep their value, including CR, TAB, BS and DEL.  NUL is ignored because
`TUI_KEY_NONE` is zero.

| key | value | sequence |
|---|---|---|
| ESC | `001Bh` | single ESC |
| Up / Down / Right / Left | `0101h` / `0102h` / `0104h` / `0103h` | `ESC [ A/B/C/D` |
| Home / End | `0110h` / `0111h` | `ESC [ H/F` |
| Insert / Delete | `0114h` / `0115h` | `ESC [ 2~/3~` |
| Page Up / Page Down | `0112h` / `0113h` | `ESC [ 5~/6~` |
| F1-F4 | `0121h`-`0124h` | `ESC O P/Q/R/S` |
| F5-F8 | `0125h`-`0128h` | `ESC [ 15~/17~/18~/19~` |
| F9-F12 | `0129h`-`012Ch` | `ESC [ 20~/21~/23~/24~` |

```c
int  pending = tui_key_pending(); /* >0 key ready, 0 empty, <0 error */
tui_key_t key = tui_pollkey();    /* never blocks; NONE when empty */
tui_key_t key = tui_getkey();     /* idles until a key is ready */
```

A lone ESC is returned after one immediate non-blocking look-ahead; it does
not wait for another byte.  The `[` or `O` introducer must therefore already
be available with ESC.  Once CSI or SS3 state has been entered, the remaining
sequence bytes may arrive across later console reads.  Unknown, malformed or
overlong sequences return ESC first, then replay every consumed tail byte as
ordinary input in order, so no input is lost.  `tui_getkey()` calls `idle()`
only between genuinely empty polls.

## Save and restore

Dialogs and overlays can preserve a clipped region with caller-owned storage:

```c
struct tui_rect box = {10, 5, 20, 8};
uint16_t need = tui_saved_size(&box);
struct tui_cell *saved = my_storage; /* need bytes */
if (tui_save(&box, saved, need) < 0) { /* error */ }
/* draw the overlay */
if (tui_restore(&box, saved, need) < 0) { /* error */ }
```

The rectangle is relative to the active window and clipped once.  The size is
the clipped row-major cell count times `sizeof(struct tui_cell)`.  There is no
allocator and no hidden region pool.  Save and restore reject a short or null
buffer before copying; a zero-area clip has size zero and succeeds without
access.  Restore copies character and resolved attribute, marks only changed
cells dirty, and never moves the cursor, changes style/window state or
flushes.

## Widgets

Include `tui_widgets.h` and link `LIBS=tui_widgets:tui`.  All widget
coordinates are relative to the active window.

```c
tui_statusbar("status");                 /* bottom row of the window */
tui_menubar(items, count, &selected);    /* top row; selected is an item ID */
tui_label(x, y, "label");
tui_button(x, y, "OK", TUI_ID_OK, focused);
tui_input(x, y, width, buffer, buffer_size);
tui_checkbox(x, y, "Enabled", &checked);
tui_listbox(&rect, &list);
tui_id_t id = tui_menu_run(items, count);
int committed = tui_input_run(x, y, width, buffer, buffer_size);
tui_id_t answer = tui_message_box(title, message, buttons);
tui_id_t answer = tui_confirm(title, message);
```

Render-only calls leave input policy to the application and redraw only the
cells they own.  `tui_menu_run()` starts on the first item and returns its ID,
or `TUI_ID_NONE` on ESC.  `tui_input_run()` accepts printable bytes, arrows,
HOME/END, BS/DELETE and returns 1 on ENTER, 0 on ESC, or a negative error.

Button masks use `TUI_BUTTON(TUI_ID_OK)`, `TUI_BUTTON(TUI_ID_CANCEL)`,
`TUI_BUTTON(TUI_ID_YES)` and `TUI_BUTTON(TUI_ID_NO)`.  A zero or foreign-bit
mask is invalid.  Buttons are ordered OK, YES, NO, CANCEL; the first present
button has focus; left/right or TAB moves focus; ENTER chooses the focused
ID; ESC chooses CANCEL when present, otherwise `TUI_ID_NONE`.
`tui_confirm()` is a YES/NO message box with YES initially focused.

Message and confirmation boxes center within the active window, wrap at word
boundaries, and are capped at 60 columns by 16 rows.  They are
non-reentrant: a nested modal call is refused with `TUI_ID_NONE`.  The
separately linked modal member owns one static 60 x 16 cell save buffer and
restores its covered region exactly on every exit path.  Render-only widget
programs do not link that member.

## Results and errors

Unless documented otherwise, lifecycle, cursor, window, style, drawing,
clear, save and restore calls return 0 on success or a negated `KOS_E*`
value.  A null required pointer, invalid enum/rectangle, operation before
`tui_init()`, window-stack overflow/underflow or unmatched `tui_end()` is
`-KOS_EINVAL`.

`tui_putc()` returns 1.  `tui_puts()`, `tui_write()` and their `_xy` forms
return the input-byte count.  Formatting calls return the formatted byte
count.

## Memory cost

The core library uses static storage only, sized by the capacity: one
byte per cell for the characters and four bits per cell for the
attributes, dirty and invert bitmaps (one bit per cell each), one shared
render/ANSI run buffer (the capacity's columns plus 16) and a small state
block.

| capacity | characters | attributes | bitmaps | run buffer |
|---|---:|---:|---:|---:|
| 85 x 26 (`libtui.lib`) | 2,210 | 1,105 | 2 x 277 | 101 |
| 128 x 35 (`libtui_128x35.lib`) | 4,480 | 2,240 | 2 x 560 | 144 |

The formatter and every optional function group are separate archive
members, so a program that does not call them does not link their
code.  The widget archive is separate; its modal member is the only hidden
widget backing store (1,920 bytes) and is absent from render-only programs.
Application save/restore buffers and widget lists/values belong to the
caller.  There is no heap allocation.

## Shutdown and restoration

On `tui_shutdown()` the pending logical cells are flushed best effort, then
normal rendition, automatic wrapping and a visible cursor are restored.
Calling it twice is safe.  An application that exits without shutting down
may leave the terminal in the last SGR/wrapping state; always call
`tui_shutdown()` from normal exit paths.

See the standalone examples `tui_demo.c`, `tui_keys.c` and `tui_dialog.c`
for complete programs, and [sdk.md](sdk.md) for building and packaging.
