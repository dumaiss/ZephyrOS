/* ZephyrOS far-code outcome program (work package 44b): the primary half.
 *
 * The third program of the xmem-selftest image: `xmem_selftest.zex` execs
 * it, it drives the far-code path and it SYS_EXECs `xmem_child.zex` from
 * inside its far code.  Its container declares two FAR_CODE segments, both
 * the linked `_FAR_CODE` area (the SDK links one far area per program), so
 * the image exercises the loader's one-far-segment-per-auxiliary-bank
 * placement: the two segments land in two banks, `SYS_SEG_MAP` returns a
 * distinct token for each, and the far code returns its own latch so the
 * two calls prove which bank each ran.  The 36.17 link rule holds for
 * this program: its image, BSS and whole stack lie outside 8000h-BFFFh
 * (the packer checks it).
 *
 * Outcomes (asserted by tests/test_xmem_selftest_sim.py):
 *
 *   * `seg-info-*`: both segments report FAR_CODE, LOADED, size, handle 0;
 *   * `seg-map-*`: a token per own FAR_CODE segment, the caller's own bank
 *     field, P of 2..7 (never bank 6 or bank 7), distinct tokens for the
 *     two banks, and a refusal for an id the container does not have;
 *   * `far-call-*`, `far-latch-*`: map, plain CALL, return - the far
 *     entry returns (latch << 8) | child status, so the exact token
 *     proves the bank, and the child status proves the exec;
 *   * `far-marker`: far code touched primary data and called primary code;
 *   * `far-buffer`: the far code's syscall whose buffer was in the
 *     aperture wrote the far-area literal into the object (identity);
 *   * `far-exec-*`: P = 0 in the child (its own words), the parent's
 *     latch restored, and the child refuses the parent's live object
 *     handle and far-segment id (a child inherits neither);
 *   * `far-mappable-same-bank`: the free aperture slot of a far segment's
 *     bank holds a mappable object - same token, address A000h;
 *   * exit from far context: the last act is a far call whose entry runs
 *     SYS_EXIT with the aperture open; the parent's capacity check and
 *     the launcher's report prove the release.
 */

#include "zos.h"

#define FAR_STATUS   43
#define CHILD_STATUS 42
#define APERTURE_BASE 0x8000u

/* The banking latch (port 00h): mode in D4-D3, bank in D2-D0, page in
 * D7-D5.  Read directly - no SDK call touches it. */
__sfr __at 0x00 latch_port;

/* The far half (tests/xmem/xmem_far_code.c, --codeseg FAR_CODE). */
extern int xmem_far_entry(int handle, int arg);
extern int xmem_far_exit(int status, int unused);

/* Called from the far code (address < 8000h, so it stays mapped while the
 * aperture is open) and from here. */
int xmem_far_primary(int value);
unsigned int xmem_far_marker;

static char tail[8];
static unsigned char buf[64];

static unsigned int checks;
static unsigned int failures;

static const char far_text[] = "xmem-far: far code says hello\r\n";

static void check(const char *label, int ok)
{
	++checks;
	if (!ok) {
		++failures;
		zos_printf("xmem-far: FAIL %s\r\n", label);
		return;
	}
	zos_printf("xmem-far: %s OK\r\n", label);
}

int xmem_far_primary(int value)
{
	xmem_far_marker = xmem_far_marker + 1u;
	return value + 1;
}

/* The child's command tail: "<handle> <segment>" - the live object handle
 * and the far-segment id of the process that exec'd it. */
static void make_tail(unsigned int handle)
{
	char digits[6];
	unsigned int count = 0u;
	unsigned int i = 0u;

	do {
		digits[count++] = (char)('0' + handle % 10u);
		handle /= 10u;
	} while (handle != 0u);
	while (count != 0u) {
		tail[i++] = digits[--count];
	}
	tail[i++] = ' ';
	tail[i++] = '1';
	tail[i] = '\0';
}

int main(void)
{
	struct zos_seg_info info1;
	struct zos_seg_info info2;
	struct zos_seg_info info;
	unsigned int t1 = 0u;
	unsigned int t2 = 0u;
	unsigned int tm = 0u;
	unsigned int address = 0u;
	unsigned int handle;
	unsigned int mappable;
	unsigned int expected;
	int result;

	zos_puts("xmem-far: begin\r\n");

	/* SYS_SEG_INFO names the container's own segments: kind FAR_CODE,
	 * LOADED, no handle, the logical size the loader placed. */
	info1.size = 0u;
	info1.kind = 0u;
	info1.state = 0u;
	info1.handle = 0xFFFFu;
	info2.size = 0u;
	info2.kind = 0u;
	info2.state = 0u;
	info2.handle = 0xFFFFu;
	check("seg-info-1",
	      zos_seg_info(1u, &info1) == 0 &&
	      info1.kind == ZOS_SEG_KIND_FAR_CODE &&
	      info1.state == ZOS_SEG_STATE_LOADED &&
	      info1.handle == 0u && info1.size > 0u);
	check("seg-info-2",
	      zos_seg_info(2u, &info2) == 0 &&
	      info2.kind == ZOS_SEG_KIND_FAR_CODE &&
	      info2.state == ZOS_SEG_STATE_LOADED &&
	      info2.handle == 0u && info2.size == info1.size);
	info.size = 0u;
	check("seg-info-unknown", zos_seg_info(3u, &info) == -ZOS_EINVAL);

	/* SYS_SEG_MAP: a token for each of this process's FAR_CODE
	 * segments.  The token keeps the caller's bank field and P =
	 * 7 - bank with an application bank (0-5), so its page field is
	 * 2..7 and it never names bank 6 or bank 7. */
	result = zos_seg_map(1u);
	t1 = (unsigned int)result;
	check("seg-map-token", result >= 1);
	check("seg-map-page",
	      ((t1 >> 5) & 7u) >= 2u &&
	      (t1 & 7u) == ((unsigned int)latch_port & 7u));
	result = zos_seg_map(2u);
	t2 = (unsigned int)result;
	check("seg-map-several", result >= 1 && t2 != t1);
	check("seg-map-refused",
	      zos_seg_map(0u) == -ZOS_EINVAL &&
	      zos_seg_map(3u) == -ZOS_EINVAL);

	/* The far call: map, a plain CALL and return.  The far code moves
	 * primary data and calls primary code; the child it execs checks
	 * this process's handle and segment id (both foreign to it). */
	handle = (unsigned int)zos_xmem_alloc(64u, ZOS_XMEM_FLAG_NONE);
	check("far-object", (int)handle >= 1);
	if ((int)handle >= 1) {
		xmem_far_marker = 0u;
		make_tail(handle);
		result = zos_far_call(1u, xmem_far_entry, (int)handle,
				      (int)tail);
		check("far-call-1",
		      result == (int)((t1 << 8) |
				      (unsigned int)CHILD_STATUS));
		check("far-latch-1", ((unsigned int)result >> 8) == t1);
		check("far-exec-status",
		      (result & 0xFF) == CHILD_STATUS);
		result = zos_far_call(2u, xmem_far_entry, (int)handle,
				      (int)tail);
		check("far-call-2",
		      result == (int)((t2 << 8) |
				      (unsigned int)CHILD_STATUS));
		check("far-latch-2", ((unsigned int)result >> 8) == t2);
		check("far-banks-differ", t2 != t1);
		check("far-aperture-closed",
		      ((unsigned int)latch_port & 0xE0u) == 0u);
		/* The far entry bumped the primary static once and the
		 * primary code it called bumped it again, in each call. */
		check("far-marker", xmem_far_marker == 4u);
		/* The far code wrote its far-bank literal into the object
		 * by handle: an aperture buffer resolved to the far bank. */
		expected = (unsigned int)(sizeof(far_text) - 1u);
		check("far-buffer",
		      zos_xmem_read(handle, 0u, buf, expected) ==
			      (int)expected &&
		      buf[0] == (unsigned char)'x' &&
		      buf[expected - 2u] == (unsigned char)'\r' &&
		      buf[expected - 1u] == (unsigned char)'\n');
	}

	/* The free aperture slot of a far segment's bank may hold
	 * mappable data: the newly mappable object packs into that bank,
	 * so it shares the segment's token (same page) and appears at
	 * A000h in the aperture. */
	mappable = (unsigned int)zos_xmem_alloc(8192u,
						ZOS_XMEM_FLAG_MAPPABLE);
	check("far-mappable-alloc", (int)mappable >= 1);
	if ((int)mappable >= 1) {
		result = zos_xmem_map(mappable, &address);
		tm = (unsigned int)result;
		check("far-mappable-same-bank",
		      result >= 1 && tm == t1 &&
		      address == APERTURE_BASE + 0x2000u);
		(void)zos_xmem_free(mappable);
	}
	if ((int)handle >= 1) {
		(void)zos_xmem_free(handle);
	}

	zos_printf("xmem-far: %u checks, %u failures\r\n", checks, failures);
	if (failures == 0u) {
		zos_puts("xmem-far: PASS\r\n");
	} else {
		zos_puts("xmem-far: FAIL\r\n");
	}

	/* Exit from far context: the far entry runs SYS_EXIT with the
	 * aperture open onto segment 1.  It does not return; the
	 * launcher's report and the parent's capacity check prove that
	 * exit released the segments, the objects and the bank slots. */
	(void)zos_far_call(1u, xmem_far_exit,
			   (failures == 0u) ? (int)FAR_STATUS : 1, 0);
	return 1; /* not reached */
}
