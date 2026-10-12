/* ZephyrOS ZEX v2 DATA-segment outcome program (package 44a, test only).
 *
 * Run by the subsystem process selftest after it has dirtied the
 * auxiliary bank's BSS area.  The program proves the container's segment
 * wiring from inside the child:
 *
 *   - SYS_SEG_INFO(1) reports the declared logical size, the DATA kind
 *     and the LOADED state, and hands out the extended-memory handle;
 *   - the payload arrived intact (the generator's byte pattern);
 *   - the segment BSS was zeroed by the loader (the kernel test wrote
 *     0x5A over it first);
 *   - SYS_XMEM_WRITE reaches the object and reads back;
 *   - SYS_XMEM_FREE refuses the loader-created segment;
 *   - an id outside the container and a stale report are refused;
 *   - its own primary BSS is zero and its bank was not cleared.
 *
 * The exit status (PASS | bank) is what the selftest asserts.
 */

#include "zos.h"
#include "process_selftest.h"

/* Uninitialized: the loader must zero this whole region. */
static unsigned char bss_probe[32];

/* Initialized constant: must arrive from the packed image intact. */
static const unsigned char ro_sentinel[4] = {0xDE, 0xAD, 0xBE, 0xEF};

static const char ok_line[] = "zex2seg: OK\r\n";

/* The generator's pattern, pinned in process_selftest.h. */
static unsigned char expect(unsigned int index)
{
	return (unsigned char)((index * ZOS_PCST_SEG_MUL +
				ZOS_PCST_SEG_ADD) & 0xFFu);
}

static void z2_fail(unsigned char code)
{
	exit(ZOS_PCST_ZEX2_FAIL | code);
}

int main(void)
{
	const unsigned char *mark =
		(const unsigned char *)ZOS_PCST_ZEX2_MARK_ADDR;
	struct zos_seg_info info;
	struct zos_info_record pinfo;
	unsigned char buf[4];
	unsigned char i;

	if (getinfo(&pinfo) != ZOS_GETINFO_SIZE) {
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

	/* The container declares exactly one DATA segment. */
	if (seginfo(1u, &info) != 0) {
		z2_fail(ZOS_PCST_Z2_FAIL_SEGINFO);
	}
	if (info.size != (unsigned int)(ZOS_PCST_SEG_IMAGE + ZOS_PCST_SEG_BSS) ||
	    info.kind != ZOS_SEG_KIND_DATA ||
	    info.state != ZOS_SEG_STATE_LOADED || info.handle == 0u) {
		z2_fail(ZOS_PCST_Z2_FAIL_SEGINFO);
	}

	/* Payload integrity: the first and last four image bytes. */
	if (xread(info.handle, 0u, buf, 4u) != 4) {
		z2_fail(ZOS_PCST_Z2_FAIL_SEGDATA);
	}
	for (i = 0; i < 4u; ++i) {
		if (buf[i] != expect(i)) {
			z2_fail(ZOS_PCST_Z2_FAIL_SEGDATA);
		}
	}
	if (xread(info.handle,
			  (unsigned int)(ZOS_PCST_SEG_IMAGE - 4u), buf, 4u) != 4) {
		z2_fail(ZOS_PCST_Z2_FAIL_SEGDATA);
	}
	for (i = 0; i < 4u; ++i) {
		if (buf[i] != expect(ZOS_PCST_SEG_IMAGE - 4u + i)) {
			z2_fail(ZOS_PCST_Z2_FAIL_SEGDATA);
		}
	}

	/* The BSS the kernel test dirtied must read back zero. */
	if (xread(info.handle, ZOS_PCST_SEG_IMAGE, buf, 4u) != 4) {
		z2_fail(ZOS_PCST_Z2_FAIL_SEGBSS);
	}
	for (i = 0; i < 4u; ++i) {
		if (buf[i] != 0u) {
			z2_fail(ZOS_PCST_Z2_FAIL_SEGBSS);
		}
	}

	/* Write-back through the handle, then read it back. */
	buf[0] = 0x11u;
	buf[1] = 0x22u;
	buf[2] = 0x33u;
	buf[3] = 0x44u;
	if (xwrite(info.handle, 0u, buf, 4u) != 4 ||
	    xread(info.handle, 0u, buf, 4u) != 4) {
		z2_fail(ZOS_PCST_Z2_FAIL_WRITE);
	}
	if (buf[0] != 0x11u || buf[1] != 0x22u ||
	    buf[2] != 0x33u || buf[3] != 0x44u) {
		z2_fail(ZOS_PCST_Z2_FAIL_WRITE);
	}

	/* A loader-created segment is released only at exit. */
	if (xfree(info.handle) >= 0) {
		z2_fail(ZOS_PCST_Z2_FAIL_FREE);
	}
	/* No second segment in this container. */
	if (seginfo(2u, &info) >= 0) {
		z2_fail(ZOS_PCST_Z2_FAIL_SEGINFO);
	}

	(void)console_write(ok_line, (unsigned int)(sizeof(ok_line) - 1u));
	exit(ZOS_PCST_ZEX2_PASS | pinfo.bank);
	return 0; /* unreachable */
}
