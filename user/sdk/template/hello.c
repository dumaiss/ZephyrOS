/* KonaOS user SDK template: copy this file (or this directory) and add
 * one build line where the top-level Makefile includes the SDK rule:
 *
 *     $(eval $(call exe_program,NAME,DIR,hello.c,STACK=256))
 *
 * `DIR` is the directory holding the copy; the packed program lands in
 * build/user/NAME.exe.  A program needs no entry assembly and no kernel
 * header: crt0 calls main() with the command-tail argv and exits with the
 * returned status.  See docs/sdk.md.
 */

#include "kos.h"

int main(int argc, char **argv)
{
	printf("hello from a KonaOS program\r\n");
	printf("argc=%u argv0=%s\r\n",
		   (unsigned int)argc,
		   argc > 0 ? argv[0] : "(none)");
	return 0;
}
