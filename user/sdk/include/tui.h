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
