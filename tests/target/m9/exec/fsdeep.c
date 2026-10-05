/* ZephyrOS M9 SYS_EXEC depth-probe program (test artifact, package 28).
 *
 * Packed as fsdeep.zex and launched by the M9 exec acceptance hook.  It
 * recursively execs itself; at ZOS_EXEC_MAX_DEPTH the next exec is refused
 * with ZOS_EBUSY before anything is allocated.  Each level encodes its
 * depth in the exit status handed up by its parent, so the outermost
 * instance exits with exactly the number of live levels:
 *
 *   hook (depth 0) -> fsdeep depth 1 -> depth 2 -> depth 3 -> depth 4
 *   depth 4's zos_exec returns -ZOS_EBUSY, so depth 4 exits 1, depth 3
 *   exits 2, depth 2 exits 3 and depth 1 exits 4.
 *
 * A status of 4 proves the chain reached exactly four user processes and
 * that the fifth exec was refused.
 */

#define DP_SELF  "/SYSTEM/fsdeep.zex"

#include "zos.h"
int main(void)
{
	int result = zos_exec(DP_SELF, (const char *)0);

	if (result == -(int)ZOS_EBUSY) {
		zex_exit(1u); /* deepest level: the refusal is the proof */
	}
	if (result < 0) {
		zex_exit(0x00FFu); /* unexpected failure */
	}
	zex_exit((unsigned int)(result + 1));
	return 0; /* unreachable */
}
