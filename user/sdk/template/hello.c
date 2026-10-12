/* KonaOS user SDK template: the smallest program.  Copy this directory
 * (Makefile + this file) next to the unpacked SDK, give the program a
 * name and run `make`:
 *
 *     $(eval $(call exe_program,NAME,.,hello.c,STACK=256))
 *
 * The packed program lands in build/user/NAME.exe; copy it to the card
 * and run it from the monitor with `X /fs/NAME.exe`, or from the shell
 * with `NAME`.  A program needs no entry assembly and no kernel header:
 * crt0 calls main() with the command-tail argv and exits with the
 * returned status.  See docs/sdk.md.
 */

#include "kos.h"

int main(int argc, char **argv)
{
	static const char none[] = "(none)";
	const char *name = none;

	if (argc > 0 && argv[0] != (char *)0) {
		name = argv[0];
	}
	printf("hello from a KonaOS program\r\n");
	printf("argc=%u argv0=%s\r\n", (unsigned int)argc, name);
	return 0;
}
