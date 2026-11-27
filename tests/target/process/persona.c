/* KonaOS SYS_PERSONALITY probe (test only, Zephyr-80 Mk2).
 *
 * Builds a 64 KiB space from two objects (seven pages and one), each
 * page tagged with its own fill byte at its first and last 256 bytes,
 * page 0 starting with `EI; JR $` at the entry 0000h.  Then the refusals,
 * each from an otherwise valid list: a count of 7, a page past its
 * object's end, a nonzero reserved byte, a handle no object has, a freed
 * object.  It prints the five results and, KonaOS intact, exits 0 when
 * every one is -KOS_EINVAL.
 *
 * `persona go` then takes the RT interrupt (a lone RETN in the RT region,
 * 1 kHz) and makes the call: it does not return.
 * tests/test_mk2_personality.py checks both runs.
 */

#include "kos.h"

#define PERSONA_TAG(n) ((unsigned char)(0xA1u + (n) * 0x11u))

static struct kos_personality_page pages[KOS_PERSONALITY_PAGES];
static unsigned char fill[256];

static int persona_page(unsigned int handle, unsigned int offset,
			unsigned char n)
{
	memset(fill, PERSONA_TAG(n), sizeof(fill));
	if (xwrite(handle, (unsigned int)(offset + 0x1F00u), fill,
		   sizeof(fill)) != (int)sizeof(fill)) {
		return -1;
	}
	if (n == 0u) {
		fill[0] = 0xFBu;  /* EI     */
		fill[1] = 0x18u;  /* JR $   */
		fill[2] = 0xFEu;
	}
	return xwrite(handle, offset, fill, sizeof(fill)) ==
	       (int)sizeof(fill) ? 0 : -1;
}

int main(int argc, char **argv)
{
	int seven = xalloc(0xE000u, KOS_XMEM_FLAG_NONE);
	int one = xalloc(0x2000u, KOS_XMEM_FLAG_NONE);
	int spare = xalloc(0x2000u, KOS_XMEM_FLAG_NONE);
	int result[5];
	unsigned char n;

	if (seven < 0 || one < 0 || spare < 0) {
		console_puts("persona: no memory\r\n");
		return 2;
	}
	for (n = 0u; n < KOS_PERSONALITY_PAGES; ++n) {
		unsigned int handle = n < 7u ? (unsigned int)seven
					     : (unsigned int)one;
		unsigned char page = n < 7u ? n : 0u;

		pages[n].handle = handle;
		pages[n].page = page;
		if (persona_page(handle, (unsigned int)page << 13, n) < 0) {
			console_puts("persona: xwrite failed\r\n");
			return 2;
		}
	}

	result[0] = personality(pages, 7u, 0x0000u);
	pages[3].page = 7u;
	result[1] = personality(pages, KOS_PERSONALITY_PAGES, 0x0000u);
	pages[3].page = 3u;
	pages[2].reserved = 1u;
	result[2] = personality(pages, KOS_PERSONALITY_PAGES, 0x0000u);
	pages[2].reserved = 0u;
	pages[5].handle = 0x01FFu;
	result[3] = personality(pages, KOS_PERSONALITY_PAGES, 0x0000u);
	pages[5].handle = (unsigned int)seven;
	pages[7].handle = (unsigned int)spare;
	(void)xfree((unsigned int)spare);
	result[4] = personality(pages, KOS_PERSONALITY_PAGES, 0x0000u);
	pages[7].handle = (unsigned int)one;
	printf("persona: refused %d %d %d %d %d\r\n", result[0], result[1],
	       result[2], result[3], result[4]);
	for (n = 0u; n < 5u; ++n) {
		if (result[n] != -KOS_EINVAL) {
			return 1;
		}
	}
	if (argc < 1 || argv[0][0] != 'g') {
		return 0;
	}

	*(volatile unsigned char *)KOS_RT_REGION_BASE = 0xEDu; /* RETN */
	*(volatile unsigned char *)(KOS_RT_REGION_BASE + 1u) = 0x45u;
	if (rt_open((const void *)KOS_RT_REGION_BASE, 1000u) != 0) {
		console_puts("persona: no RT clock\r\n");
		return 3;
	}
	console_puts("persona: go\r\n");
	(void)personality(pages, KOS_PERSONALITY_PAGES, 0x0000u);
	console_puts("persona: returned\r\n");
	return 4;
}
