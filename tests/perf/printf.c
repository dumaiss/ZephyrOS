/* Simulator formatted-console probe: printf lines, controlled by the host.
 * A program of its own, so the main probe's image and the cache state its
 * load leaves stay as they are. */
#include "kos.h"

unsigned int perf_items, perf_result;

void perf_ready(void) { __asm nop __endasm; }
/* `perf_items` lines, each written out at its '\n'; the result is the
 * number of characters formatted. */
void perf_printf(void)
{
	unsigned int line, total = 0u;

	for (line = 0u; line < perf_items; line++) {
		total += (unsigned int)printf("%5u %04x %s\n", line,
					      line * 7u, "formatted");
	}
	perf_result = total;
}

int main(void)
{
	perf_ready();
	for (;;) { __asm halt __endasm; }
}
