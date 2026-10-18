/* Private libtui state.
 *
 * Only libtui sources include this header.  The state and cell map are
 * static storage owned by the library; they are not part of the public
 * ABI.
 */

#ifndef KONA_TUI_INTERNAL_H
#define KONA_TUI_INTERNAL_H

#include "tui.h"

/* The logical terminal surface: the V9958 console's text geometry. */
#define TUI_LOGICAL_COLS 85u
#define TUI_LOGICAL_ROWS 26u
#define TUI_CELL_COUNT ((uint16_t)TUI_LOGICAL_COLS * TUI_LOGICAL_ROWS)
#define TUI_DIRTY_BYTES ((uint16_t)(TUI_CELL_COUNT + 7u) / 8u)

/* One dirty horizontal run starts with a cursor sequence (at most nine
 * bytes) and may carry one SGR sequence (four bytes) and a whole row. */
#define TUI_RUN_MAX (TUI_LOGICAL_COLS + 16u)

/* A bounded transaction nesting count; begin/end calls beyond the bound
 * saturate, so a balanced pair still flushes at its outermost end. */
#define TUI_TRANSACTION_MAX 8u

struct tui_state {
	uint8_t initialized;
	struct tui_size screen;
	struct tui_rect window;
	uint8_t cursor_x;
	uint8_t cursor_y;
	uint8_t cursor_visible;
	uint8_t attr;
	uint8_t transaction_depth;

	/* What the terminal currently shows, for suppressing redundant
	 * cursor, SGR and visibility sequences. */
	uint8_t phys_x;
	uint8_t phys_y;
	uint8_t phys_attr;
	uint8_t phys_cursor_visible;
};

extern struct tui_state tui_state;
extern struct tui_cell tui_cells[TUI_CELL_COUNT];
extern uint8_t tui_dirty[TUI_DIRTY_BYTES];
extern uint8_t tui_run[TUI_RUN_MAX];

#define TUI_CELL_INDEX(x, y) \
	((uint16_t)(((uint16_t)(y) * TUI_LOGICAL_COLS) + (uint16_t)(x)))
#define TUI_DIRTY_TEST(index) \
	((tui_dirty[(uint16_t)(index) >> 3] & \
	  (uint8_t)(1u << ((uint16_t)(index) & 7u))) != 0u)
#define TUI_DIRTY_SET(index) \
	(tui_dirty[(uint16_t)(index) >> 3] |= \
	 (uint8_t)(1u << ((uint16_t)(index) & 7u)))
#define TUI_DIRTY_CLEAR(index) \
	(tui_dirty[(uint16_t)(index) >> 3] &= \
	 (uint8_t)~(1u << ((uint16_t)(index) & 7u)))

/* Draw one byte with the window-relative cursor held in x and y.
 * Control bytes move that cursor; printable bytes clip to the active
 * window. */
void tui_priv_put_byte(uint8_t *x, uint8_t *y, uint8_t ch);

#endif /* KONA_TUI_INTERNAL_H */
