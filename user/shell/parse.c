/* KonaOS native shell: the argument tail of an external command.
 *
 * The shell parses its line with the SDK splitter (user/sdk/src/args.c),
 * the one parser crt0 runs over a child's tail too, and rebuilds the
 * tail from argv[1..] with the inverse rules: a word that is empty or
 * holds whitespace, a quote or a backslash is written in double quotes
 * with `"` and `\` escaped, and every other word goes as it is.  The
 * splitter reads that form back word for word, so a child sees exactly
 * the words the shell ran it with.
 */

#include "shell.h"

/* 1 when `word` only survives the splitter inside double quotes. */
static unsigned char tail_needs_quotes(const char *word)
{
	unsigned int index;

	if (word[0] == '\0') {
		return 1;
	}
	for (index = 0; word[index] != '\0'; ++index) {
		char c = word[index];

		if (c == ' ' || c == '\t' || c == '\'' || c == '"' ||
		    c == '\\') {
			return 1;
		}
	}
	return 0;
}

int shell_serialize(char **argv, int argc, char *out, unsigned int cap)
{
	unsigned int length = 0;
	int index;

	for (index = 1; index < argc; ++index) {
		const char *word = argv[index];
		unsigned char quotes = tail_needs_quotes(word);
		unsigned int cost = (index > 1) ? 1u : 0u;
		unsigned int i;

		if (quotes != 0) {
			cost += 2u;
		}
		for (i = 0; word[i] != '\0'; ++i) {
			cost += (quotes != 0 &&
				 (word[i] == '"' || word[i] == '\\'))
				? 2u : 1u;
		}
		if (length + cost > (unsigned int)KOS_STARTUP_TAIL_MAX ||
		    length + cost + 1u > cap) {
			return -KOS_EINVAL;
		}
		if (index > 1) {
			out[length++] = ' ';
		}
		if (quotes == 0) {
			for (i = 0; word[i] != '\0'; ++i) {
				out[length++] = word[i];
			}
			continue;
		}
		out[length++] = '"';
		for (i = 0; word[i] != '\0'; ++i) {
			if (word[i] == '"' || word[i] == '\\') {
				out[length++] = '\\';
			}
			out[length++] = word[i];
		}
		out[length++] = '"';
	}
	if (length + 1u > cap) {
		return -KOS_EINVAL;
	}
	out[length] = '\0';
	return (int)length;
}
