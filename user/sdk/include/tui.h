/* KonaOS TUI SDK: the core text-screen library.
 *
 * libtui owns VT100 input interpretation and the logical text screen.  The
 * IOC translates keyboard events into VT100 bytes, the console transports
 * and renders bytes, and applications own UI policy.  A program includes
 * this header and links `libtui`; it never emits terminal sequences itself.
 *
 * Results.  Lifecycle, cursor, window, style, drawing, clear, save and
 * restore calls return 0 on success or a negated KOS_E* value on failure.
 * A null required pointer, an invalid enum, an invalid rectangle, an
 * operation before tui_init(), window-stack overflow/underflow, or an
 * unmatched tui_end() returns -KOS_EINVAL.  tui_putc() returns 1;
 * tui_write(), tui_puts() and their _xy forms return the number of input
 * bytes consumed, even where clipping prevents a cell change.  The
 * formatting calls return their formatted byte count.
 *
 * Geometry.  TUI v1 has one logical geometry, which tui_get_size()
 * reports; applications query it instead of embedding the numbers.
 *
 * Storage.  The library has static storage only: no heap and no OS-visible
 * global state.  Save/restore buffers belong to the caller.
 *
 * Lifecycle.  tui_init() makes the terminal and the model agree, and is
 * safe again after tui_shutdown(); a second call while initialized returns
 * -KOS_EINVAL and leaves the screen untouched.  tui_shutdown() is
 * idempotent and restores normal rendition and a visible cursor.
 */

#ifndef KONA_TUI_H
#define KONA_TUI_H

#include "kos/types.h"
#include "kos/errno.h"
#include "kos/key.h"

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

/* Start a transaction; tui_end() flushes when the outermost one ends. */
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
 * Write `length` literal glyph bytes at a window-relative position with
 * one attribute.  Every byte is stored as itself: control bytes have no
 * cursor effect, the cursor does not move, and cells past the window's
 * right edge are clipped.  The effect is that of `length` calls to
 * tui_putc_xy() with `tui_set_attr(attr)` in effect.  Returns 0, or
 * -KOS_EINVAL before tui_init() or for a null `text`.
 */
int tui_put_text(uint8_t x, uint8_t y, const char *text, uint8_t length,
		 uint8_t attr);

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

/*
 * Move a full-width absolute screen region by one row without repainting
 * every retained cell.  The exposed row is filled with the current
 * attribute.  A console without semantic render scrolling returns
 * -KOS_ENOTSUP and leaves the logical screen unchanged.
 */
enum {
	TUI_SCROLL_UP = 0,
	TUI_SCROLL_DOWN
};

int tui_scroll_rows(uint8_t top, uint8_t height, uint8_t direction);

/* ---- window / clipping --------------------------------------------- */

/*
 * Replace the current logical window.
 * Coordinates passed to ordinary cursor/drawing calls become relative
 * to this rectangle.
 */
int tui_window(uint8_t x, uint8_t y, uint8_t w, uint8_t h);

/* Restore the full screen as the current window. */
int tui_reset_window(void);

/* Report the current logical window (a read-only convenience for layers
 * such as widgets that own a row or edge of the active window). */
int tui_get_window(struct tui_rect *out);

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

/* The keys are kos/key.h's: the same decoder, the same codes.  A program
 * that wants keys without the TUI (a graphics program) uses kos/key.h
 * directly. */
enum {
	TUI_KEY_NONE      = KEY_NONE,

	TUI_KEY_ESC       = KEY_ESC,

	TUI_KEY_UP        = KEY_UP,
	TUI_KEY_DOWN      = KEY_DOWN,
	TUI_KEY_LEFT      = KEY_LEFT,
	TUI_KEY_RIGHT     = KEY_RIGHT,

	TUI_KEY_HOME      = KEY_HOME,
	TUI_KEY_END       = KEY_END,
	TUI_KEY_PGUP      = KEY_PGUP,
	TUI_KEY_PGDN      = KEY_PGDN,
	TUI_KEY_INSERT    = KEY_INSERT,
	TUI_KEY_DELETE    = KEY_DELETE,

	TUI_KEY_F1        = KEY_F1,
	TUI_KEY_F2        = KEY_F2,
	TUI_KEY_F3        = KEY_F3,
	TUI_KEY_F4        = KEY_F4,
	TUI_KEY_F5        = KEY_F5,
	TUI_KEY_F6        = KEY_F6,
	TUI_KEY_F7        = KEY_F7,
	TUI_KEY_F8        = KEY_F8,
	TUI_KEY_F9        = KEY_F9,
	TUI_KEY_F10       = KEY_F10,
	TUI_KEY_F11       = KEY_F11,
	TUI_KEY_F12       = KEY_F12,

	TUI_KEY_MOUSE     = 0x0140  /* the TUI's own: a mouse event */
};

/* Nonzero when enough input is pending to return a key without blocking. */
int tui_key_pending(void);

/* Blocking read of one decoded key. */
tui_key_t tui_getkey(void);

/* Non-blocking read. Returns TUI_KEY_NONE if no complete key is available. */
tui_key_t tui_pollkey(void);

/* ---- mouse ---------------------------------------------------------- */

/*
 * Opt-in mouse.  tui_mouse_enable() after tui_init() opens the mouse and
 * the pointer, tracks over the pointer's 510 x 208 pixel text area and
 * shows the pointer.  From then on tui_pollkey()/tui_getkey() return
 * TUI_KEY_MOUSE when a mouse read changed something worth an event, after
 * any key already pending; tui_mouse_get() returns that event.  The
 * pointer follows the mouse on every read that moved it.
 *
 * Event kinds.  One read yields at most one event; `kind` names the first
 * of: a press, a release, wheel notches, a motion to another cell while a
 * button is held (a drag) or, with TUI_MOUSE_MOTION, without one.  Notches
 * that arrive with a press or release stay pending and come as a WHEEL
 * event of the next read; `wheel` is nonzero only on that event.
 */
enum {
	TUI_MOUSE_NONE = 0,
	TUI_MOUSE_PRESS,
	TUI_MOUSE_RELEASE,
	TUI_MOUSE_WHEEL,
	TUI_MOUSE_MOVE
};

/* `buttons`, `pressed` and `released`. */
#define TUI_MOUSE_LEFT   0x01u
#define TUI_MOUSE_RIGHT  0x02u
#define TUI_MOUSE_MIDDLE 0x04u

/* tui_mouse_enable() flags: also report motion with no button held. */
#define TUI_MOUSE_MOTION 0x01u

struct tui_mouse {
	uint8_t kind;
	uint8_t col;      /* screen cell: pixel x / 6, y / 8, on the screen */
	uint8_t row;
	int8_t  wcol;     /* the cell in the active window, both -1 outside */
	int8_t  wrow;
	uint8_t buttons;  /* held */
	uint8_t pressed;  /* went down since the previous read */
	uint8_t released; /* went up since the previous read */
	int16_t wheel;    /* signed notches, positive away from the user */
};

/*
 * 0, or a negative error with the program left keyboard-only:
 * -KOS_EINVAL before tui_init(); -KOS_ENOTSUP when the console has no
 * render path for the pointer to draw over (the serial console, or the
 * video console while its serial rescue tee is armed), before anything is
 * bound; the bind error when the build publishes no mouse or no video;
 * -KOS_EBUSY while the video is held exclusively.  Enabling again keeps
 * the handles and takes the new flags.
 */
int  tui_mouse_enable(uint8_t flags);

/* Hide the pointer and release both handles; tui_shutdown() calls it. */
void tui_mouse_disable(void);

/* The event of the latest TUI_KEY_MOUSE, its window cell against the
 * active window: 0, or -KOS_EINVAL for a null `ev` or with the mouse not
 * enabled. */
int  tui_mouse_get(struct tui_mouse *ev);

/* 1 when the event's window cell is inside `rect` (active-window
 * coordinates), else 0, also for a null pointer. */
int  tui_mouse_hit(const struct tui_mouse *ev, const struct tui_rect *rect);

/*
 * The way the key loop, tui_shutdown() and the widget runners reach the
 * mouse member: null until tui_mouse_enable() installs it, so a program
 * that never enables the mouse links no mouse code.  Layers over libtui
 * call through it; applications call the functions above.
 */
struct tui_mouse_hook {
	int  (*poll)(void);
	void (*disable)(void);
	int  (*get)(struct tui_mouse *ev);
};

extern const struct tui_mouse_hook *tui_mouse_hook;

/* ---- save / restore ------------------------------------------------- */

/*
 * Save/restore a rectangular cell region.
 *
 * Required bytes are the active-window-clipped rectangle's area times
 * sizeof(struct tui_cell).  Use tui_saved_size() for the exact value.
 */
uint16_t tui_saved_size(const struct tui_rect *rect);

int tui_save(const struct tui_rect *rect,
	     struct tui_cell *buffer,
	     uint16_t buffer_size);

int tui_restore(const struct tui_rect *rect,
		const struct tui_cell *buffer,
		uint16_t buffer_size);

#endif
