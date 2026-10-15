/* KonaOS SDK console-order acceptance: the nested-exec child.
 *
 * It prints one line of its own and returns 0, so the parent's replay
 * can tell which process wrote what: an unbuffered parent's pending
 * bytes appear before this line, a buffered one that flushed too late
 * would put them after it.
 */

#include "kos.h"

int main(void)
{
	console_puts("iomix: child ran\r\n");
	return 0;
}
