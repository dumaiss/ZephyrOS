/* KonaOS namespace acceptance: the nested-exec child (test artifact).
 *
 * SYS_EXEC starts it from /system/nschild.exe while the parent holds bound
 * handles.  Its job is the inheritance rule of architecture 4.10: a child
 * gets *new* entries for the two standard handles and nothing else, so its
 * own numbering starts at KOS_HANDLE_FIRST_BOUND and a handle number the
 * parent still uses is meaningless here.  It also checks the CWD half of
 * the rule: it starts in the parent's CWD and its own `chdir` moves only
 * its own slot.
 *
 * The lines it prints come through its own KOS_HANDLE_STDOUT; the replay
 * reads them after the parent's `ns: exec` line.
 */

#include "kos.h"

#include "ns_test.h"
#include "kos/protocol.h"

int main(int argc, char **argv)
{
	char cwd[16];
	unsigned char type = 0u;
	int handle;
	int result;

	(void)argc;
	(void)argv;

	console_puts("nschild: stdout ok\r\n");

	/* The child inherits the parent's CWD (4.10), and its own `chdir`
	 * goes to its own CWD slot. */
	result = getcwd(cwd, sizeof(cwd));
	if (result != 4 || strcmp(cwd, "/dev") != 0) {
		console_puts("nschild: FAIL cwd\r\n");
		return 1;
	}
	printf("nschild: cwd %s\r\n", cwd);
	if (chdir("/system") != 0) {
		console_puts("nschild: FAIL chdir\r\n");
		return 1;
	}
	result = getcwd(cwd, sizeof(cwd));
	if (result != 7 || strcmp(cwd, "/system") != 0) {
		console_puts("nschild: FAIL chdir cwd\r\n");
		return 1;
	}
	printf("nschild: cwd %s\r\n", cwd);

	handle = bind("/dev/console", 0u, &type);
	printf("nschild: /dev/console -> %d handle %d\r\n", type, handle);
	if (handle != (int)KOS_HANDLE_FIRST_BOUND ||
	    type != KOS_TYPE_CONSOLE) {
		console_puts("nschild: FAIL own table\r\n");
		return 1;
	}

	/* A number the parent owns is not in this table. */
	result = unbind((unsigned char)(handle + 2));
	if (result != -KOS_EINVAL) {
		console_puts("nschild: FAIL foreign handle\r\n");
		return 1;
	}

	result = unbind((unsigned char)handle);
	printf("nschild: unbind %d -> %d\r\n", handle, result);
	console_puts("nschild: OK\r\n");
	return 0;
}
