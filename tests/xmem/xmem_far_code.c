/* KonaOS far-code outcome program: the far half
 * of segment 1.
 *
 * Built with bank 1 and `--codeseg FAR1` through the SDK's FAR1= option
 * and linked at 8000h, so this module - its code and its string literals -
 * becomes the `_FAR1` area the container loads into the aperture slice of
 * an auxiliary bank.  The primary half maps the segment and calls
 * `xmem_far_entry` with `farcall` (the primitive: an OUT with the
 * aperture token, then a plain CALL to the entry's linked address), and
 * calls the `__banked` entries below as plain C, through the SDK's
 * trampoline.
 *
 * The entry proves the far-code rules of architecture 7.2 from inside:
 *
 *   * it reads its own banking latch - the aperture is open, so P is the
 *     segment's page (7 - bank);
 *   * it touches primary data (`xmem_far_marker`) - data used while the
 *     aperture is open lives outside 8000h-BFFFh of the primary image;
 *   * it calls primary code directly (the linker resolves the call);
 *   * it runs syscalls whose buffers lie in the far bank: a console write
 *     of the far-area literal and an xmem write of the same bytes (the
 *     buffer-identity rule, 4.9);
 *   * it execs /system/xmem_child.exe from far context, with both the
 *     path and the command tail in the far area - the child starts with
 *     P = 0 and this process's latch comes back exactly;
 *   * it returns (latch << 8) | child status, so the primary half proves
 *     which bank ran the code and that the plain CALL returned.
 *
 * `xmem_far_exit` is the exit path: SYS_EXIT from far context, with the
 * aperture open - the process's segments, objects and slots are released
 * at exit (the launcher's report and the parent's capacity check).
 */

#include "kos.h"

#define CHILD_PATH "/system/xmem_child.exe"

/* The banking latch (port 00h): mode in D4-D3, bank in D2-D0, page in
 * D7-D5.  Read directly. */
__sfr __at 0x00 latch_port;

/* The primary half: a function and a static, both outside the aperture. */
extern int xmem_far_primary(int value);
extern unsigned int xmem_far_marker;

/* This literal is part of the _FAR_CODE area, so while the aperture is
 * open it lives in the far bank: a syscall buffer naming it is resolved
 * against the far bank, not the primary image. */
static const char far_text[] = "xmem-far: far code says hello\r\n";

int xmem_far_entry(int handle, int arg)
{
	unsigned char latch = latch_port;
	unsigned int text = (unsigned int)(sizeof(far_text) - 1u);
	int status;

	/* Data outside the aperture: the primary static. */
	xmem_far_marker = xmem_far_marker + 1u;

	/* Primary code, called directly from the far bank. */
	if (xmem_far_primary(arg) != arg + 1) {
		return -1;
	}
	/* A driver call whose buffer is the far bank. */
	if (console_write(far_text, text) != (int)text) {
		return -1;
	}
	/* A syscall whose buffer is the far bank. */
	if (xwrite((unsigned int)handle, 0u, far_text, text) !=
	    (int)text) {
		return -1;
	}
	/* Exec from far context: the child starts with P = 0, and this
	 * process's latch is restored exactly when it exits. */
	status = run(CHILD_PATH, (const char *)arg);
	return (int)(((unsigned int)latch << 8) |
		     (unsigned int)(status & 0xFF));
}

int xmem_far_exit(int status, int unused)
{
	(void)unused;
	exit((unsigned int)status); /* does not return */
	return -1;
}

/* The banked entries.  Each is `__banked`, so this module -
 * compiled with bank 1 - emits `b_xmem_bk_* = 1` and the caller's plain C
 * call goes through ___sdcc_bcall_ehl, which maps segment 1 and restores
 * the caller's latch.  Together they pin the calling convention: 0, 1, 2
 * and 3 arguments of 8, 16 and 32 bits, all on the stack, with the result
 * in A (8 bits), DE (16) or DEHL (32). */

unsigned char xmem_bk_c(void) __banked
{
	return 0xC5u;
}

unsigned int xmem_bk_c16(void) __banked
{
	return 0x1234u;
}

unsigned long xmem_bk_c32(void) __banked
{
	return 0x12345678ul;
}

unsigned char xmem_bk_8(unsigned char a) __banked
{
	return (unsigned char)(a + 1u);
}

unsigned char xmem_bk_88(unsigned char a, unsigned char b) __banked
{
	return (unsigned char)(a + b + 1u);
}

unsigned char xmem_bk_888(unsigned char a, unsigned char b,
			 unsigned char c) __banked
{
	return (unsigned char)(a + b + c);
}

unsigned int xmem_bk_16(unsigned int a) __banked
{
	return (unsigned int)(a + 2u);
}

unsigned int xmem_bk_1616(unsigned int a, unsigned int b) __banked
{
	return (unsigned int)(a + a + a + b);
}

unsigned int xmem_bk_161616(unsigned int a, unsigned int b,
			    unsigned int c) __banked
{
	return (unsigned int)(a + b + c);
}

unsigned long xmem_bk_32(unsigned long a) __banked
{
	return a + 3ul;
}

unsigned long xmem_bk_3232(unsigned long a, unsigned long b) __banked
{
	return a + b;
}

unsigned long xmem_bk_323232(unsigned long a, unsigned long b,
			     unsigned long c) __banked
{
	return a + b + c;
}

/* A mixed call: 7 bytes of arguments, more than the three register pairs,
 * so the whole list is on the stack and the result comes back in DEHL. */
unsigned long xmem_bk_mix(unsigned char a, unsigned int b,
			  unsigned long c) __banked
{
	return c + (unsigned long)b + (unsigned long)a;
}

/* A banked call from far code: the trampoline runs with the aperture
 * already open onto segment 1, so every level maps and restores the same
 * segment (the cached token, no syscall). */
unsigned int xmem_bk_rec(unsigned int n) __banked
{
	if (n == 0u) {
		return 0u;
	}
	return (unsigned int)(n + xmem_bk_rec(n - 1u));
}
