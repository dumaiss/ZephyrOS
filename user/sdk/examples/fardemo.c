/* KonaOS far-code SDK demo (work packages 44b/49b): the primary half.
 *
 * The simplest complete far-code program: `fardemo_far.c` is compiled
 * into the `_FAR1` area and linked at 8000h through the exe_program
 * FAR1= option, so it is segment 1, the packer proves the 36.17 link rule
 * (this image, its BSS and the whole stack stay outside 8000h-BFFFh), and
 * the far entry is called as plain C: it is `__banked`, so SDCC emits the
 * banked call and the SDK's trampoline maps the segment and restores the
 * previous latch (work package 49b).
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

/* The far entry (fardemo_far.c, linked at 8000h as segment 1):
 * `__banked`, so a plain C call goes through the SDK trampoline, which
 * maps the segment the definition declared (FAR1=, id 1). */
extern int fardemo_far_main(int seed, int unused) __banked;

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

	/* The banked call: a plain C call, mapped by the trampoline from
	 * the id the definition declared.  The far entry prints its line,
	 * calls primary code and returns (latch << 8) | (seed + 1). */
	result = fardemo_far_main(SEED, 0);
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
