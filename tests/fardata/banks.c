/* KonaOS far-data replay fixtures (work package 49d): the bank-exhaustion
 * container's primary.
 *
 * The program itself never runs: its container declares six FAR_CODE rows
 * (banks.segments.json, all six over the one `_FAR1` area this link
 * produces), which is one auxiliary bank more than the machine can give a
 * child, so `run("/system/banks.exe", "")` is refused with `-KOS_ENOSPC`
 * before any of this code is reached.  The replay asserts the refusal and
 * that nothing was allocated.
 */

#include "kos.h"

int main(void)
{
	return 0;
}
