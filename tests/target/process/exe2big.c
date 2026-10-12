/* KonaOS EXE v2 large-container outcome program (package 44a, test only).
 *
 * A v2 container may exceed 64 KiB (32-bit file geometry).  This program
 * carries two 32-KiB DATA segments; its container is over 64 KiB, and the
 * last bytes of the second segment sit past the 64-KiB file offset, so
 * reading them exercises the loader's 32-bit seek and stream.  The
 * program checks the first, the slot-boundary and the last bytes of each
 * segment plus the zeroed BSS, then exits with PASS | bank.
 */

#include "kos.h"
#include "process_selftest.h"

/* Uninitialized: the loader must zero this whole region. */
static unsigned char bss_probe[32];

static const char ok_line[] = "exe2big: OK\r\n";

static unsigned char expect1(unsigned int index)
{
	return (unsigned char)((index * KOS_PCST_BIG1_MUL +
				KOS_PCST_BIG1_ADD) & 0xFFu);
}

static unsigned char expect2(unsigned int index)
{
	return (unsigned char)((index * KOS_PCST_BIG2_MUL +
				KOS_PCST_BIG2_ADD) & 0xFFu);
}

static void z2_fail(unsigned char code)
{
	exit(KOS_PCST_EXE2_FAIL | code);
}

/* Check one byte of a segment through the extended-memory handle. */
static void check_byte(unsigned int handle, unsigned int offset,
		       unsigned char wanted)
{
	unsigned char got;

	if (xread(handle, offset, &got, 1u) != 1) {
		z2_fail(KOS_PCST_Z2_FAIL_SEGDATA);
	}
	if (got != wanted) {
		z2_fail(KOS_PCST_Z2_FAIL_SEGDATA);
	}
}

int main(void)
{
	struct kos_seg_info info1;
	struct kos_seg_info info2;
	struct kos_info_record pinfo;
	unsigned char i;

	if (getinfo(&pinfo) != KOS_GETINFO_SIZE) {
		z2_fail(KOS_PCST_Z2_FAIL_GETINFO);
	}
	for (i = 0; i < (unsigned char)sizeof(bss_probe); ++i) {
		if (bss_probe[i] != 0) {
			z2_fail(KOS_PCST_Z2_FAIL_BSS);
		}
	}
	if (seginfo(1u, &info1) != 0 || seginfo(2u, &info2) != 0) {
		z2_fail(KOS_PCST_Z2_FAIL_SEGINFO);
	}
	if (info1.size != (unsigned int)(KOS_PCST_BIG1_IMAGE + KOS_PCST_BIG1_BSS) ||
	    info2.size != (unsigned int)(KOS_PCST_BIG2_IMAGE + KOS_PCST_BIG2_BSS) ||
	    info1.kind != KOS_SEG_KIND_DATA ||
	    info2.kind != KOS_SEG_KIND_DATA ||
	    info1.handle == 0u || info2.handle == 0u) {
		z2_fail(KOS_PCST_Z2_FAIL_SEGINFO);
	}

	/* Segment 1: first, the 8-KiB slot boundary and the last byte. */
	check_byte(info1.handle, 0u, expect1(0u));
	check_byte(info1.handle, 8191u, expect1(8191u));
	check_byte(info1.handle, 8192u, expect1(8192u));
	check_byte(info1.handle, KOS_PCST_BIG1_IMAGE - 1u,
		   expect1(KOS_PCST_BIG1_IMAGE - 1u));

	/* Segment 2: its last byte lies past the 64-KiB file offset of the
	 * container, so this read proves the 32-bit geometry. */
	check_byte(info2.handle, 0u, expect2(0u));
	check_byte(info2.handle, 16383u, expect2(16383u));
	check_byte(info2.handle, 16384u, expect2(16384u));
	check_byte(info2.handle, KOS_PCST_BIG2_IMAGE - 1u,
		   expect2(KOS_PCST_BIG2_IMAGE - 1u));

	/* Zeroed BSS tails. */
	for (i = 0; i < KOS_PCST_BIG1_BSS; ++i) {
		check_byte(info1.handle, KOS_PCST_BIG1_IMAGE + i, 0u);
	}
	for (i = 0; i < KOS_PCST_BIG2_BSS; ++i) {
		check_byte(info2.handle, KOS_PCST_BIG2_IMAGE + i, 0u);
	}

	(void)console_write(ok_line, (unsigned int)(sizeof(ok_line) - 1u));
	exit(KOS_PCST_EXE2_PASS | pinfo.bank);
	return 0; /* unreachable */
}
