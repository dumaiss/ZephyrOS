/* KonaOS SDK example: the canonical minimal program.
 *
 * Built by one exe_program line (this directory's Makefile) with the SDK
 * alone, packed to build/user/hello.exe and booted as /system/hello.exe
 * by the OS tree's helloworld demo image.  It prints the greeting, then
 * one line per command-tail argument, and exits 0.
 */

#include "kos.h"

int main(int argc, char **argv)
{
	int i;

	printf("Hello, KonaOS!\r\n");
	for (i = 0; i < argc; ++i) {
		printf("%s\r\n", argv[i]);
	}
	return 0;
}
