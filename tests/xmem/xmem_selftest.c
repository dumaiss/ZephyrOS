/* ZephyrOS extended-memory outcome acceptance (work package 43).
 *
 * The ZEX primary the `xmem-selftest` image boots: an ordinary SDK
 * program that drives the extended-memory family and the mode-10 far
 * aperture through `zos/xmem.h` and prints one line per outcome.  It
 * covers the architecture-37 `xmem` rows and the far-data rows that need
 * no ZEX v2 (far-code segments are package 44):
 *
 *   * sizes and slot rounding (1 / 8191 / 8192 / 8193 bytes), the largest
 *     object, oversized rejection, exhaustion and fragmentation;
 *   * stale and foreign handle rejection (the child it execs proves the
 *     foreign half; the child's handle arrives in the command tail);
 *   * no pre-zeroing: a freed object's bytes are what the next object
 *     starts with;
 *   * first/last byte, slot boundary and whole-object transfers, and
 *     range rejection before any copy;
 *   * multiple objects per bank with independent contents;
 *   * the aperture: MAP returns the token and the address, direct reads
 *     and writes through it, and buffer identity - a syscall buffer in
 *     `8000h-BFFFh` names the far bank, so the exec path itself can live
 *     in the aperture;
 *   * exec from far context: the child starts with P = 0, the parent's
 *     latch is restored exactly, and the child's leak is released at exit
 *     (capacity before == capacity after);
 *   * exit releases the process's own objects: the program leaves the two
 *     anchor objects live, and the launcher's `free-banks 6` report
 *     proves the release (their bank is free again);
 *   * far code (package 44b): it SYS_EXECs /SYSTEM/xmem_far.zex, whose
 *     two FAR_CODE segments run through the aperture, and its own
 *     capacity check around that exec proves the far program's exit
 *     released its segments, objects and bank slots.
 *
 * The far-code program's own outcomes are
 * tests/xmem/xmem_far.c.  The replay asserts the lines below; the final
 * line is the verdict.
 *
 * Work package 45b adds one tail option, `stage`: the production
 * exerciser runs this program beside the monitor and the runner, so two
 * application banks are held by their live processes and only the others
 * are open to auxiliary placement (36.15).  The pool-wide rows are then
 * stated against the number of banks this process can actually open
 * (measured by filling whole banks), and the far sub-exec stays with the
 * runner: this program already sits at the third level of the four-deep
 * exec chain, and nesting xmem_far's own exec below it would be the
 * fifth.  The boot path - the acceptance image's own rows and the far
 * sub-exec - is unchanged.
 */

#include "zos.h"

#define MAPPABLE ZOS_XMEM_FLAG_MAPPABLE

#define APERTURE_BASE 0x8000u
#define APERTURE_END  0xC000u
#define BLK           512u

/* The stage tail (work package 45b). */
#define XMEM_STAGE_TAIL "stage"

#define CHILD_PATH   "/SYSTEM/xmem_child.zex"
#define CHILD_STATUS 42
#define FAR_PATH     "/SYSTEM/xmem_far.zex"
#define FAR_STATUS   43

/* The banking latch (port 00h): mode in D4-D3, bank in D2-D0, page in
 * D7-D5.  The aperture checks read it directly. */
__sfr __at 0x00 latch_port;

static unsigned char pattern[BLK];
static unsigned char buf[BLK];
/* One entry per object the allocation loops hold; they stop at ZOS_ENOSPC
 * well before this. */
static unsigned int held[20];

static unsigned int checks;
static unsigned int failures;
/* 1: the exerciser's stage run (45b): pool rows scale with the open
 * banks and the far sub-exec belongs to the runner. */
static int stage;
/* Whole banks this process can open: 5 in the acceptance image (set in
 * main; ZEX v2 has no initialized writable data). */
static unsigned int banks;

static void check(const char *label, int ok)
{
	++checks;
	if (!ok) {
		++failures;
		zos_printf("xmem-selftest: FAIL %s\r\n", label);
		return;
	}
	zos_printf("xmem-selftest: %s OK\r\n", label);
}

/* The object byte at offset `o` is (unsigned char)o, written and checked
 * in blocks so a whole 32 KiB object can be compared. */
static int fill_range(unsigned int handle, unsigned int base,
		      unsigned int length)
{
	unsigned int done = 0u;

	while (done < length) {
		unsigned int chunk = length - done;
		unsigned int i;

		if (chunk > BLK) {
			chunk = BLK;
		}
		for (i = 0u; i < chunk; ++i) {
			pattern[i] = (unsigned char)(base + done + i);
		}
		if (zos_xmem_write(handle, base + done, pattern, chunk) !=
		    (int)chunk) {
			return 0;
		}
		done += chunk;
	}
	return 1;
}

static int check_range(unsigned int handle, unsigned int base,
		       unsigned int length)
{
	unsigned int done = 0u;

	while (done < length) {
		unsigned int chunk = length - done;
		unsigned int i;

		if (chunk > BLK) {
			chunk = BLK;
		}
		if (zos_xmem_read(handle, base + done, buf, chunk) !=
		    (int)chunk) {
			return 0;
		}
		for (i = 0u; i < chunk; ++i) {
			if (buf[i] != (unsigned char)(base + done + i)) {
				return 0;
			}
		}
		done += chunk;
	}
	return 1;
}

static int fill_byte(unsigned int handle, unsigned int offset,
		     unsigned char value, unsigned int length)
{
	unsigned int i;

	for (i = 0u; i < length; ++i) {
		pattern[i] = value;
	}
	return zos_xmem_write(handle, offset, pattern, length) ==
	       (int)length;
}

static int check_byte(unsigned int handle, unsigned int offset,
		      unsigned char value)
{
	return zos_xmem_read(handle, offset, buf, 1u) == 1 &&
	       buf[0] == value;
}

/* Allocate `size`/`flags` until the kernel refuses and return the count.
 * The objects stay held, so the caller can assert the refusal and then
 * release them with release_held(). */
static unsigned int fill_until_full(unsigned int size, unsigned int flags)
{
	unsigned int count = 0u;
	int handle;

	while (count < 20u) {
		handle = zos_xmem_alloc(size, flags);
		if (handle < 1) {
			break;
		}
		held[count++] = (unsigned int)handle;
	}
	return count;
}

static void release_held(unsigned int count)
{
	unsigned int i;

	for (i = 0u; i < count; ++i) {
		(void)zos_xmem_free(held[i]);
	}
}

static void fill_aperture(unsigned int base, unsigned int length,
			  unsigned char value)
{
	volatile unsigned char *p = (volatile unsigned char *)base;
	unsigned int i;

	for (i = 0u; i < length; ++i) {
		p[i] = value;
	}
}

static unsigned char read_aperture(unsigned int address)
{
	return *(volatile unsigned char *)address;
}

static void write_aperture_string(unsigned int address, const char *text)
{
	volatile unsigned char *p = (volatile unsigned char *)address;

	while (*text != '\0') {
		*p++ = (unsigned char)*text++;
	}
	*p = 0u;
}

/* Decimal digits of a handle and the far-segment field, for the child's
 * command tail: "<handle> <segment>".  This program declares no segments,
 * so the field is 0. */
static void child_tail(char *out, unsigned int handle)
{
	char digits[6];
	unsigned int count = 0u;

	do {
		digits[count++] = (char)('0' + handle % 10u);
		handle /= 10u;
	} while (handle != 0u);
	while (count != 0u) {
		*out++ = digits[--count];
	}
	*out++ = ' ';
	*out++ = '0';
	*out = '\0';
}

static int same(const char *a, const char *b)
{
	while (*a == *b) {
		if (*a == '\0') {
			return 1;
		}
		++a;
		++b;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct zos_startup *boot = zos_args();
	const char *tail = (const char *)0;
	struct zos_xmem_info info;
	unsigned int anchor;
	unsigned int second;
	unsigned int token_a;
	unsigned int token_b;
	unsigned int address_a = 0u;
	unsigned int address_b = 0u;
	unsigned int saved;
	unsigned int count;
	unsigned int i;
	int handle;
	int status;

	/* The stage tail (45b) is the only accepted option. */
	if (argc == 1 && argv != (char **)0 && same(argv[0], XMEM_STAGE_TAIL)) {
		stage = 1;
		tail = XMEM_STAGE_TAIL;
	} else if (argc != 0) {
		zos_puts("xmem-selftest: FAIL options\r\n");
		return 2;
	}

	zos_puts("xmem-selftest: begin\r\n");
	/* Boot contract: version 1 with the empty tail; a stage run proves
	 * the option tail arrived byte-exact instead. */
	check("startup-version",
	      boot != (struct zos_startup *)0 && boot->version == 1u);
	check("startup-tail",
	      boot != (struct zos_startup *)0 &&
	      (tail == (const char *)0
		       ? boot->length == 0u
		       : (boot->length == 5u && same(boot->tail, tail))));
	check("console-write", zos_console_write("=", 1u) == 1);

	/* The pool this run may use: five banks when this image is the whole
	 * machine (the acceptance image), the measured count when the
	 * exerciser runs it beside the monitor and the runner.  A whole-bank
	 * (32 KiB) object needs slots 0-3 of one bank, so the count of them
	 * is the number of open banks. */
	banks = 5u;
	if (stage != 0) {
		banks = fill_until_full(32768u, 0u);
		release_held(banks);
	}

	/* The two anchors are the first objects this image allocates: the
	 * first gets handle 1 and the lower aperture slot, the second the
	 * upper slot.  They stay live until the program exits. */
	handle = zos_xmem_alloc(8192u, MAPPABLE);
	anchor = (unsigned int)handle;
	handle = zos_xmem_alloc(8192u, MAPPABLE);
	second = (unsigned int)handle;
	check("anchor-alloc", anchor >= 1u && second >= 1u);

	info.requested = 0u;
	info.allocated = 0u;
	info.flags = 0u;
	info.type = 0u;
	info.reserved = 0u;
	check("anchor-info",
	      zos_xmem_info(anchor, &info) == 0 && info.requested == 8192u &&
	      info.allocated == 8192u && info.flags == MAPPABLE &&
	      info.type == ZOS_XMEM_TYPE_RUNTIME);

	/* Sizes and rounding. */
	check("alloc-zero-rejected", zos_xmem_alloc(0u, 0u) == -ZOS_EINVAL);
	check("alloc-flags-rejected", zos_xmem_alloc(64u, 2u) == -ZOS_EINVAL);
	check("alloc-oversized", zos_xmem_alloc(32769u, 0u) == -ZOS_ENOSPC);
	check("alloc-mappable-oversized",
	      zos_xmem_alloc(16385u, MAPPABLE) == -ZOS_EINVAL);

	handle = zos_xmem_alloc(1u, 0u);
	check("size-1",
	      handle >= 1 &&
	      zos_xmem_info((unsigned int)handle, &info) == 0 &&
	      info.requested == 1u && info.allocated == 8192u);
	(void)zos_xmem_free((unsigned int)handle);
	handle = zos_xmem_alloc(8191u, 0u);
	check("size-8191",
	      handle >= 1 &&
	      zos_xmem_info((unsigned int)handle, &info) == 0 &&
	      info.allocated == 8192u);
	(void)zos_xmem_free((unsigned int)handle);
	handle = zos_xmem_alloc(8192u, 0u);
	check("size-8192",
	      handle >= 1 &&
	      zos_xmem_info((unsigned int)handle, &info) == 0 &&
	      info.allocated == 8192u);
	(void)zos_xmem_free((unsigned int)handle);
	handle = zos_xmem_alloc(8193u, 0u);
	check("size-8193",
	      handle >= 1 &&
	      zos_xmem_info((unsigned int)handle, &info) == 0 &&
	      info.requested == 8193u && info.allocated == 16384u);

	/* One object, every boundary: first byte, last byte, the slot
	 * boundary inside it and the whole 16384 bytes. */
	check("boundary-first",
	      fill_byte((unsigned int)handle, 0u, 0x11u, 1u) &&
	      check_byte((unsigned int)handle, 0u, 0x11u));
	check("boundary-last",
	      fill_byte((unsigned int)handle, 16383u, 0x22u, 1u) &&
	      check_byte((unsigned int)handle, 16383u, 0x22u));
	check("boundary-slot",
	      fill_byte((unsigned int)handle, 8192u - 32u, 0x33u, 64u) &&
	      check_byte((unsigned int)handle, 8192u - 32u, 0x33u) &&
	      check_byte((unsigned int)handle, 8192u + 31u, 0x33u));
	check("whole-object",
	      fill_range((unsigned int)handle, 0u, 16384u) &&
	      check_range((unsigned int)handle, 0u, 16384u));
	check("range-read-end",
	      zos_xmem_read((unsigned int)handle, 16384u, buf, 1u) ==
	      -ZOS_EINVAL);
	check("range-read-straddle",
	      zos_xmem_read((unsigned int)handle, 16383u, buf, 2u) ==
	      -ZOS_EINVAL);
	check("range-write-end",
	      zos_xmem_write((unsigned int)handle, 16384u, pattern, 1u) ==
	      -ZOS_EINVAL);
	check("range-offset",
	      zos_xmem_read((unsigned int)handle, 0xF000u, buf, 1u) ==
	      -ZOS_EINVAL);
	check("range-buffer",
	      zos_xmem_read((unsigned int)handle, 0u, (void *)0xE000u, 1u) ==
	      -ZOS_EINVAL);
	{
		unsigned int old = (unsigned int)handle;

		(void)zos_xmem_free(old);
		check("stale-free", zos_xmem_free(old) == -ZOS_EINVAL);
		handle = zos_xmem_alloc(8193u, 0u);
		check("stale-reused",
		      handle >= 1 && (unsigned int)handle != old);
		check("stale-read-live",
		      zos_xmem_read(old, 0u, buf, 1u) == -ZOS_EINVAL);
		check("stale-info-live",
		      zos_xmem_info(old, &info) == -ZOS_EINVAL);
		(void)zos_xmem_free((unsigned int)handle);
	}

	/* The largest object (32 KiB) and no pre-zeroing. */
	handle = zos_xmem_alloc(32768u, 0u);
	check("largest-alloc",
	      handle >= 1 &&
	      zos_xmem_info((unsigned int)handle, &info) == 0 &&
	      info.allocated == 32768u);
	check("largest-head",
	      fill_byte((unsigned int)handle, 0u, 0x44u, 1u) &&
	      check_byte((unsigned int)handle, 0u, 0x44u));
	check("largest-tail",
	      fill_byte((unsigned int)handle, 32767u, 0x55u, 1u) &&
	      check_byte((unsigned int)handle, 32767u, 0x55u));
	check("largest-whole",
	      fill_range((unsigned int)handle, 0u, 32768u) &&
	      check_range((unsigned int)handle, 0u, 32768u));
	(void)fill_byte((unsigned int)handle, 0u, 0x66u, 1u);
	(void)fill_byte((unsigned int)handle, 32767u, 0x67u, 1u);
	(void)zos_xmem_free((unsigned int)handle);
	handle = zos_xmem_alloc(32768u, 0u);
	check("no-zeroing",
	      handle >= 1 &&
	      check_byte((unsigned int)handle, 0u, 0x66u) &&
	      check_byte((unsigned int)handle, 32767u, 0x67u));
	(void)zos_xmem_free((unsigned int)handle);

	/* Multiple objects in one bank, independent contents. */
	{
		unsigned int multi[5];
		int ok = 1;
		int j;

		for (i = 0u; i < 5u; ++i) {
			handle = zos_xmem_alloc(8192u, 0u);
			if (handle < 1) {
				ok = 0;
				handle = 0;
			}
			multi[i] = (unsigned int)handle;
		}
		check("multi-alloc", ok);
		if (ok) {
			for (i = 0u; i < 5u; ++i) {
				(void)fill_byte(multi[i], 0u,
						(unsigned char)(0x70u + i),
						64u);
				(void)fill_byte(multi[i], 8191u,
						(unsigned char)(0x80u + i),
						1u);
			}
			ok = 1;
			for (i = 0u; i < 5u; ++i) {
				if (!check_byte(multi[i], 0u,
						(unsigned char)(0x70u + i)) ||
				    !check_byte(multi[i], 8191u,
						(unsigned char)(0x80u + i))) {
					ok = 0;
				}
			}
			check("multi-contents", ok);
			ok = 1;
			for (i = 0u; i < 5u; ++i) {
				for (j = (int)i + 1; j < 5; ++j) {
					if (multi[i] == multi[j]) {
						ok = 0;
					}
				}
			}
			check("multi-distinct", ok);
			for (i = 0u; i < 5u; ++i) {
				(void)zos_xmem_free(multi[i]);
			}
		}
	}

	/* Fragmentation: whole banks held, free slots remain, and no run is
	 * large enough - until one bank is released. */
	{
		unsigned int big[5];
		int ok = 1;

		for (i = 0u; i < banks; ++i) {
			handle = zos_xmem_alloc(32768u, 0u);
			if (handle < 1) {
				ok = 0;
				handle = 0;
			}
			big[i] = (unsigned int)handle;
		}
		check("fill-banks", ok);
		check("fragmentation-32k",
		      zos_xmem_alloc(32768u, 0u) == -ZOS_ENOSPC);
		check("fragmentation-16k",
		      zos_xmem_alloc(16384u, 0u) == -ZOS_ENOSPC);
		handle = zos_xmem_alloc(8192u, 0u);
		check("fragmentation-hole", handle >= 1);
		(void)zos_xmem_free(big[banks - 1u]);
		{
			int again = zos_xmem_alloc(32768u, 0u);

			check("fragmentation-refill", again >= 1);
			(void)zos_xmem_free((unsigned int)again);
		}
		(void)zos_xmem_free((unsigned int)handle);
		for (i = 0u; i + 1u < banks; ++i) {
			(void)zos_xmem_free(big[i]);
		}
	}

	/* Exhaustion: 16 KiB objects fill every two-slot run, the kernel
	 * refuses while they are held, and release returns the capacity. */
	count = fill_until_full(16384u, 0u);
	check("exhaustion-16k-count", count == 2u * banks);
	check("exhaustion-16k",
	      zos_xmem_alloc(16384u, 0u) == -ZOS_ENOSPC);
	release_held(count);
	handle = zos_xmem_alloc(16384u, 0u);
	check("exhaustion-16k-refill", handle >= 1);
	(void)zos_xmem_free((unsigned int)handle);
	/* Every aperture slot except the two the anchors hold. */
	count = fill_until_full(8192u, MAPPABLE);
	check("exhaustion-mappable-count", count == 2u * banks - 2u);
	check("exhaustion-mappable",
	      zos_xmem_alloc(8192u, MAPPABLE) == -ZOS_ENOSPC);
	release_held(count);
	handle = zos_xmem_alloc(8192u, MAPPABLE);
	check("exhaustion-mappable-refill", handle >= 1);
	(void)zos_xmem_free((unsigned int)handle);

	/* The aperture: token, address, direct access and buffer identity. */
	handle = zos_xmem_map(anchor, &address_a);
	token_a = (unsigned int)handle;
	check("map-token", token_a >= 1u);
	check("map-address-low", token_a >= 1u && address_a == APERTURE_BASE);
	handle = zos_xmem_map(second, &address_b);
	token_b = (unsigned int)handle;
	check("map-address-high",
	      token_b >= 1u && address_b == APERTURE_BASE + 0x2000u);
	/* Tokens exist only for application banks 0-5: the page field is
	 * 7 - bank, so 2..7, and the bank field is the caller's own. */
	check("map-token-page",
	      ((token_a >> 5) & 7u) >= 2u &&
	      (token_a & 7u) == ((unsigned int)latch_port & 7u) &&
	      (token_b & 7u) == ((unsigned int)latch_port & 7u));
	handle = zos_xmem_alloc(64u, 0u);
	check("map-non-mappable",
	      handle >= 1 &&
	      zos_xmem_map((unsigned int)handle, &address_a) == -ZOS_EINVAL);
	(void)zos_xmem_free((unsigned int)handle);

	saved = (unsigned int)zos_aperture_open((unsigned char)token_a);
	check("aperture-open", (saved & 0xE0u) == 0u);
	fill_aperture(APERTURE_BASE, APERTURE_END - APERTURE_BASE, 0x77u);
	check("aperture-direct",
	      read_aperture(APERTURE_BASE) == 0x77u &&
	      read_aperture(0x8F00u) == 0x77u &&
	      read_aperture(0xA000u) == 0x77u &&
	      read_aperture(APERTURE_END - 1u) == 0x77u);
	check("aperture-xmem-read",
	      zos_xmem_read(anchor, 0x0F00u, buf, BLK) == (int)BLK &&
	      buf[0] == 0x77u && buf[BLK - 1u] == 0x77u);

	/* Buffer identity: a syscall buffer behind the aperture is the far
	 * bank, never the program's own memory at that address. */
	fill_aperture(0x9800u, 64u, 0x91u);
	check("identity-write",
	      zos_xmem_write(anchor, 0x1000u, (void *)0x9800u, 64u) == 64 &&
	      zos_xmem_read(anchor, 0x1000u, buf, 64u) == 64 &&
	      buf[0] == 0x91u && buf[63] == 0x91u);
	check("identity-read",
	      zos_xmem_read(anchor, 0x1000u, (void *)0x9900u, 64u) == 64 &&
	      read_aperture(0x9900u) == 0x91u &&
	      read_aperture(0x993Fu) == 0x91u &&
	      check_byte(anchor, 0x1900u, 0x91u));

	/* Exec from far context: the path itself lives in the aperture.
	 * The child must start with P = 0, the parent's latch must come
	 * back exactly, and the objects the child leaks must be released at
	 * its exit (capacity before == capacity after). */
	{
		char tail[8];
		unsigned int after;

		child_tail(tail, anchor);
		write_aperture_string(0x8100u, CHILD_PATH);
		count = fill_until_full(8192u, 0u);
		release_held(count);
		status = zos_exec((const char *)0x8100u, tail);
		check("exec-status", status == CHILD_STATUS);
		check("exec-latch-restored",
		      (unsigned int)latch_port == token_a);
		check("exec-parent-object-intact",
		      read_aperture(APERTURE_BASE) == 0x77u &&
		      read_aperture(0x8F00u) == 0x77u &&
		      read_aperture(0xA000u) == 0x77u &&
		      read_aperture(APERTURE_END - 1u) == 0x77u);
		after = fill_until_full(8192u, 0u);
		check("exec-child-released", after == count);
		release_held(after);
	}

	zos_aperture_close((unsigned char)saved);
	check("aperture-close", ((unsigned int)latch_port & 0xE0u) == 0u);

	/* Far code (package 44b): /SYSTEM/xmem_far.zex runs its own
	 * FAR_CODE segments through the aperture - map, plain CALL and
	 * return, far code calling primary code, syscalls with an aperture
	 * buffer, and an exec from far context whose child refuses this
	 * process's handle and segment id.  It exits from far context; this
	 * process's aperture capacity before and after proves the exit
	 * released its segments, objects and bank slots.
	 *
	 * In a stage run the runner invokes the same program itself, one
	 * exec level shallower (45b): this program is already the third of
	 * the four-deep chain, so the far program's own nested exec would be
	 * the fifth and the kernel refuses it before any bank is used.  The
	 * runner checks the far program's exit status and that its exit
	 * returned every bank, so no row of the far program is lost. */
	if (stage == 0) {
		unsigned int before = fill_until_full(8192u, MAPPABLE);

		release_held(before);
		status = zos_exec(FAR_PATH, "");
		check("far-program-status", status == FAR_STATUS);
		count = fill_until_full(8192u, MAPPABLE);
		check("far-program-released", count == before);
		release_held(count);
	}

	/* The anchors stay live: exit must release them, which the
	 * launcher's `free-banks 6` report proves. */
	check("hold-anchors",
	      zos_xmem_info(anchor, &info) == 0 &&
	      zos_xmem_info(second, &info) == 0);

	zos_printf("xmem-selftest: %u checks, %u failures\r\n", checks,
		   failures);
	if (failures == 0u) {
		zos_puts("xmem-selftest: PASS\r\n");
	} else {
		zos_puts("xmem-selftest: FAIL\r\n");
	}
	return (failures == 0u) ? 0 : 1;
}
