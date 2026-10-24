/* Private libtui state.
 *
 * Only libtui sources include this header.  The state and cell map are
 * static storage owned by the library; they are not part of the public
 * ABI.
 */

#ifndef KONA_TUI_INTERNAL_H
#define KONA_TUI_INTERNAL_H

#include "tui.h"

/* The compiled capacity of the logical surface: 85 x 26, or the grid the
 * archive is built for (TUI_GRID_COLS/ROWS, a program's TUI_GRID option).
 * The screen itself is the grid the console reports (tui_init), clamped to
 * this; the cell map is indexed with the capacity's stride whatever the
 * grid. */
#ifndef TUI_GRID_COLS
#define TUI_GRID_COLS 85u
#define TUI_GRID_ROWS 26u
#endif
#define TUI_MAX_COLS TUI_GRID_COLS
#define TUI_MAX_ROWS TUI_GRID_ROWS
/* The cell map's row stride: the column capacity rounded up to an even
 * number, so every row starts on an attribute-byte boundary. */
#define TUI_STRIDE ((uint16_t)((TUI_MAX_COLS + 1u) & ~1u))
_Static_assert((TUI_STRIDE & 1u) == 0u, "TUI_STRIDE must be even");
#define TUI_CELL_COUNT ((uint16_t)TUI_STRIDE * TUI_MAX_ROWS)
/* The attribute store: 4 bits per cell, two cells per byte. */
#define TUI_ATTR_BYTES ((uint16_t)(TUI_CELL_COUNT + 1u) / 2u)
#define TUI_DIRTY_BYTES ((uint16_t)(TUI_CELL_COUNT + 7u) / 8u)

/* One dirty horizontal run starts with a cursor sequence (at most nine
 * bytes) and may carry one SGR sequence (four bytes) and a whole row. */
#define TUI_RUN_MAX (TUI_MAX_COLS + 16u)
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
	uint8_t model_only;     /* update cells without scheduling output */
	uint8_t stride;         /* the cell map's row stride (TUI_STRIDE) */
};

extern struct tui_state tui_state;
/* The cell map: one byte per character, one 4-bit attribute per cell
 * (the even cell in the low nibble).  Read and write through the
 * accessors; `struct tui_cell` is only the save/restore format. */
extern uint8_t tui_chars[TUI_CELL_COUNT];
extern uint8_t tui_attrs[TUI_ATTR_BYTES];
uint8_t tui_priv_attr(uint16_t index);
void tui_priv_store(uint16_t index, uint8_t ch, uint8_t attr);
extern uint8_t tui_dirty[TUI_DIRTY_BYTES];
extern uint8_t tui_invert[TUI_DIRTY_BYTES];
extern uint8_t tui_run[TUI_RUN_MAX];
/* One flag per capacity row: set whenever a cell of the row gains a
 * dirty or invert bit, cleared with the maps, so a flush skips clean
 * rows.  A set flag with no bits is harmless; a clear flag with bits
 * set is a bug. */
extern uint8_t tui_row_pending[TUI_MAX_ROWS];
/* RENDER and ANSI fallback are mutually exclusive during a flush. */
#define tui_render tui_run

#define TUI_CELL_INDEX(x, y) \
	((uint16_t)(((uint16_t)(y) * TUI_STRIDE) + (uint16_t)(x)))
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
