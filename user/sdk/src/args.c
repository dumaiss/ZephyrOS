/* ZephyrOS libzos: startup-block accessor and command-tail splitter.
 *
 * crt0 stores HL (the startup block) into zos_startup_block before it
 * calls main(), so startup() can hand programs the raw block and
 * splitargs() can be exercised independently.  docs/zex-format.md
 * section 5 is the normative splitting description.
 */

#include "zos.h"

struct zos_startup *zos_startup_block;

struct zos_startup *startup(void)
{
	return zos_startup_block;
}

int splitargs(char *tail, char **argv, int max)
{
	char *src;
	char *dst;
	int argc = 0;
	unsigned char quoted = 0;
	unsigned char in_word = 0;

	if (tail == (char *)0 || argv == (char **)0 || max <= 0) {
		return 0;
	}
	src = tail;
	dst = tail;
	for (;;) {
		char c = *src;

		if (c == '\0') {
			break;
		}
		if (c == '"') {
			if (in_word == 0) {
				if (argc < max) {
					argv[argc] = dst;
				}
				++argc;
				in_word = 1;
			}
			quoted = (unsigned char)(quoted == 0);
			++src;
			continue;
		}
		if (quoted == 0 && (c == ' ' || c == '\t')) {
			if (in_word != 0) {
				*dst++ = '\0';
				in_word = 0;
			}
			++src;
			continue;
		}
		if (in_word == 0) {
			if (argc < max) {
				argv[argc] = dst;
			}
			++argc;
			in_word = 1;
		}
		*dst++ = c;
		++src;
	}
	*dst = '\0';
	return argc;
}
