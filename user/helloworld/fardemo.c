/* KonaOS far-code SDK demo (work package 44b): the primary half.
 *
 * The simplest complete far-code program: `fardemo_far.c` is compiled
 * into the `_FAR_CODE` area and linked at 8000h through the exe_program
 * FAR= option, the packer proves the 36.17 link rule (this image, its BSS
 * and the whole stack stay outside 8000h-BFFFh), and `farcall` maps
 * the segment, makes the plain CALL and restores the previous latch.
 *
 * The far code prints its own line from a far-area buffer (a console
 * syscall whose buffer is the aperture) and calls back into primary code
 * (`fardemo_add`); it returns its latch byte, so this half proves which
 * bank ran the code.
 */

#include "kos.h"

#define SEGMENT 1u
#define SEED    0x2Au

/* Primary code the far entry calls directly (address < 8000h). */
int fardemo_add(int value);

int fardemo_add(int value)
{
	return value + 1;
}

/* The far entry (fardemo_far.c, --codeseg FAR_CODE, linked at 8000h). */
extern int fardemo_far_main(int seed, int unused);

int main(void)
{
	int token;
	int result;
	unsigned int expected;

	console_puts("fardemo: begin\r\n");

	/* The aperture token of this program's own FAR_CODE segment.  The
	 * page field P = 7 - bank is 2..7: it never names bank 6 or 7. */
	token = segmap(SEGMENT);
	if (token < 1) {
		printf("fardemo: FAIL map %d\r\n", (unsigned int)token);
		return 1;
	}
	printf("fardemo: token %02x\r\n", (unsigned int)token);

	/* Map, a plain CALL and restore.  The far entry prints its line,
	 * calls primary code and returns (latch << 8) | (seed + 1). */
	result = farcall(SEGMENT, fardemo_far_main, SEED, 0);
	expected = ((unsigned int)token << 8) | (unsigned int)(SEED + 1);
	if ((unsigned int)result != expected) {
		printf("fardemo: FAIL call %04x\r\n",
			   (unsigned int)result);
		return 1;
	}
	printf("fardemo: far result %04x\r\n", (unsigned int)result);

	/* The helper restored the latch: the aperture is closed again. */
	console_puts("fardemo: PASS\r\n");
	return 0;
}
