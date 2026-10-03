/* ZephyrOS user runtime: hexadecimal parsing and formatting.
 *
 * See hexfmt.h for the contract.  No standard library is linked: the
 * conversions are plain shifts, masks, and digit-table lookups.
 */

#include "hexfmt.h"

static const char zrt_hex_digits[] = "0123456789ABCDEF";

static unsigned char zrt_hex_nibble(unsigned char ch, unsigned char *value)
{
	if (ch >= '0' && ch <= '9') {
		*value = (unsigned char)(ch - '0');
		return 1;
	}
	if (ch >= 'A' && ch <= 'F') {
		*value = (unsigned char)(ch - 'A' + 10);
		return 1;
	}
	if (ch >= 'a' && ch <= 'f') {
		*value = (unsigned char)(ch - 'a' + 10);
		return 1;
	}
	return 0;
}

void zrt_skip_spaces(char **cursor)
{
	char *scan = *cursor;

	while (*scan == ' ' || *scan == '\t') {
		++scan;
	}
	*cursor = scan;
}

unsigned char zrt_parse_hex(char **cursor, unsigned int *value)
{
	char *scan;
	unsigned int result = 0;
	unsigned char digits = 0;
	unsigned char nibble;

	zrt_skip_spaces(cursor);
	scan = *cursor;
	while (zrt_hex_nibble((unsigned char)*scan, &nibble)) {
		if (digits == 4) {
			return 0; /* more than four digits: malformed */
		}
		result = (unsigned int)((result << 4) | nibble);
		++scan;
		++digits;
	}
	if (digits == 0) {
		return 0;
	}
	*cursor = scan;
	*value = result;
	return 1;
}

unsigned char zrt_parse_hex8(char **cursor, unsigned int *value)
{
	unsigned int word;

	if (!zrt_parse_hex(cursor, &word)) {
		return 0;
	}
	if (word > 0xFFu) {
		return 0;
	}
	*value = word;
	return 1;
}

unsigned char zrt_line_end(char **cursor)
{
	zrt_skip_spaces(cursor);
	return (unsigned char)(**cursor == '\0');
}

void zrt_fmt_hex8(char *dst, unsigned int value)
{
	dst[0] = zrt_hex_digits[(value >> 4) & 0x0Fu];
	dst[1] = zrt_hex_digits[value & 0x0Fu];
	dst[2] = '\0';
}

void zrt_fmt_hex16(char *dst, unsigned int value)
{
	zrt_fmt_hex8(dst, (value >> 8) & 0xFFu);
	zrt_fmt_hex8(dst + 2, value & 0xFFu);
}
