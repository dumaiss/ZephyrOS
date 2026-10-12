/* KonaOS EXE v2 loader-outcome probe (package 44a, test only).
 *
 * Run by the subsystem process selftest after it has written a marker
 * into this program's bank outside its image, BSS and stack, and after it
 * has dirtied the whole BSS range.  The program proves the 36.19 rule
 * from inside the child:
 *
 *   - the loader zeroed its BSS (the kernel test wrote 0x5A over the
 *     whole range first, so a surviving zero can only come from the
 *     loader's BSS clear);
 *   - the image arrived intact (a constant sentinel);
 *   - the bank was NOT cleared (the marker outside image/BSS/stack is
 *     still there);
 *   - an ordinary (zero-segment) program runs with its full bank: the
 *     latch is mode 10 with P = 0, so nothing in 0000h-DFFFh is an
 *     aperture (36.17);
 *   - the startup block is present (GETINFO reports the identity).
 *
 * The console lines are for hardware transcripts; the exit status
 * (PASS | bank) is what the selftest asserts.
 */

#include "kos.h"
#include "process_selftest.h"

/* The banking latch (port 00h): mode in D4-D3, bank in D2-D0, page in
 * D7-D5 (platform/zephyr80/public/hardware.inc). */
__sfr __at 0x00 latch_port;

/* Uninitialized: the loader must zero this whole region. */
static unsigned char bss_probe[64];

/* Initialized constant: must arrive from the packed image intact. */
static const unsigned char ro_sentinel[4] = {0xDE, 0xAD, 0xBE, 0xEF};

static const char ok_line[] = "exe2probe: OK\r\n";

static void z2_fail(unsigned char code)
{
	exit(KOS_PCST_EXE2_FAIL | code);
}

int main(void)
{
	const unsigned char *mark =
		(const unsigned char *)KOS_PCST_EXE2_MARK_ADDR;
	struct kos_info_record info;
	unsigned char i;

	if (getinfo(&info) != KOS_GETINFO_SIZE) {
		z2_fail(KOS_PCST_Z2_FAIL_GETINFO);
	}
	for (i = 0; i < (unsigned char)sizeof(bss_probe); ++i) {
		if (bss_probe[i] != 0) {
			z2_fail(KOS_PCST_Z2_FAIL_BSS);
		}
	}
	if (ro_sentinel[0] != 0xDEu || ro_sentinel[1] != 0xADu ||
	    ro_sentinel[2] != 0xBEu || ro_sentinel[3] != 0xEFu) {
		z2_fail(KOS_PCST_Z2_FAIL_IMAGE);
	}
	/* An ordinary EXE v2 program runs in the application mode over its
	 * own bank with P = 0: every address in the bank is primary. */
	if ((latch_port & 0x18u) != 0x10u || (latch_port & 0xE0u) != 0u) {
		z2_fail(KOS_PCST_Z2_FAIL_LATCH);
	}
	if (mark[0] != KOS_PCST_EXE2_MARK_0 || mark[1] != KOS_PCST_EXE2_MARK_1 ||
	    mark[2] != KOS_PCST_EXE2_MARK_2 ||
	    mark[3] != KOS_PCST_EXE2_MARK_3) {
		z2_fail(KOS_PCST_Z2_FAIL_MARK);
	}
	(void)console_write(ok_line, (unsigned int)(sizeof(ok_line) - 1u));
	exit(KOS_PCST_EXE2_PASS | info.bank);
	return 0; /* unreachable */
}
