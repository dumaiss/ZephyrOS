/* libtui caller-buffered save and restore of clipped cell regions.
 *
 * Rectangles are relative to the active window and clipped once by the
 * common clipping layer.  The caller owns the only backing store; this
 * member has no pool and never allocates or flushes.  Restore marks only
 * cells whose character or resolved attribute actually changes.
 */

#include "tui_internal.h"

static uint16_t tui_region_bytes(const struct tui_rect *rect)
{
	uint16_t cells = 0u;
	uint8_t y;

	for (y = 0u; y < rect->h; ++y) {
		cells = (uint16_t)(cells + rect->w);
	}
	return (uint16_t)(cells << 1);
}

uint16_t tui_saved_size(const struct tui_rect *rect)
{
	struct tui_rect clipped;

	if (tui_state.initialized == 0u ||
	    tui_priv_clip_rect(rect, &clipped) < 0) {
		return 0u;
	}
	return tui_region_bytes(&clipped);
}

int tui_save(const struct tui_rect *rect, struct tui_cell *buffer,
	     uint16_t buffer_size)
{
	struct tui_rect clipped;
	uint16_t required;
	uint16_t at = 0u;
	uint8_t x;
	uint8_t y;

	if (tui_state.initialized == 0u ||
	    buffer == (struct tui_cell *)0) {
		return -KOS_EINVAL;
	}
	if (tui_priv_clip_rect(rect, &clipped) < 0) {
		return -KOS_EINVAL;
	}
	required = tui_region_bytes(&clipped);
	if (buffer_size < required) {
		return -KOS_EINVAL;
	}
	for (y = 0u; y < clipped.h; ++y) {
		for (x = 0u; x < clipped.w; ++x) {
			uint16_t index = TUI_CELL_INDEX(
				(uint8_t)(tui_state.window.x + clipped.x + x),
				(uint8_t)(tui_state.window.y + clipped.y + y));
			buffer[at] = tui_cells[index];
			++at;
		}
	}
	return 0;
}

int tui_restore(const struct tui_rect *rect,
		const struct tui_cell *buffer, uint16_t buffer_size)
{
	struct tui_rect clipped;
	uint16_t required;
	uint16_t at = 0u;
	uint8_t x;
	uint8_t y;

	if (tui_state.initialized == 0u ||
	    buffer == (const struct tui_cell *)0) {
		return -KOS_EINVAL;
	}
	if (tui_priv_clip_rect(rect, &clipped) < 0) {
		return -KOS_EINVAL;
	}
	required = tui_region_bytes(&clipped);
	if (buffer_size < required) {
		return -KOS_EINVAL;
	}
	for (y = 0u; y < clipped.h; ++y) {
		for (x = 0u; x < clipped.w; ++x) {
			uint16_t index = TUI_CELL_INDEX(
				(uint8_t)(tui_state.window.x + clipped.x + x),
				(uint8_t)(tui_state.window.y + clipped.y + y));
			tui_priv_set_cell((uint8_t)(clipped.x + x),
					  (uint8_t)(clipped.y + y),
					  buffer[at].ch, buffer[at].attr);
			++at;
		}
	}
	return 0;
}
