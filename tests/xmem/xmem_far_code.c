/* KonaOS far-code outcome program (work package 44b): the far half.
 *
 * Built with `--codeseg FAR_CODE` through the SDK's FAR= option and
 * linked at 8000h, so this module - its code and its string literals -
 * becomes the `_FAR_CODE` area the container loads into the aperture
 * slice of an auxiliary bank.  The primary half maps a segment and calls
 * this entry with `farcall`, which is an OUT (the aperture token)
 * followed by a plain CALL to the entry's linked address.
 *
 * The entry proves the far-code rules of architecture 36.17 from inside:
 *
 *   * it reads its own banking latch - the aperture is open, so P is the
 *     segment's page (7 - bank);
 *   * it touches primary data (`xmem_far_marker`) - data used while the
 *     aperture is open lives outside 8000h-BFFFh of the primary image;
 *   * it calls primary code directly (the linker resolves the call);
 *   * it runs syscalls whose buffers lie in the far bank: a console write
 *     of the far-area literal and an xmem write of the same bytes (the
 *     buffer-identity rule, 36.8);
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
