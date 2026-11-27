/* KonaOS far-data SDK demo: the far half.
 *
 * Compiled with `--codeseg FAR1` and bank 1 through the exe_program
 * `FAR1=` option and linked at 8000h, so this module is the container's
 * first row, a FAR_CODE segment in the aperture slice of its own bank.
 * The `__banked` definition emits `b_fardata_far_sum = 1`, the id the
 * caller's banked call names.
 *
 * A far function cannot see far data: while it runs, the window is this
 * code.  So the caller copies table values out and passes them by value
 * (the one-window rule).
 */

#include "kos.h"

int fardata_far_sum(int a, int b) __banked
{
	return a + b;
}
