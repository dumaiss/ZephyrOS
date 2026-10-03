/* ZephyrOS M4 loader test fixture: BSS-heavy filler.
 *
 * Registers no devices and claims no IRQs.  Its init verifies that the
 * whole 600-byte private-state pad was zeroed by the loader (the SRAM
 * behind the arena is not zero at boot, so a missed BSS clear shows up as
 * a failure) and then returns success.  The image plus BSS spans more
 * than one granule, which lets the M4 self-test drive the arena into
 * granule exhaustion without exhausting the module descriptor table.
 */

#include "zephyros/driver.h"

#define M4_FILLER_PAD_SIZE 600u

struct m4_filler_state {
	uint16_t magic;
	uint8_t pad[M4_FILLER_PAD_SIZE];
};

struct m4_filler_state m4_filler_state;

int m4_filler_init(const struct zdriver_api *api)
{
	uint16_t i;

	(void)api;
	for (i = 0; i < M4_FILLER_PAD_SIZE; ++i) {
		if (m4_filler_state.pad[i] != 0) {
			return ZDRIVER_FAIL;
		}
	}
	if (m4_filler_state.magic != 0) {
		return ZDRIVER_FAIL;
	}
	m4_filler_state.magic = 0x4D46u;
	return ZDRIVER_OK;
}
