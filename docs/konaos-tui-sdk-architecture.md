# KonaOS TUI SDK Architecture

## 1. Purpose

This document defines a small Turbo-Pascal-style TUI layer for KonaOS.

The goal is to make ordinary text applications easy to write without requiring
each application to:

- emit ANSI/VT100 cursor sequences;
- decode VT100 keyboard sequences;
- manage clipping and text windows;
- know CP850 box-drawing byte values;
- redraw whole screens unnecessarily;
- build its own menus, status bars, dialogs, and basic controls.

The design intentionally stays small. It is closer to **Turbo Pascal `Crt` plus
a small subset of Turbo Vision's conveniences** than to curses or a full UI
framework.

The initial implementation is entirely user-space. It requires **no new IOC,
HID, kernel, or V9958 driver ABI**.

---

## 2. Design goals

The initial TUI SDK should:

1. run over the existing KonaOS `/dev/console`;
2. accept the existing IOC/HID **VT100 byte sequences** unchanged;
3. decode special keys once inside `libtui`, not in every application;
4. provide easy cursor/window/screen operations;
5. batch output so one logical UI update does not become hundreds of small
   console calls;
6. support the existing V9958/HID console and the serial console through the
   same application API;
7. use the current 85x26 V9958 text environment efficiently;
8. provide enough optional controls to build monitors, diagnostics, editors,
   compiler front-ends, file managers, and setup utilities;
9. remain a user-space SDK/library, not an OS subsystem.

---

## 3. Non-goals

The first version does **not** need:

- raw HID key events from the IOC;
- mouse or pointer support;
- a graphical UI;
- multitasking or a window server;
- a Turbo Vision-compatible object hierarchy;
- full curses compatibility;
- arbitrary terminal capability discovery;
- a new kernel input protocol;
- a new video mode.

The IOC continues to translate keyboard input into VT100-compatible byte
sequences before KonaOS sees it.

---

## 4. Architecture

```text
                    application
                        |
                    libtui
              +---------+---------+
              |                   |
         screen API           key API
              |                   |
     logical cell/window      VT100 decoder
          management              |
              |                   |
        dirty/run batching        |
              +---------+---------+
                        |
                   /dev/console
                        |
              +---------+---------+
              |                   |
        V9958/HID console     serial console
              |                   |
            V9958              VT100 terminal
```

The application deals only with TUI concepts.

It never needs to know whether the underlying console is:

- the Zephyr-80 V9958/HID console;
- a serial terminal;
- a future compatible console backend.

---

## 5. Input architecture

### 5.1 IOC/HID remains unchanged

The IOC continues to emit ordinary characters and VT100 key sequences.

Examples:

```text
'a'             -> 61h

Up              -> ESC [ A
Down            -> ESC [ B
Right           -> ESC [ C
Left            -> ESC [ D

Delete          -> ESC [ 3 ~
Page Up         -> ESC [ 5 ~
Page Down       -> ESC [ 6 ~
```

The exact sequence set supported by the current IOC firmware becomes the
initial KonaOS terminal-key vocabulary.

Applications must **not** decode these byte sequences themselves.

### 5.2 `libtui` decodes keys

`libtui` consumes console bytes and turns them into one `tui_key_t`.

Ordinary text remains its byte value:

```text
'a'     -> 0061h
'A'     -> 0041h
ENTER   -> 000Dh
TAB     -> 0009h
```

Extended keys occupy a separate range:

```text
TUI_KEY_UP
TUI_KEY_DOWN
TUI_KEY_LEFT
TUI_KEY_RIGHT
TUI_KEY_HOME
TUI_KEY_END
TUI_KEY_INSERT
TUI_KEY_DELETE
TUI_KEY_PGUP
TUI_KEY_PGDN
TUI_KEY_F1 ... TUI_KEY_F12
```

This gives applications a simple switch-friendly input model.

### 5.3 Escape ambiguity

A standalone `ESC` must remain distinguishable from the beginning of a VT100
sequence.

The decoder should:

1. receive `ESC`;
2. briefly check whether additional bytes of a known sequence are already
   pending;
3. continue parsing if they are;
4. otherwise return `TUI_KEY_ESC`.

Any timeout/polling policy belongs entirely inside `libtui`.

No IOC protocol change is required.

---

## 6. Screen model

### 6.1 Coordinates

TUI coordinates are **0-based**:

```text
top-left = (0, 0)
```

The V9958 console currently exposes:

```text
85 columns x 26 rows
```

Applications should query the logical screen size rather than hard-code it.

### 6.2 Windows

A TUI window is a clipping and coordinate region.

```c
struct tui_rect {
    uint8_t x;
    uint8_t y;
    uint8_t w;
    uint8_t h;
};
```

When a window is active:

- `(0,0)` means the window's top-left corner;
- cursor movement is relative to the window;
- clear operations affect only the window;
- writes are clipped at the window boundary.

This mirrors the useful behavior of Turbo Pascal's `Window()`.

### 6.3 Window stack

Temporary dialogs and overlays benefit from nested windows.

`libtui` therefore supports:

```text
push window
    draw relative to it
pop window
```

The stack should remain small and bounded.

---

## 7. Output model

Applications should be encouraged to treat screen updates as transactions:

```c
tui_begin();

/* many drawing calls */

tui_end();
```

or equivalently:

```c
/* many drawing calls */
tui_flush();
```

The library may buffer logical cell changes and combine adjacent changed cells
into larger console writes.

This is important because KonaOS IPC is intentionally coarse-grained.

The TUI should avoid turning:

```text
draw 80 characters
```

into:

```text
80 individual console calls
```

whenever a run can be emitted as one write.

---

## 8. Character and attribute model

### 8.1 Cells

A logical text cell is:

```c
struct tui_cell {
    uint8_t ch;
    uint8_t attr;
};
```

The initial implementation may use a backing cell map for:

- dirty detection;
- clipped drawing;
- save/restore overlays;
- batched output.

For the current V9958 console:

```text
85 x 26 x 2 bytes = 4420 bytes
```

so a complete logical screen image is practical.

### 8.2 Semantic styles

Applications should prefer semantic styles:

```text
NORMAL
TITLE
SELECTED
DISABLED
ERROR
INPUT
STATUS
ACCENT
```

The backend maps these styles to what the console can represent.

The current V9958 console may initially map several styles onto the same
normal/reverse representation. Richer colors can be added later without
changing application code.

### 8.3 Box-drawing glyphs

Applications should not embed CP850 box-drawing byte values.

`libtui` defines symbolic glyphs or frame styles:

```text
SINGLE
DOUBLE
NONE
```

The V9958 console backend may map these to the existing CP850 font.

A serial-console backend may use ASCII fallbacks if necessary.

---

## 9. Core public header: `tui.h`

The following signatures define the proposed v1 SDK surface.

```c
#ifndef KONA_TUI_H
#define KONA_TUI_H

#include <stdint.h>

typedef uint16_t tui_key_t;
typedef uint8_t  tui_style_t;
typedef uint8_t  tui_frame_style_t;

struct tui_size {
    uint8_t cols;
    uint8_t rows;
};

struct tui_rect {
    uint8_t x;
    uint8_t y;
    uint8_t w;
    uint8_t h;
};

struct tui_cell {
    uint8_t ch;
    uint8_t attr;
};

/* ---- lifecycle ------------------------------------------------------- */

int  tui_init(void);
void tui_shutdown(void);

/* Flush all pending output to /dev/console. */
int  tui_flush(void);

/* Optional transaction spelling. begin() may be a no-op in v1. */
void tui_begin(void);
int  tui_end(void);

/* ---- screen information --------------------------------------------- */

int tui_get_size(struct tui_size *out);

/* ---- cursor --------------------------------------------------------- */

int     tui_gotoxy(uint8_t x, uint8_t y);
uint8_t tui_wherex(void);
uint8_t tui_wherey(void);

int tui_cursor_show(void);
int tui_cursor_hide(void);

/* ---- text output ---------------------------------------------------- */

int tui_putc(char ch);
int tui_puts(const char *text);
int tui_write(const char *text, uint16_t length);

int tui_putc_xy(uint8_t x, uint8_t y, char ch);
int tui_puts_xy(uint8_t x, uint8_t y, const char *text);

/*
 * Optional convenience layer.
 * This may be compiled separately if formatted I/O size becomes undesirable.
 */
int tui_printf(const char *format, ...);
int tui_printf_xy(uint8_t x, uint8_t y, const char *format, ...);

/* ---- clear / line operations --------------------------------------- */

int tui_clear(void);
int tui_clreol(void);
int tui_clrline(void);

int tui_insert_line(void);
int tui_delete_line(void);

/* ---- window / clipping --------------------------------------------- */

/*
 * Replace the current logical window.
 * Coordinates passed to ordinary cursor/drawing calls become relative
 * to this rectangle.
 */
int tui_window(uint8_t x, uint8_t y, uint8_t w, uint8_t h);

/* Restore the full screen as the current window. */
int tui_reset_window(void);

/* Bounded nested-window helpers. */
int tui_push_window(const struct tui_rect *rect);
int tui_pop_window(void);

/* ---- styles --------------------------------------------------------- */

enum {
    TUI_STYLE_NORMAL = 0,
    TUI_STYLE_TITLE,
    TUI_STYLE_SELECTED,
    TUI_STYLE_DISABLED,
    TUI_STYLE_ERROR,
    TUI_STYLE_INPUT,
    TUI_STYLE_STATUS,
    TUI_STYLE_ACCENT,
    TUI_STYLE_COUNT
};

int tui_set_style(tui_style_t style);
tui_style_t tui_get_style(void);

/*
 * Optional low-level attribute escape hatch.
 * The interpretation is backend-defined and should not be the normal
 * application interface.
 */
int tui_set_attr(uint8_t attr);
uint8_t tui_get_attr(void);

/* ---- primitive drawing --------------------------------------------- */

enum {
    TUI_FRAME_NONE = 0,
    TUI_FRAME_SINGLE,
    TUI_FRAME_DOUBLE
};

int tui_fill(const struct tui_rect *rect, char ch, tui_style_t style);

int tui_hline(uint8_t x, uint8_t y, uint8_t width,
              tui_frame_style_t style);

int tui_vline(uint8_t x, uint8_t y, uint8_t height,
              tui_frame_style_t style);

int tui_frame(const struct tui_rect *rect,
              tui_frame_style_t style,
              tui_style_t text_style);

int tui_frame_title(const struct tui_rect *rect,
                    tui_frame_style_t style,
                    tui_style_t text_style,
                    const char *title);

/* ---- keyboard ------------------------------------------------------- */

enum {
    TUI_KEY_NONE      = 0x0000,

    TUI_KEY_ESC       = 0x001B,

    TUI_KEY_UP        = 0x0101,
    TUI_KEY_DOWN      = 0x0102,
    TUI_KEY_LEFT      = 0x0103,
    TUI_KEY_RIGHT     = 0x0104,

    TUI_KEY_HOME      = 0x0110,
    TUI_KEY_END       = 0x0111,
    TUI_KEY_PGUP      = 0x0112,
    TUI_KEY_PGDN      = 0x0113,
    TUI_KEY_INSERT    = 0x0114,
    TUI_KEY_DELETE    = 0x0115,

    TUI_KEY_F1        = 0x0121,
    TUI_KEY_F2        = 0x0122,
    TUI_KEY_F3        = 0x0123,
    TUI_KEY_F4        = 0x0124,
    TUI_KEY_F5        = 0x0125,
    TUI_KEY_F6        = 0x0126,
    TUI_KEY_F7        = 0x0127,
    TUI_KEY_F8        = 0x0128,
    TUI_KEY_F9        = 0x0129,
    TUI_KEY_F10       = 0x012A,
    TUI_KEY_F11       = 0x012B,
    TUI_KEY_F12       = 0x012C
};

/* Nonzero when enough input is pending to return a key without blocking. */
int tui_key_pending(void);

/* Blocking read of one decoded key. */
tui_key_t tui_getkey(void);

/* Non-blocking read. Returns TUI_KEY_NONE if no complete key is available. */
tui_key_t tui_pollkey(void);

/* ---- save / restore ------------------------------------------------- */

/*
 * Save/restore a rectangular cell region.
 *
 * Required bytes are:
 *     rect->w * rect->h * sizeof(struct tui_cell)
 */
uint16_t tui_saved_size(const struct tui_rect *rect);

int tui_save(const struct tui_rect *rect,
             struct tui_cell *buffer,
             uint16_t buffer_size);

int tui_restore(const struct tui_rect *rect,
                const struct tui_cell *buffer,
                uint16_t buffer_size);

#endif
```

---

## 10. Optional widget header: `tui_widgets.h`

The widget layer is intentionally separate from the low-level TUI.

Applications that only want `Crt`-style screen handling do not need to link it.

A first useful subset could be:

```c
#ifndef KONA_TUI_WIDGETS_H
#define KONA_TUI_WIDGETS_H

#include "tui.h"

typedef uint16_t tui_id_t;

#define TUI_ID_NONE    0u
#define TUI_ID_OK      1u
#define TUI_ID_CANCEL  2u
#define TUI_ID_YES     3u
#define TUI_ID_NO      4u

struct tui_menu_item {
    const char *label;
    tui_key_t shortcut;
    tui_id_t id;
};

struct tui_list {
    const char * const *items;
    uint16_t count;
    uint16_t selected;
    uint16_t top;
};

/* ---- status / menu -------------------------------------------------- */

int tui_statusbar(const char *text);

int tui_menubar(const struct tui_menu_item *items,
                uint8_t count,
                tui_id_t *selected);

/* ---- simple dialogs ------------------------------------------------ */

tui_id_t tui_message_box(const char *title,
                         const char *message,
                         uint16_t buttons);

tui_id_t tui_confirm(const char *title,
                     const char *message);

/* ---- controls ------------------------------------------------------- */

int tui_button(uint8_t x, uint8_t y,
               const char *label,
               tui_id_t id,
               uint8_t focused);

int tui_label(uint8_t x, uint8_t y,
              const char *text);

int tui_input(uint8_t x, uint8_t y,
              uint8_t width,
              char *buffer,
              uint16_t buffer_size);

int tui_checkbox(uint8_t x, uint8_t y,
                 const char *label,
                 uint8_t *checked);

int tui_listbox(const struct tui_rect *rect,
                struct tui_list *list);

/* ---- common interaction helpers ------------------------------------ */

tui_id_t tui_menu_run(const struct tui_menu_item *items,
                      uint8_t count);

int tui_input_run(uint8_t x, uint8_t y,
                  uint8_t width,
                  char *buffer,
                  uint16_t buffer_size);

#endif
```

The widget layer should remain procedural and lightweight.

Do **not** introduce a large object model or inheritance framework.

---

## 11. Example application

```c
#include <kos.h>
#include <tui.h>

static void draw_screen(void)
{
    struct tui_rect body = { 0, 1, 85, 24 };

    tui_set_style(TUI_STYLE_TITLE);
    tui_gotoxy(0, 0);
    tui_puts(" Kona Utility ");

    tui_set_style(TUI_STYLE_NORMAL);
    tui_frame(&body, TUI_FRAME_SINGLE, TUI_STYLE_NORMAL);

    tui_puts_xy(2, 2, "F1 Help");
    tui_puts_xy(2, 3, "F10 Quit");

    tui_set_style(TUI_STYLE_STATUS);
    tui_puts_xy(0, 25, "Ready");

    tui_flush();
}

int main(void)
{
    tui_key_t key;

    if (tui_init() < 0)
        return 1;

    tui_clear();
    draw_screen();

    for (;;) {
        key = tui_getkey();

        switch (key) {
        case TUI_KEY_F1:
            /* show help */
            break;

        case TUI_KEY_F10:
        case TUI_KEY_ESC:
            tui_shutdown();
            return 0;

        default:
            break;
        }

        tui_flush();
    }
}
```

---

## 12. Recommended implementation model

### 12.1 `libtui` state

A simple internal state object is sufficient:

```c
struct tui_state {
    uint8_t initialized;

    struct tui_size screen;
    struct tui_rect window;

    uint8_t cursor_x;
    uint8_t cursor_y;
    uint8_t cursor_visible;

    tui_style_t style;
    uint8_t attr;

    struct tui_rect window_stack[TUI_WINDOW_STACK_MAX];
    uint8_t window_depth;

    /* VT100 input parser state */
    uint8_t key_state;
    uint8_t key_buffer[TUI_KEY_SEQUENCE_MAX];
    uint8_t key_length;

    /* optional logical cell map */
    struct tui_cell *cells;
};
```

This is library-private, not ABI.

### 12.2 Output backend

The first backend targets `/dev/console`.

It should generate ANSI/VT100-style output accepted by both current console
implementations.

Useful batching policy:

```text
logical cell changes
        |
        +-> find changed horizontal runs
        |
        +-> emit cursor-position sequence
        |
        +-> emit attribute change only when needed
        |
        `-> emit run in one console write
```

Do not emit a cursor-position sequence per character.

### 12.3 Input backend

The input path reads bytes from the console and maintains a small state machine:

```text
NORMAL
ESC
CSI
SS3
```

It only needs to recognize the sequences actually emitted by the current
KonaOS console sources.

Unknown or malformed sequences should fail safely and resynchronize.

---

## 13. Driver and kernel impact

### IOC firmware

**No change required.**

The IOC continues sending VT100 sequences.

### HID driver

**No change required.**

It continues delivering translated text bytes.

### Console driver

Ideally **no ABI change required** for TUI v1.

The TUI uses the existing console byte stream.

### V9958 driver

No required architectural change for TUI v1.

Possible later improvement:

- richer SGR foreground/background colors;
- additional text attributes.

Those improvements should remain transparent to applications using semantic
TUI styles.

### Kernel

**No change required.**

`libtui` is an SDK/user-space component.

---

## 14. Relationship to future graphical UI

The TUI API should not become the future graphical API, but the two libraries
can deliberately share concepts:

```text
TUI                    GUI
---                    ---

rect                   rect
style                  style/theme
button                 button
list                   list
menu                   menu
dialog                 dialog
focus                  focus
```

Possible future stack:

```text
                 application logic
                 /              \
             libtui             libui
               |                  |
          /dev/console           libgfx
                                  |
                              /dev/video
```

This lets KonaOS develop a coherent native application vocabulary without
coupling text and graphical rendering implementations.

---

# 15. Suggested work packages

The following breakdown is intended to be directly usable as implementation
work packages.

## WP-TUI-1: Core library and console lifecycle

Implement:

```text
tui_init
tui_shutdown
tui_get_size
tui_flush
tui_begin
tui_end
```

Requirements:

- build `libtui`;
- use existing KonaOS console handles/API;
- establish an internal state object;
- query or configure the initial 85x26 screen geometry;
- restore sane cursor/style state on shutdown.

Acceptance:

- a test program can initialize the library, write text, flush, and return to
  the shell without leaving the console in a broken state.

---

## WP-TUI-2: Cursor, text, clear, and windows

Implement:

```text
tui_gotoxy
tui_wherex
tui_wherey

tui_putc
tui_puts
tui_write
tui_putc_xy
tui_puts_xy

tui_clear
tui_clreol
tui_clrline
tui_insert_line
tui_delete_line

tui_window
tui_reset_window
tui_push_window
tui_pop_window
```

Requirements:

- 0-based application coordinates;
- relative coordinates inside windows;
- hard clipping at window boundaries;
- bounded window stack;
- no application-visible ANSI strings.

Acceptance:

- a demo can divide the screen into independent regions and write/clear each
  without damaging the others.

---

## WP-TUI-3: VT100 keyboard decoder

Implement:

```text
tui_key_pending
tui_getkey
tui_pollkey
```

Decode at minimum:

```text
ordinary characters
ESC
ENTER
TAB
BACKSPACE

arrows
HOME / END
INSERT / DELETE
PAGE UP / PAGE DOWN
F1-F12
```

Requirements:

- no IOC firmware changes;
- no HID ABI changes;
- consume the existing VT100 byte stream;
- distinguish standalone ESC from sequence prefix;
- recover from malformed/unknown sequences.

Acceptance:

- one test application prints symbolic key names for every supported keyboard
  key;
- the same program works over the V9958/HID console and serial console where
  equivalent VT100 sequences are available.

---

## WP-TUI-4: Styles and box primitives

Implement:

```text
tui_set_style
tui_get_style
tui_set_attr
tui_get_attr

tui_fill
tui_hline
tui_vline
tui_frame
tui_frame_title
```

Requirements:

- semantic style table;
- CP850 box drawing hidden inside the library;
- ASCII fallback permitted for backends that cannot display box glyphs;
- current V9958 normal/reverse capabilities are sufficient for v1.

Acceptance:

- demo renders framed panels, titles, highlighted selection, status line and
  error message without embedding terminal or CP850 constants in application
  code.

---

## WP-TUI-5: Logical cell map and dirty batching

Implement optional full-screen logical cell storage.

Requirements:

- track `(character, attribute)` per cell;
- mark modified regions/cells dirty;
- coalesce adjacent changes into horizontal output runs;
- minimize redundant cursor and attribute sequences;
- one `tui_flush()` emits the pending screen changes.

Acceptance:

- drawing an entire form through many TUI calls results in a small number of
  console writes rather than one write per cell;
- unchanged cells are not retransmitted.

This package may be implemented earlier if it simplifies WP-TUI-2/4.

---

## WP-TUI-6: Save/restore regions

Implement:

```text
tui_saved_size
tui_save
tui_restore
```

Requirements:

- caller supplies storage;
- no heap requirement;
- clipping and buffer-size validation;
- restored cells participate in normal dirty flushing.

Acceptance:

- a popup can cover an arbitrary region and later restore the exact previous
  contents.

---

## WP-TUI-7: Basic widget library

Create separate `libtui_widgets`.

Initial controls:

```text
status bar
menu bar
message box
confirmation box
label
button
input field
checkbox
list box
```

Requirements:

- procedural API;
- no object hierarchy;
- widgets built entirely on `libtui`;
- keyboard-driven focus/navigation;
- no new OS API.

Acceptance:

- a small configuration utility can be written with menu, list, editable field,
  buttons, and modal confirmation without direct terminal handling.

---

## WP-TUI-8: SDK documentation and examples

Provide:

```text
include/tui.h
include/tui_widgets.h

examples/tui_demo
examples/tui_keys
examples/tui_dialog
```

Document:

- coordinate rules;
- window clipping;
- style semantics;
- key values;
- VT100 decoding boundary;
- batching/flush rules;
- lifecycle expectations.

Acceptance:

- an application author can write a basic panel/menu application using only
  the public documentation and headers.

---

## 16. Recommended initial stopping point

The first useful milestone is **WP-TUI-1 through WP-TUI-5**.

That provides:

```text
screen management
windows
cursor positioning
batched text
styles
frames
special-key decoding
dirty updates
```

which is already enough for substantial applications such as:

- diagnostics;
- file managers;
- compiler front-ends;
- editors;
- setup utilities;
- non-graphical tracker tools.

`libtui_widgets` can then be driven by a real application instead of being
designed speculatively.

---

## 17. Core architectural rule

The most important boundary is:

> The IOC owns keyboard-to-VT100 translation.  
> The console owns byte transport.  
> `libtui` owns VT100-to-TUI interpretation.  
> Applications own UI behavior.

This keeps the hardware, OS, SDK, and application layers cleanly separated.
