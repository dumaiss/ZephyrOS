/* KonaOS far-code outcome program: the primary half.
 *
 * The third program of the xmem-selftest image: `xmem_selftest.exe` execs
 * it, it drives the far-code path and it SYS_EXECs `xmem_child.exe` from
 * inside its far code.  Its container declares two FAR_CODE segments, the
 * linked `_FAR1` and `_FAR2` areas (`FAR1=`/`FAR2=`), so the image
 * exercises the loader's one-far-segment-per-auxiliary-bank placement:
 * the two segments land in two banks, `SYS_SEG_MAP` returns a distinct
 * token for each, and the far code returns its own latch so the calls
 * prove which bank each ran.  The 7.2 link rule holds for this program:
 * its image, BSS and whole stack lie outside 8000h-BFFFh (the packer
 * checks it).
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
 *   * `bk-*`: the far module's `__banked` entries called as
 *     plain C - 0-3 arguments of 8, 16 and 32 bits, a mixed call and a
 *     recursion inside segment 1 - through the SDK's
 *     `___sdcc_bcall_ehl` trampoline, which maps the segment the
 *     definition declared and restores the caller's latch;
 *   * `bk-2-*`: segment 2's banked entry calls back into segment 1, so
 *     the inner call has to restore *its* caller's bank;
 *   * `bk-invalid-segment`: a `__banked` definition in the primary bank
 *     (`b_` = 0) names no far segment, so the call reports
 *     KOS_RT_EFARMAP instead of running it;
 *   * exit from far context: the last act is a far call whose entry runs
 *     SYS_EXIT with the aperture open; the parent's capacity check and
 *     the launcher's report prove the release.
 */

#include "kos.h"

#define FAR_STATUS   43
#define CHILD_STATUS 42
#define APERTURE_BASE 0x8000u

/* The banked-call arguments and constants: wide enough to
 * fill the stack slots the far module's `__banked` entries read. */
#define BK_8_A   0x11u
#define BK_8_B   0x22u
#define BK_16_A  0x0102u
#define BK_16_B  0x0304u
#define BK_32_A  0x01020304ul
#define BK_32_B  0x11121314ul

/* The banking latch (port 00h): mode in D4-D3, bank in D2-D0, page in
 * D7-D5.  Read directly - no SDK call touches it. */
__sfr __at 0x00 latch_port;

/* The far halves: tests/xmem/xmem_far_code.c (linked at 8000h as segment
 * 1) and tests/xmem/xmem_far_code2.c (segment 2).  The plain entries are
 * the `farcall` arm - the primitive stays covered here.  The `xmem_bk_*`
 * and `xmem_far_entry2` entries are the `__banked` arm:
 * `__banked` ones are called as plain C through the trampoline, the
 * others through `farcall` like segment 1's entry. */
extern int xmem_far_entry(int handle, int arg);
extern int xmem_far_exit(int status, int unused);
extern int xmem_far_entry2(int seed, int unused);
extern unsigned char xmem_bk_c(void) __banked;
extern unsigned char xmem_bk_8(unsigned char a) __banked;
extern unsigned char xmem_bk_88(unsigned char a, unsigned char b) __banked;
extern unsigned char xmem_bk_888(unsigned char a, unsigned char b,
				 unsigned char c) __banked;
extern unsigned int xmem_bk_c16(void) __banked;
extern unsigned int xmem_bk_16(unsigned int a) __banked;
extern unsigned int xmem_bk_1616(unsigned int a, unsigned int b) __banked;
extern unsigned int xmem_bk_161616(unsigned int a, unsigned int b,
				   unsigned int c) __banked;
extern unsigned long xmem_bk_c32(void) __banked;
extern unsigned long xmem_bk_32(unsigned long a) __banked;
extern unsigned long xmem_bk_3232(unsigned long a, unsigned long b) __banked;
extern unsigned long xmem_bk_323232(unsigned long a, unsigned long b,
				    unsigned long c) __banked;
extern unsigned long xmem_bk_mix(unsigned char a, unsigned int b,
				 unsigned long c) __banked;
extern unsigned int xmem_bk_rec(unsigned int n) __banked;
extern unsigned int xmem_bk2(unsigned int seed) __banked;

/* A `__banked` definition in the primary bank: its module emits `b_` = 0,
 * which is not a far segment id, so the trampoline reports
 * KOS_RT_EFARMAP instead of calling it (the caller reads the -code). */
unsigned int xmem_bk_bank0(unsigned int a) __banked;
unsigned int xmem_bk_bank0(unsigned int a) __banked
{
	return a;
}

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
		printf("xmem-far: FAIL %s\r\n", label);
		return;
	}
	printf("xmem-far: %s OK\r\n", label);
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
	struct kos_seg_info info1;
	struct kos_seg_info info2;
	struct kos_seg_info info;
	unsigned int t1 = 0u;
	unsigned int t2 = 0u;
	unsigned int tm = 0u;
	unsigned int address = 0u;
	unsigned int handle;
	unsigned int mappable;
	unsigned int expected;
	unsigned char value8;
	unsigned int value16;
	unsigned long value32;
	int result;

	console_puts("xmem-far: begin\r\n");

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
	      seginfo(1u, &info1) == 0 &&
	      info1.kind == KOS_SEG_KIND_FAR_CODE &&
	      info1.state == KOS_SEG_STATE_LOADED &&
	      info1.handle == 0u && info1.size > 0u);
	check("seg-info-2",
	      seginfo(2u, &info2) == 0 &&
	      info2.kind == KOS_SEG_KIND_FAR_CODE &&
	      info2.state == KOS_SEG_STATE_LOADED &&
	      info2.handle == 0u && info2.size > 0u);
	info.size = 0u;
	check("seg-info-unknown", seginfo(3u, &info) == -KOS_EINVAL);

	/* SYS_SEG_MAP: a token for each of this process's FAR_CODE
	 * segments.  The token keeps the caller's bank field and P =
	 * 7 - bank with an application bank (0-5), so its page field is
	 * 2..7 and it never names bank 6 or bank 7. */
	result = segmap(1u);
	t1 = (unsigned int)result;
	check("seg-map-token", result >= 1);
	check("seg-map-page",
	      ((t1 >> 5) & 7u) >= 2u &&
	      (t1 & 7u) == ((unsigned int)latch_port & 7u));
	result = segmap(2u);
	t2 = (unsigned int)result;
	check("seg-map-several", result >= 1 && t2 != t1);
	check("seg-map-refused",
	      segmap(0u) == -KOS_EINVAL &&
	      segmap(3u) == -KOS_EINVAL);

	/* The far call: map, a plain CALL and return.  The far code moves
	 * primary data and calls primary code; the child it execs checks
	 * this process's handle and segment id (both foreign to it). */
	handle = (unsigned int)xalloc(64u, KOS_XMEM_FLAG_NONE);
	check("far-object", (int)handle >= 1);
	if ((int)handle >= 1) {
		xmem_far_marker = 0u;
		make_tail(handle);
		result = farcall(1u, xmem_far_entry, (int)handle,
				      (int)tail);
		check("far-call-1",
		      result == (int)((t1 << 8) |
				      (unsigned int)CHILD_STATUS));
		check("far-latch-1", ((unsigned int)result >> 8) == t1);
		check("far-exec-status",
		      (result & 0xFF) == CHILD_STATUS);
		result = farcall(2u, xmem_far_entry2, (int)handle, 0);
		check("far-call-2",
		      result == (int)((t2 << 8) |
				      ((unsigned int)(handle + 1u) &
				       0xFFu)));
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
		      xread(handle, 0u, buf, expected) ==
			      (int)expected &&
		      buf[0] == (unsigned char)'x' &&
		      buf[expected - 2u] == (unsigned char)'\r' &&
		      buf[expected - 1u] == (unsigned char)'\n');
	}

	/* The free aperture slot of a far segment's bank may hold
	 * mappable data: the newly mappable object packs into that bank,
	 * so it shares the segment's token (same page) and appears at
	 * A000h in the aperture. */
	mappable = (unsigned int)xalloc(8192u,
						KOS_XMEM_FLAG_MAPPABLE);
	check("far-mappable-alloc", (int)mappable >= 1);
	if ((int)mappable >= 1) {
		result = xmap(mappable, &address);
		tm = (unsigned int)result;
		check("far-mappable-same-bank",
		      result >= 1 && (tm == t1 || tm == t2) &&
		      address == APERTURE_BASE + 0x2000u);
		(void)xfree(mappable);
	}
	if ((int)handle >= 1) {
		(void)xfree(handle);
	}

	/* Banked calls: plain C on the far module's
	 * `__banked` entries, 0-3 arguments of 8, 16 and 32 bits, a mixed
	 * call and a recursion.  Every one goes through the SDK's
	 * ___sdcc_bcall_ehl trampoline, which maps the segment the
	 * definition declared - segment 1 here, from its cached token -
	 * and restores this caller's latch; the recursion re-enters the
	 * trampoline with the aperture already open onto the same
	 * segment. */
	value8 = xmem_bk_c();
	check("bk-0-8", value8 == 0xC5u);
	value16 = xmem_bk_c16();
	check("bk-0-16", value16 == 0x1234u);
	value32 = xmem_bk_c32();
	check("bk-0-32", value32 == 0x12345678ul);
	value8 = xmem_bk_8(BK_8_A);
	check("bk-1-8", value8 == (unsigned char)(BK_8_A + 1u));
	value16 = xmem_bk_16(BK_16_A);
	check("bk-1-16", value16 == (unsigned int)(BK_16_A + 2u));
	value32 = xmem_bk_32(BK_32_A);
	check("bk-1-32", value32 == BK_32_A + 3ul);
	value8 = xmem_bk_88(BK_8_A, BK_8_B);
	check("bk-2-8", value8 == (unsigned char)(BK_8_A + BK_8_B + 1u));
	value16 = xmem_bk_1616(BK_16_A, BK_16_B);
	check("bk-2-16", value16 == (unsigned int)(BK_16_A + BK_16_A +
						   BK_16_A + BK_16_B));
	value32 = xmem_bk_3232(BK_32_A, BK_32_B);
	check("bk-2-32", value32 == BK_32_A + BK_32_B);
	value8 = xmem_bk_888(BK_8_A, BK_8_B, 1u);
	check("bk-3-8", value8 == (unsigned char)(BK_8_A + BK_8_B + 1u));
	value16 = xmem_bk_161616(BK_16_A, BK_16_B, 1u);
	check("bk-3-16", value16 == (unsigned int)(BK_16_A + BK_16_B + 1u));
	value32 = xmem_bk_323232(BK_32_A, BK_32_B, 1ul);
	check("bk-3-32", value32 == BK_32_A + BK_32_B + 1ul);
	value32 = xmem_bk_mix(BK_8_A, BK_16_A, BK_32_A);
	check("bk-3-mix",
	      value32 == BK_32_A + (unsigned long)BK_16_A +
			 (unsigned long)BK_8_A);
	value16 = xmem_bk_rec(4u);
	check("bk-recursion", value16 == 10u);

	/* Segment 2's banked entry calls back into segment 1: the inner
	 * call must restore *its* caller's mapping, so the latch and the
	 * far literal it reads after that call are segment 2's own.  Byte
	 * 1 of tests/xmem/xmem_far_code2.c's literal is 'a'. */
	value16 = xmem_bk2(BK_16_A);
	check("bk-2-bank", (value16 >> 8) == (t2 & 0xFFu));
	check("bk-2-value",
	      (value16 & 0xFFu) ==
		      (((xmem_bk_16(BK_16_A) & 0xFFu) + 0x61u) &
		       0xFFu));

	/* A `__banked` definition in the primary bank names no far
	 * segment: the trampoline reports KOS_RT_EFARMAP (DE = -code)
	 * instead of calling it. */
	result = (int)xmem_bk_bank0(BK_16_A);
	check("bk-invalid-segment", result == -KOS_RT_EFARMAP);

	/* Every call above restored this caller's latch. */
	check("bk-aperture-closed",
	      ((unsigned int)latch_port & 0xE0u) == 0u);

	printf("xmem-far: %u checks, %u failures\r\n", checks, failures);
	if (failures == 0u) {
		console_puts("xmem-far: PASS\r\n");
	} else {
		console_puts("xmem-far: FAIL\r\n");
	}

	/* Exit from far context: the far entry runs SYS_EXIT with the
	 * aperture open onto segment 1.  It does not return; the
	 * launcher's report and the parent's capacity check prove that
	 * exit released the segments, the objects and the bank slots. */
	(void)farcall(1u, xmem_far_exit,
			   (failures == 0u) ? (int)FAR_STATUS : 1, 0);
	return 1; /* not reached */
}
