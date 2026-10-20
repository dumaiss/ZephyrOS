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
#define TUI_RENDER_MAX TUI_RUN_MAX

/* A bounded transaction nesting count; begin/end calls beyond the bound
 * saturate, so a balanced pair still flushes at its outermost end. */
#define TUI_TRANSACTION_MAX 8u

/* Nested logical windows kept by push/pop. */
#define TUI_WINDOW_STACK_MAX 4u

struct tui_state {
	uint8_t initialized;
	struct tui_size screen;
	struct tui_rect window;
	uint8_t cursor_x;
	uint8_t cursor_y;
	uint8_t cursor_visible;
	uint8_t style;
	uint8_t attr;
	uint8_t transaction_depth;
	struct tui_rect window_stack[TUI_WINDOW_STACK_MAX];
	uint8_t window_depth;
	uint16_t key_generation;

	/* What the terminal currently shows, for suppressing redundant
	 * cursor, SGR and visibility sequences. */
	uint8_t phys_x;
	uint8_t phys_y;
	uint8_t phys_attr;
	uint8_t phys_cursor_visible;
	uint8_t render_support; /* 0 unknown, 1 RENDER, 2 ANSI fallback */
};

extern struct tui_state tui_state;
extern struct tui_cell tui_cells[TUI_CELL_COUNT];
extern uint8_t tui_dirty[TUI_DIRTY_BYTES];
extern uint8_t tui_invert[TUI_DIRTY_BYTES];
extern uint8_t tui_run[TUI_RUN_MAX];
/* RENDER and ANSI fallback are mutually exclusive during a flush. */
#define tui_render tui_run

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
#define TUI_INVERT_TEST(index) \
	((tui_invert[(uint16_t)(index) >> 3] & \
	  (uint8_t)(1u << ((uint16_t)(index) & 7u))) != 0u)
#define TUI_INVERT_SET(index) \
	(tui_invert[(uint16_t)(index) >> 3] |= \
	 (uint8_t)(1u << ((uint16_t)(index) & 7u)))
#define TUI_INVERT_CLEAR(index) \
	(tui_invert[(uint16_t)(index) >> 3] &= \
	 (uint8_t)~(1u << ((uint16_t)(index) & 7u)))
#define TUI_PENDING_TEST(index) \
	(TUI_DIRTY_TEST(index) || TUI_INVERT_TEST(index))

/* Draw one byte with the window-relative cursor held in x and y.
 * Control bytes move that cursor; printable bytes clip to the active
 * window. */
void tui_priv_put_byte(uint8_t *x, uint8_t *y, uint8_t ch);

/* Clip a window-relative rectangle to the active window.  Null pointers
 * are -KOS_EINVAL; an empty clip is successful with w/h zero. */
int tui_priv_clip_rect(const struct tui_rect *rect, struct tui_rect *clipped);

/* Draw one cell at a window-relative coordinate, clipped to the window.
 * The cell is dirtied only when its character or attribute changes. */
void tui_priv_set_cell(uint8_t x, uint8_t y, uint8_t ch, uint8_t attr);

/* Fill a rectangle already clipped by tui_priv_clip_rect(). */
void tui_priv_fill_clipped(const struct tui_rect *clipped, uint8_t ch,
			   uint8_t attr);

#endif /* KONA_TUI_INTERNAL_H */
