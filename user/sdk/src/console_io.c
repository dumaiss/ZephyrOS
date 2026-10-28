/* KonaOS libkos: console byte/string I/O and bounded line editing.
 *
 * All ordinary I/O goes through the console-driver wrappers
 * (console_proto.c); nothing here knows which driver a byte came from
 * or went to - the console driver composes that.  The output buffer and
 * its flush live there too.  The reads (getchar, console_readline) are
 * console_line.c.  See kos/console.h.
 */

#include "kos.h"

int console_puts(const char *text)
{
	const char *scan = text;
	unsigned int len = 0;

	while (*scan != '\0') {
		++scan;
		++len;
	}
	if (len == 0) {
		return 0;
	}
	return console_write(text, len);
}
