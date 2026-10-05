/* ZephyrOS native startup-block runtime: splitter (user/runtime,
 * package 28b).
 *
 * Docs/zex-format.md section 5 is the normative description of the
 * splitting rules; this file is their only implementation and is runtime
 * policy, not an ABI.  The accessor (zos_args) and the entry glue
 * (zos_args_init) live in zex_args.asm, so a program that only reads the
 * block does not link this file.
 */

#include "zex_args.h"

int zos_split(char *tail, char **argv, int max)
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
