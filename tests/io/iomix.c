/* KonaOS SDK console-order acceptance: the interleaving program.
 *
 * Console output is line-buffered (docs/sdk.md), so this program proves
 * the buffer never reorders a program's own output against what it reads
 * or against a child's output:
 *
 *   * two markers go out with no newline, so they can only arrive
 *     through the flush rules - before the line that is read next, and
 *     before the child `run` starts;
 *   * the last marker has no newline either, so it can only arrive
 *     through the exit path's flush;
 *   * the transcript between the markers is exactly what an unbuffered
 *     program produces, which is what `tests/test_io_sim.py` asserts.
 *
 * It runs from the monitor's X as `/system/iomix.exe` with a line of
 * input on the console.
 */

#include "kos.h"

#define CHILD "/system/iomix_child.exe"

int main(void)
{
	char line[KOS_LINE_MAX + 1];
	int length;

	/* No newline: the marker must be on the wire before the line
	 * reader asks for input, or the prompt would arrive after the
	 * echo of the line it prompted for. */
	(void)console_write("iomix: prompt ", 14u);
	length = console_readline(line, sizeof(line));
	if (length < 0) {
		printf("iomix: readline %d\r\n", length);
		return 1;
	}
	printf("iomix: read \"%s\"\r\n", line);

	/* No newline again: `run` flushes first, so the marker precedes
	 * every byte the child writes. */
	(void)console_write("iomix: before-child ", 20u);
	length = run(CHILD, "iomix");
	if (length != 0) {
		printf("iomix: child %d\r\n", length);
		return 2;
	}

	/* The last marker has no newline: only the exit path's flush can
	 * deliver it, and it must land before the monitor reports the
	 * status of this program. */
	(void)console_write("iomix: after-child", 18u);
	return 0;
}
