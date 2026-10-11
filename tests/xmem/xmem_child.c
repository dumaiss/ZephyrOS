/* ZephyrOS extended-memory outcome acceptance (work package 43): the
 * nested-exec child.
 *
 * SYS_EXEC starts it from /SYSTEM/xmem_child.zex while the parent's
 * aperture is open, so this program proves what the parent cannot:
 *
 *   * the child starts in mode 10 with page 0 (exec starts every child
 *     with P = 0, architecture 36.17) - it reads the banking latch itself,
 *     and the simulator and the hardware are the authority;
 *   * `8000h-BFFFh` is its own bank, not the parent's aperture: it writes
 *     markers there, and the parent checks afterwards that its mapped
 *     objects still hold their own bytes;
 *   * a child inherits none of the parent's objects: every use of the
 *     parent's live handle (passed in the command tail) is refused
 *     (architecture 36.15: a foreign handle is ZOS_EINVAL);
 *   * its own objects work, including a mapped one written through the
 *     aperture;
 *   * it exits from far context (aperture open) with objects leaked, so
 *     the parent's post-exit capacity check proves exit released them all.
 *
 * Its exit status is the value the parent compares (CHILD_STATUS when
 * every check passed, 1 otherwise).
 */

#include "zos.h"

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

/* The parent's handle number arrives as the command tail in decimal. */
static unsigned int tail_handle(void)
{
	struct zos_startup *boot = zos_args();
	unsigned int value = 0u;
	unsigned int i = 0u;

	if (boot == (struct zos_startup *)0 || boot->version != 1u ||
	    boot->length == 0u) {
		return 0u;
	}
	while (i < (unsigned int)boot->length && boot->tail[i] != '\0') {
		char c = boot->tail[i];

		if (c < '0' || c > '9') {
			return 0u;
		}
		value = value * 10u + (unsigned int)(c - '0');
		++i;
	}
	return value;
}

int main(int argc, char **argv)
{
	unsigned char latch = latch_port;
	unsigned int foreign = tail_handle();
	unsigned int address = 0u;
	unsigned int token = 0u;
	unsigned int saved;
	int h1 = -1;
	int h2 = -1;
	int ok = 1;

	(void)argc;
	(void)argv;

	zos_puts("xmem-child: begin\r\n");
	zos_printf("xmem-child: latch %02x\r\n", (unsigned int)latch);
	/* The page field must be zero: a child that inherited the parent's
	 * aperture would sabotage everything it writes above 8000h. */
	if ((latch & 0xE0u) != 0u) {
		zos_puts("xmem-child: FAIL latch-page\r\n");
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
		zos_puts("xmem-child: FAIL own-bank\r\n");
		ok = 0;
	}

	/* The parent's live object (its handle arrives in the tail): a child
	 * inherits no objects, so every operation must be refused. */
	if (foreign == 0u) {
		zos_puts("xmem-child: FAIL foreign-handle\r\n");
		ok = 0;
	} else {
		if (zos_xmem_free(foreign) != -ZOS_EINVAL) {
			zos_puts("xmem-child: FAIL foreign-free\r\n");
			ok = 0;
		}
		if (zos_xmem_read(foreign, 0u, buf, 1u) != -ZOS_EINVAL) {
			zos_puts("xmem-child: FAIL foreign-read\r\n");
			ok = 0;
		}
		if (zos_xmem_write(foreign, 0u, pattern, 1u) != -ZOS_EINVAL) {
			zos_puts("xmem-child: FAIL foreign-write\r\n");
			ok = 0;
		}
		if (zos_xmem_map(foreign, &address) != -ZOS_EINVAL) {
			zos_puts("xmem-child: FAIL foreign-map\r\n");
			ok = 0;
		}
	}

	/* Its own objects work. */
	h1 = zos_xmem_alloc(32768u, ZOS_XMEM_FLAG_NONE);
	if (h1 < 1) {
		zos_puts("xmem-child: FAIL own-alloc\r\n");
		ok = 0;
	} else {
		make_pattern(0x11u);
		if (zos_xmem_write((unsigned int)h1, 0u, pattern,
				   sizeof(pattern)) != (int)sizeof(pattern) ||
		    zos_xmem_read((unsigned int)h1, 0u, buf,
				  sizeof(buf)) != (int)sizeof(buf) ||
		    !matches_pattern()) {
			zos_puts("xmem-child: FAIL own-io\r\n");
			ok = 0;
		}
	}

	h2 = zos_xmem_alloc(8192u, ZOS_XMEM_FLAG_MAPPABLE);
	if (h2 < 1) {
		zos_puts("xmem-child: FAIL own-mappable\r\n");
		ok = 0;
	} else {
		int mapped = zos_xmem_map((unsigned int)h2, &address);

		if (mapped < 1 || address < APERTURE_BASE ||
		    address >= APERTURE_END) {
			zos_puts("xmem-child: FAIL own-map\r\n");
			ok = 0;
		} else {
			token = (unsigned int)mapped;
			saved = (unsigned int)zos_aperture_open(
				(unsigned char)token);
			*(volatile unsigned char *)address = 0x3Cu;
			*(volatile unsigned char *)(address + 0x7FFu) =
				0x3Cu;
			zos_aperture_close((unsigned char)saved);
			(void)zos_xmem_read((unsigned int)h2, 0u, buf, 1u);
			if (buf[0] != 0x3Cu) {
				zos_puts("xmem-child: FAIL own-aperture\r\n");
				ok = 0;
			}
		}
	}

	/* A freed handle is stale even for a fresh object with the same
	 * index: the generation in the handle changes. */
	if (h1 >= 1) {
		int again;

		if (zos_xmem_free((unsigned int)h1) != 0) {
			zos_puts("xmem-child: FAIL own-free\r\n");
			ok = 0;
		}
		again = zos_xmem_alloc(32768u, ZOS_XMEM_FLAG_NONE);
		if (again < 1 || again == h1) {
			zos_puts("xmem-child: FAIL stale-handle\r\n");
			ok = 0;
		} else if (zos_xmem_free((unsigned int)h1) != -ZOS_EINVAL) {
			zos_puts("xmem-child: FAIL stale-free\r\n");
			ok = 0;
		} else {
			h1 = again; /* leaked on purpose, like h2 */
		}
	}

	/* Exit from far context, with both objects leaked: the aperture is
	 * left open so the exit path runs with P != 0. */
	if (token != 0u) {
		saved = (unsigned int)zos_aperture_open((unsigned char)token);
		(void)saved;
	}
	if (ok) {
		zos_puts("xmem-child: PASS\r\n");
		return CHILD_STATUS;
	}
	zos_puts("xmem-child: FAIL\r\n");
	return 1;
}
