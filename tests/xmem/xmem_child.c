/* KonaOS extended-memory outcome acceptance (work packages 43/44b): the
 * nested-exec child.
 *
 * SYS_EXEC starts it twice: from /system/xmem_selftest.exe while that
 * program's (object) aperture is open, and from the far code of
 * /system/xmem_far.exe while a FAR_CODE segment's aperture is open.  It
 * proves what the exec'ing process cannot, from its own side:
 *
 *   * the child starts in mode 10 with page 0 (exec starts every child
 *     with P = 0, architecture 36.17) - it reads the banking latch itself,
 *     and the simulator and the hardware are the authority;
 *   * `8000h-BFFFh` is its own bank, not the parent's aperture: it writes
 *     markers there, and the parent checks afterwards that its mapped
 *     objects still hold their own bytes;
 *   * a child inherits none of the parent's objects: every use of the
 *     parent's live handle (passed in the command tail) is refused
 *     (architecture 36.15: a foreign handle is KOS_EINVAL);
 *   * a child inherits none of the parent's segments either: the
 *     parent's live FAR_CODE segment id (also in the tail) has no
 *     description and no token here, even though the id namespace is
 *     per-container;
 *   * its own objects work, including a mapped one written through the
 *     aperture;
 *   * it exits from far context (aperture open) with objects leaked, so
 *     the parent's post-exit capacity check proves exit released them all.
 *
 * The command tail is "<handle> <segment>": the exec'ing process's live
 * object handle and far-segment id, or 0 for either.  Its exit status is
 * the value the parent compares (CHILD_STATUS when every check passed, 1
 * otherwise).
 */

#include "kos.h"

#define CHILD_STATUS 42

#define APERTURE_BASE 0x8000u
#define APERTURE_END  0xC000u

/* The banking latch (port 00h): mode in D4-D3, bank in D2-D0, page in
 * D7-D5.  Read directly - no SDK call should touch it. */
__sfr __at 0x00 latch_port;

static unsigned char pattern[128];
static unsigned char buf[128];

static void make_pattern(unsigned char seed)
{
	unsigned int i;

	for (i = 0u; i < sizeof(pattern); ++i) {
		pattern[i] = (unsigned char)(seed + i);
	}
}

static int matches_pattern(void)
{
	unsigned int i;

	for (i = 0u; i < sizeof(pattern); ++i) {
		if (buf[i] != pattern[i]) {
			return 0;
		}
	}
	return 1;
}

/* The exec'ing process's command tail: "<handle> <segment>", the live
 * object handle and far-segment id to attempt, or 0 for either.  Returns
 * 1 when the tail was parsed, 0 otherwise. */
static int tail_numbers(unsigned int *handle, unsigned int *segment)
{
	struct kos_startup *boot = startup();
	unsigned int value = 0u;
	unsigned int i = 0u;

	*handle = 0u;
	*segment = 0u;
	if (boot == (struct kos_startup *)0 || boot->version != 1u ||
	    boot->length == 0u) {
		return 0;
	}
	while (i < (unsigned int)boot->length && boot->tail[i] >= '0' &&
	       boot->tail[i] <= '9') {
		value = value * 10u + (unsigned int)(boot->tail[i] - '0');
		++i;
	}
	*handle = value;
	if (i >= (unsigned int)boot->length || boot->tail[i] == '\0') {
		return 1;
	}
	++i; /* one separator */
	value = 0u;
	while (i < (unsigned int)boot->length && boot->tail[i] >= '0' &&
	       boot->tail[i] <= '9') {
		value = value * 10u + (unsigned int)(boot->tail[i] - '0');
		++i;
	}
	*segment = value;
	return 1;
}

int main(int argc, char **argv)
{
	unsigned char latch = latch_port;
	struct kos_seg_info info;
	unsigned int foreign = 0u;
	unsigned int foreign_seg = 0u;
	unsigned int address = 0u;
	unsigned int token = 0u;
	unsigned int saved;
	int h1 = -1;
	int h2 = -1;
	int ok = 1;

	(void)argc;
	(void)argv;

	console_puts("xmem-child: begin\r\n");
	printf("xmem-child: latch %02x\r\n", (unsigned int)latch);
	/* The page field must be zero: a child that inherited the parent's
	 * aperture would sabotage everything it writes above 8000h. */
	if ((latch & 0xE0u) != 0u) {
		console_puts("xmem-child: FAIL latch-page\r\n");
		ok = 0;
	}
	if (!tail_numbers(&foreign, &foreign_seg)) {
		console_puts("xmem-child: FAIL foreign-handle\r\n");
		ok = 0;
	}

	/* Its own bank, not an aperture: marker bytes across the aperture
	 * range.  With P = 0 these land in the child's own bank, which the
	 * parent's post-exec check distinguishes from its mapped objects. */
	*(volatile unsigned char *)APERTURE_BASE = 0x5Au;
	*(volatile unsigned char *)(APERTURE_BASE + 0x1000u) = 0x5Au;
	*(volatile unsigned char *)(APERTURE_BASE + 0x2000u) = 0x5Au;
	*(volatile unsigned char *)(APERTURE_END - 1u) = 0x5Au;
	if (*(volatile unsigned char *)APERTURE_BASE != 0x5Au ||
	    *(volatile unsigned char *)(APERTURE_END - 1u) != 0x5Au) {
		console_puts("xmem-child: FAIL own-bank\r\n");
		ok = 0;
	}

	/* The parent's live object (its handle arrives in the tail): a child
	 * inherits no objects, so every operation must be refused. */
	if (foreign == 0u) {
		console_puts("xmem-child: FAIL foreign-handle\r\n");
		ok = 0;
	} else {
		if (xfree(foreign) != -KOS_EINVAL) {
			console_puts("xmem-child: FAIL foreign-free\r\n");
			ok = 0;
		}
		if (xread(foreign, 0u, buf, 1u) != -KOS_EINVAL) {
			console_puts("xmem-child: FAIL foreign-read\r\n");
			ok = 0;
		}
		if (xwrite(foreign, 0u, pattern, 1u) != -KOS_EINVAL) {
			console_puts("xmem-child: FAIL foreign-write\r\n");
			ok = 0;
		}
		if (xmap(foreign, &address) != -KOS_EINVAL) {
			console_puts("xmem-child: FAIL foreign-map\r\n");
			ok = 0;
		}
	}

	/* And the exec'ing process's live FAR_CODE segments (their id
	 * arrives in the tail): a child inherits none, so an id that exists
	 * in the parent's container does not exist here - no description
	 * and no token, even though this process could map its own segment
	 * with the same id. */
	if (foreign_seg != 0u) {
		info.size = 0u;
		info.kind = 0u;
		info.state = 0u;
		info.handle = 0u;
		info.reserved = 0u;
		if (seginfo(foreign_seg, &info) != -KOS_EINVAL) {
			console_puts("xmem-child: FAIL foreign-seg-info\r\n");
			ok = 0;
		}
		if (segmap(foreign_seg) != -KOS_EINVAL) {
			console_puts("xmem-child: FAIL foreign-seg-map\r\n");
			ok = 0;
		}
	}

	/* Its own objects work. */
	h1 = xalloc(32768u, KOS_XMEM_FLAG_NONE);
	if (h1 < 1) {
		console_puts("xmem-child: FAIL own-alloc\r\n");
		ok = 0;
	} else {
		make_pattern(0x11u);
		if (xwrite((unsigned int)h1, 0u, pattern,
				   sizeof(pattern)) != (int)sizeof(pattern) ||
		    xread((unsigned int)h1, 0u, buf,
				  sizeof(buf)) != (int)sizeof(buf) ||
		    !matches_pattern()) {
			console_puts("xmem-child: FAIL own-io\r\n");
			ok = 0;
		}
	}

	h2 = xalloc(8192u, KOS_XMEM_FLAG_MAPPABLE);
	if (h2 < 1) {
		console_puts("xmem-child: FAIL own-mappable\r\n");
		ok = 0;
	} else {
		int mapped = xmap((unsigned int)h2, &address);

		if (mapped < 1 || address < APERTURE_BASE ||
		    address >= APERTURE_END) {
			console_puts("xmem-child: FAIL own-map\r\n");
			ok = 0;
		} else {
			token = (unsigned int)mapped;
			saved = (unsigned int)aperture_open(
				(unsigned char)token);
			*(volatile unsigned char *)address = 0x3Cu;
			*(volatile unsigned char *)(address + 0x7FFu) =
				0x3Cu;
			aperture_close((unsigned char)saved);
			(void)xread((unsigned int)h2, 0u, buf, 1u);
			if (buf[0] != 0x3Cu) {
				console_puts("xmem-child: FAIL own-aperture\r\n");
				ok = 0;
			}
		}
	}

	/* A freed handle is stale even for a fresh object with the same
	 * index: the generation in the handle changes. */
	if (h1 >= 1) {
		int again;

		if (xfree((unsigned int)h1) != 0) {
			console_puts("xmem-child: FAIL own-free\r\n");
			ok = 0;
		}
		again = xalloc(32768u, KOS_XMEM_FLAG_NONE);
		if (again < 1 || again == h1) {
			console_puts("xmem-child: FAIL stale-handle\r\n");
			ok = 0;
		} else if (xfree((unsigned int)h1) != -KOS_EINVAL) {
			console_puts("xmem-child: FAIL stale-free\r\n");
			ok = 0;
		} else {
			h1 = again; /* leaked on purpose, like h2 */
		}
	}

	/* Exit from far context, with both objects leaked: the aperture is
	 * left open so the exit path runs with P != 0. */
	if (token != 0u) {
		saved = (unsigned int)aperture_open((unsigned char)token);
		(void)saved;
	}
	if (ok) {
		console_puts("xmem-child: PASS\r\n");
		return CHILD_STATUS;
	}
	console_puts("xmem-child: FAIL\r\n");
	return 1;
}
