/* Private libtui storage.
 *
 * One definition shared by the screen, lifecycle and transaction members:
 * the logical cell map, dirty/invert bitmaps, flush buffers and state. A
 * program links only the storage its called members need.
 */

#include "tui_internal.h"

struct tui_state tui_state;
uint8_t tui_chars[TUI_CELL_COUNT];
uint8_t tui_attrs[TUI_ATTR_BYTES];

uint8_t tui_priv_attr(uint16_t index)
{
	uint8_t pair = tui_attrs[(uint16_t)(index >> 1)];

	return (uint8_t)(((index & 1u) != 0u ? (uint8_t)(pair >> 4) : pair) &
			 0x0Fu);
}

void tui_priv_store(uint16_t index, uint8_t ch, uint8_t attr)
{
	uint8_t *pair = &tui_attrs[(uint16_t)(index >> 1)];

	tui_chars[index] = ch;
	if ((index & 1u) != 0u) {
		*pair = (uint8_t)((*pair & 0x0Fu) | (uint8_t)(attr << 4));
	} else {
		*pair = (uint8_t)((*pair & 0xF0u) | (attr & 0x0Fu));
	}
}
uint8_t tui_dirty[TUI_DIRTY_BYTES];
uint8_t tui_invert[TUI_DIRTY_BYTES];
uint8_t tui_run[TUI_RUN_MAX];
uint8_t tui_row_pending[TUI_MAX_ROWS];
