/* ZephyrOS user runtime: 16-byte hexdump row formatting.
 *
 * See hexdump.h for the row contract.
 */

#include "hexdump.h"
#include "hexfmt.h"

void zrt_hexdump_row(char *dst, unsigned int addr,
		     const unsigned char *bytes, unsigned int count)
{
	char *out = dst;
	unsigned int i;

	zrt_fmt_hex16(out, addr);
	out += 4;
	*out++ = ':';
	*out++ = ' ';

	for (i = 0; i < count; ++i) {
		zrt_fmt_hex8(out, bytes[i]);
		out += 2;
		*out++ = ' ';
	}
	for (; i < 16u; ++i) {
		*out++ = ' ';
		*out++ = ' ';
		*out++ = ' ';
	}
	*out++ = ' ';

	for (i = 0; i < count; ++i) {
		unsigned char byte = bytes[i];

		*out++ = (byte >= 0x20u && byte < 0x7Fu)
				 ? (char)byte
				 : (char)'.';
	}
	*out = '\0';
}
