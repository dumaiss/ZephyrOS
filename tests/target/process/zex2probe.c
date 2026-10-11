/* ZephyrOS ZEX v2 loader-outcome probe (package 44a, test only).
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
 *   - the startup block is present (GETINFO reports the identity).
 *
 * The console lines are for hardware transcripts; the exit status
 * (PASS | bank) is what the selftest asserts.
 */

#include "zos.h"
#include "process_selftest.h"

/* Uninitialized: the loader must zero this whole region. */
static unsigned char bss_probe[64];

/* Initialized constant: must arrive from the packed image intact. */
static const unsigned char ro_sentinel[4] = {0xDE, 0xAD, 0xBE, 0xEF};

static const char ok_line[] = "zex2probe: OK\r\n";

static void z2_fail(unsigned char code)
{
	zex_exit(ZOS_PCST_ZEX2_FAIL | code);
}

int main(void)
{
	const unsigned char *mark =
		(const unsigned char *)ZOS_PCST_ZEX2_MARK_ADDR;
	struct zos_info_record info;
	unsigned char i;

	if (zex_getinfo(&info) != ZOS_GETINFO_SIZE) {
		z2_fail(ZOS_PCST_Z2_FAIL_GETINFO);
	}
	for (i = 0; i < (unsigned char)sizeof(bss_probe); ++i) {
		if (bss_probe[i] != 0) {
			z2_fail(ZOS_PCST_Z2_FAIL_BSS);
		}
	}
	if (ro_sentinel[0] != 0xDEu || ro_sentinel[1] != 0xADu ||
	    ro_sentinel[2] != 0xBEu || ro_sentinel[3] != 0xEFu) {
		z2_fail(ZOS_PCST_Z2_FAIL_IMAGE);
	}
	if (mark[0] != ZOS_PCST_ZEX2_MARK_0 || mark[1] != ZOS_PCST_ZEX2_MARK_1 ||
	    mark[2] != ZOS_PCST_ZEX2_MARK_2 ||
	    mark[3] != ZOS_PCST_ZEX2_MARK_3) {
		z2_fail(ZOS_PCST_Z2_FAIL_MARK);
	}
	(void)zex_write(ok_line, (unsigned int)(sizeof(ok_line) - 1u));
	zex_exit(ZOS_PCST_ZEX2_PASS | info.bank);
	return 0; /* unreachable */
}
