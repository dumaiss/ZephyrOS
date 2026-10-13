/* KonaOS far-code SDK demo: the far half.
 *
 * Compiled with `--codeseg FAR1` and bank 1 through the exe_program
 * FAR1= option and linked at 8000h, so this whole module becomes the
 * `_FAR1` area the container loads into the aperture slice of one
 * auxiliary bank, as segment 1.  The `__banked` definition also makes
 * the module emit `b_fardemo_far_main = 1`, the id the caller's banked
 * call names.
 * While the primary half has the aperture open, this code runs in place
 * at 8000h: the primary's image, BSS and stack stay outside the aperture
 * (the packer checks that rule), so the far code reaches primary
 * functions and its own syscalls normally.
 */

#include "kos.h"

/* The banking latch (port 00h): mode in D4-D3, bank in D2-D0, page in
 * D7-D5.  The aperture is open, so P is the segment's page. */
__sfr __at 0x00 latch_port;

/* Primary code, called from the far bank (linked below 8000h). */
extern int fardemo_add(int value);

/* Part of the _FAR1 area: while the aperture is open this literal is in
 * the far bank, so the console syscall that names it resolves there. */
static const char far_line[] = "fardemo: hello from the far bank\r\n";

int fardemo_far_main(int seed, int unused) __banked
{
	unsigned char latch = latch_port;
	int value;

	(void)unused;

	/* A console syscall from the far bank, its buffer in the
	 * aperture. */
	(void)console_write(far_line,
				(unsigned int)(sizeof(far_line) - 1u));

	/* Primary code: a plain call out of the aperture. */
	value = fardemo_add(seed);

	/* The latch proves which bank ran; the caller compares it with
	 * the token SYS_SEG_MAP returned. */
	return (int)(((unsigned int)latch << 8) |
		     (unsigned int)(value & 0xFF));
}
