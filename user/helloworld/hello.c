/* ZephyrOS helloworld demo: the SDK's canonical minimal program.
 *
 * Built by one zex_program line (user/helloworld/Makefile), packed to
 * build/user/hello.zex and optionally booted as /SYSTEM/hello.zex.  It
 * prints the greeting, then one line per command-tail argument, and
 * exits 0.
 */

#include "zos.h"

int main(int argc, char **argv)
{
	int i;

	zos_printf("Hello, ZephyrOS!\r\n");
	for (i = 0; i < argc; ++i) {
		zos_printf("%s\r\n", argv[i]);
	}
	return 0;
}
