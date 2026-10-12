/* KonaOS far-code outcome program (work packages 44b/49b): the far half
 * of segment 2.
 *
 * Built with bank 2 and `--codeseg FAR2` through the SDK's FAR2= option
 * and linked at 8000h, so this module becomes the `_FAR2` area and the
 * container loads it into its own auxiliary bank, as segment 2: the
 * primary half maps it and calls `xmem_far_entry2` with `farcall`, and
 * calls `xmem_bk2` as plain C through the SDK's trampoline.
 *
 * `xmem_far_entry2` is the 44b arm of segment 2: it reads its own latch
 * (the aperture is open, so the low byte is segment 2's token), touches
 * primary data through `xmem_far_primary` and returns
 * (latch << 8) | (value & 0xFF), so the caller proves which bank ran.
 *
 * `xmem_bk2` is the 49b arm: reached as plain C, it calls back into
 * segment 1's `__banked` function.  That inner call maps segment 1 and
 * must restore *this* caller's mapping on its way back, or the code after
 * it would run in the wrong bank - so it reads the latch and its own
 * far-area literal after the call and folds both into the result.
 */

#include "kos.h"

/* The banking latch (port 00h): mode in D4-D3, bank in D2-D0, page in
 * D7-D5.  Read directly. */
__sfr __at 0x00 latch_port;

/* The primary half: a function and the far module of segment 1. */
extern int xmem_far_primary(int value);
extern unsigned int xmem_far_marker;
extern unsigned int xmem_bk_16(unsigned int a) __banked;

/* Part of the _FAR2 area: this literal is in segment 2's bank while the
 * aperture is open onto segment 2.  Byte 1 ('a' = 0x61) is the value the
 * primary half expects folded into xmem_bk2's result. */
static const char far2_text[] = "far2";

int xmem_far_entry2(int seed, int unused)
{
	unsigned char latch = latch_port;
	int value;

	/* Primary data, then primary code: two bumps per far call. */
	xmem_far_marker = xmem_far_marker + 1u;
	value = xmem_far_primary(seed);

	(void)unused;
	return (int)(((unsigned int)latch << 8) |
		     (unsigned int)(value & 0xFF));
}

unsigned int xmem_bk2(unsigned int seed) __banked
{
	unsigned int value = xmem_bk_16(seed);
	unsigned char latch = latch_port;

	return (unsigned int)(((unsigned int)latch << 8) |
			      ((value +
				(unsigned char)far2_text[1]) & 0xFFu));
}
