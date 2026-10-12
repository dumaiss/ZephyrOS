/* KonaOS helloworld demo: the SDK's canonical minimal program.
 *
 * Built by one exe_program line (user/helloworld/Makefile), packed to
 * build/user/hello.exe and optionally booted as /system/hello.exe.  It
 * prints the greeting, then one line per command-tail argument, and
 * exits 0.
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
