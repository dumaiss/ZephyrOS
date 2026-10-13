/* KonaOS far-data replay fixtures (work package 49d): the FAR1 source.
 *
 * Compiled through the exe_program `FAR1=` option (bank 1, area `_FAR1`,
 * linked at 8000h), so this module is the container's first row, a
 * FAR_CODE segment.  It proves the one-window rule from the other side:
 * a `__banked` function cannot see far data, so the caller passes the
 * table values **by copy** and this half only computes with them.
 *
 * It also returns the latch byte it saw, and the caller reads the same
 * bit of the result the far-code demo does: the high byte is the far
 * bank's page, so the call really ran in the aperture.
 */

#include "kos.h"

/* The banking latch (port 00h): with the aperture open, P is the
 * segment's page. */
__sfr __at 0x00 latch_port;

/* Sum two values the caller copied out of its mapped DATA segment.  No
 * pointer into the aperture is ever passed: while this code runs, the
 * window is this far code, not the data. */
int far_data_far_sum(int a, int b) __banked
{
	unsigned char latch = latch_port;

	return ((int)latch << 8) | ((a + b) & 0xFF);
}
