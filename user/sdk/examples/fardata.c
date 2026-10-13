/* KonaOS far-data SDK demo: the primary half.
 *
 * The smallest complete far-data program: `levels.c` is compiled through
 * the exe_program `DATA1=` option (its `const` table in the area
 * `_DATA1`, its zeroed array in `_DATA1B`, the pair linked at 8000h as
 * one mappable DATA segment) and `fardata_far.c` through `FAR1=` (a
 * `__banked` entry, the container's first row).
 *
 * The program maps its own segment and uses the sources' symbols as plain
 * pointers while the window is open, writes the zeroed half, copies two
 * values out of the table, restores the caller's mapping, and only then
 * makes the banked call - one far window serves far code or far data,
 * never both (architecture 7.2).
 */

#include "kos.h"

#define SEGMENT 1u

__sfr __at 0x00 latch_port;

/* The DATA1 sources' symbols: 8000h and up, visible while segment 1 is
 * mapped. */
extern const unsigned char levels[];
extern unsigned char history[];

/* The FAR1 entry (fardata_far.c): `__banked`, so a plain C call goes
 * through the SDK trampoline. */
extern int fardata_far_sum(int a, int b) __banked;

int main(void)
{
	unsigned char before;
	unsigned char saved = 0u;
	void *window;
	unsigned int i;

	console_puts("fardata: begin\r\n");

	before = latch_port;
	window = farmap(SEGMENT, &saved);
	if (window != (void *)0x8000u) {
		printf("fardata: FAIL map %d\r\n", (int)saved);
		return 1;
	}
	printf("fardata: window %04x\r\n", (unsigned int)window);

	/* The 4 KiB table, read through the open window. */
	if (levels[0] != 0x10u || levels[3u * 512u] != 0x40u ||
	    levels[7u * 512u] != 0x80u) {
		console_puts("fardata: FAIL table\r\n");
		return 1;
	}

	/* The 2 KiB zeroed half: zeroes before the first write, then ours. */
	if (history[0] != 0u || history[2047] != 0u) {
		console_puts("fardata: FAIL zeroes\r\n");
		return 1;
	}
	for (i = 0u; i < 2048u; ++i) {
		history[i] = (unsigned char)i;
	}
	if (history[0] != 0u || history[1000] != (unsigned char)1000u ||
	    history[2047] != (unsigned char)2047u) {
		console_puts("fardata: FAIL write\r\n");
		return 1;
	}
	console_puts("fardata: table and history OK\r\n");

	/* Two values copied out of the table, the mapping handed back, then
	 * the banked call: the far half reads no pointer into the window. */
	{
		int a = levels[0];
		int b = levels[512];

		farunmap(saved);
		if (latch_port != before) {
			console_puts("fardata: FAIL latch\r\n");
			return 1;
		}
		printf("fardata: far sum %04x\r\n",
		       (unsigned int)fardata_far_sum(a, b));
	}

	console_puts("fardata: done\r\n");
	return 0;
}
