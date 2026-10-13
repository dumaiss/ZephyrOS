/* KonaOS libkos: startup-block accessor and command-tail splitter.
 *
 * crt0 stores HL (the startup block) into kos_startup_block before it
 * calls main(), so startup() can hand programs the raw block and
 * splitargs() can be exercised independently.  The splitter is the one
 * parser shared by crt0 (every program's tail) and the native shell
 * (its command line), so a child's argv always agrees with the words
 * the shell ran it with.  docs/exe-format.md section 5 is the normative
 * splitting description.
 */

#include "kos.h"

struct kos_startup *kos_startup_block;

struct kos_startup *startup(void)
{
	return kos_startup_block;
}

/* The splitter's quoting states. */
#define SPLIT_PLAIN  0u
#define SPLIT_DOUBLE 1u
#define SPLIT_SINGLE 2u

int splitargs(char *tail, char **argv, int max)
{
	char *src;
	char *dst;
	int argc = 0;
	unsigned char quote = SPLIT_PLAIN;
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
		if (quote == SPLIT_SINGLE) {
			/* Single quotes: everything inside is literal. */
			if (c == '\'') {
				quote = SPLIT_PLAIN;
				++src;
				continue;
			}
			*dst++ = c;
			++src;
			continue;
		}
		if (quote == SPLIT_DOUBLE) {
			/* Double quotes: only \" and \\ are escapes. */
			if (c == '"') {
				quote = SPLIT_PLAIN;
				++src;
				continue;
			}
			if (c == '\\' &&
			    (src[1] == '"' || src[1] == '\\')) {
				c = src[1];
				++src;
			}
			*dst++ = c;
			++src;
			continue;
		}
		if (c == ' ' || c == '\t') {
			if (in_word != 0) {
				*dst++ = '\0';
				in_word = 0;
			}
			++src;
			continue;
		}
		if (c == '\'' || c == '"') {
			if (in_word == 0) {
				if (argc < max) {
					argv[argc] = dst;
				}
				++argc;
				in_word = 1;
			}
			quote = (c == '\'') ? SPLIT_SINGLE : SPLIT_DOUBLE;
			++src;
			continue;
		}
		if (c == '\\') {
			/* Outside quotes, \x is a literal x; a trailing
			 * backslash stands for itself. */
			if (src[1] != '\0') {
				c = src[1];
				++src;
			}
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
