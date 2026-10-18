/* Private libtui state.
 *
 * Only libtui sources include this header.  The state is static storage
 * owned by the library; it is not part of the public ABI.
 */

#ifndef KONA_TUI_INTERNAL_H
#define KONA_TUI_INTERNAL_H

#include "tui.h"

/* The logical terminal surface: the V9958 console's text geometry. */
#define TUI_LOGICAL_COLS 85u
#define TUI_LOGICAL_ROWS 26u

/* A bounded transaction nesting count; begin/end calls beyond the bound
 * saturate, so a balanced pair still flushes at its outermost end. */
#define TUI_TRANSACTION_MAX 8u

struct tui_state {
	uint8_t initialized;
	struct tui_size screen;
	uint8_t transaction_depth;
};

extern struct tui_state tui_state;

#endif /* KONA_TUI_INTERNAL_H */
