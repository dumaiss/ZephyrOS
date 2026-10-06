/* ZephyrOS M10 design spike: auxiliary segment 1, code + BSS (33b/33d).
 *
 * Self-contained (no libzos) so it links into a segment with no
 * unresolved externals.  seg1_entry is far entry 1 and uses the far-call
 * frame ABI: on entry HL points at a 32-byte frame holding in_len input
 * bytes; it writes out_len output bytes back and returns its 16-bit
 * result (the gate captures it).
 */

#include "zos/types.h"

static uint16_t seg1_counter;

uint16_t seg1_entry(uint8_t *frame)
{
	uint16_t x = (uint16_t)(frame[0] | ((uint16_t)frame[1] << 8));
	uint16_t y = (uint16_t)(frame[2] | ((uint16_t)frame[3] << 8));
	uint16_t r = (uint16_t)(x + y + seg1_counter);

	frame[0] = (uint8_t)(r & 0xFFu);
	frame[1] = (uint8_t)(r >> 8);
	seg1_counter = (uint16_t)(seg1_counter + 1u);
	return r;
}
