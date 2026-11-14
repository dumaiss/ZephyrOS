/* KonaOS SDK: keyboard keys from the console input.
 *
 * The keyboard reaches a program as console bytes: the HID driver
 * translates each keypress to text, with the editing and function keys
 * as VT100 sequences.  This decoder turns those bytes back into one key
 * code each - a byte (1..255) for an ordinary character, a KEY_* code for
 * the others - for any program: a graphics program, a game, a console
 * tool.  It needs no setup: it reads console_read() on demand and keeps a
 * few bytes of parser state.  libtui's tui_getkey() is this decoder with
 * the TUI's mouse events folded in, and its TUI_KEY_* codes are these.
 *
 * A lone ESC is delivered at once when no continuation byte is already
 * pending (one immediate look-ahead, no timer).  An unknown or malformed
 * sequence is delivered as KEY_ESC followed by its bytes as characters.
 * The input is text: there are no key-up events and no held-key state.
 */

#ifndef KOS_SDK_KEY_H
#define KOS_SDK_KEY_H

#include "kos/types.h"

typedef uint16_t key_code_t;

enum {
	KEY_NONE   = 0x0000,

	KEY_ESC    = 0x001B,

	KEY_UP     = 0x0101,
	KEY_DOWN   = 0x0102,
	KEY_LEFT   = 0x0103,
	KEY_RIGHT  = 0x0104,

	KEY_HOME   = 0x0110,
	KEY_END    = 0x0111,
	KEY_PGUP   = 0x0112,
	KEY_PGDN   = 0x0113,
	KEY_INSERT = 0x0114,
	KEY_DELETE = 0x0115,

	KEY_F1     = 0x0121,
	KEY_F2     = 0x0122,
	KEY_F3     = 0x0123,
	KEY_F4     = 0x0124,
	KEY_F5     = 0x0125,
	KEY_F6     = 0x0126,
	KEY_F7     = 0x0127,
	KEY_F8     = 0x0128,
	KEY_F9     = 0x0129,
	KEY_F10    = 0x012A,
	KEY_F11    = 0x012B,
	KEY_F12    = 0x012C
};

/* 1 when a complete key can be read without waiting, 0 when not, or a
 * negated error from the console. */
int key_pending(void);

/* The next key, or KEY_NONE when no complete key is pending. */
key_code_t key_poll(void);

/* The next key, waiting for one; the wait is paced like getchar()'s
 * (idle_input). */
key_code_t key_get(void);

/* Drop any partly decoded sequence and pending key (a program that also
 * reads the console another way calls this before decoding again). */
void key_reset(void);

#endif /* KOS_SDK_KEY_H */
