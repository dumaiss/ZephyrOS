/* ZephyrOS namespace acceptance: the nested-exec child (test artifact).
 *
 * SYS_EXEC starts it from /SYSTEM/nschild.zex while the parent holds bound
 * handles.  Its job is the inheritance rule of architecture 36.13: a child
 * gets *new* entries for the two standard handles and nothing else, so its
 * own numbering starts at ZOS_HANDLE_FIRST_BOUND and a handle number the
 * parent still uses is meaningless here.  It also checks the CWD half of
 * the rule: it starts in the parent's CWD and its own `chdir` moves only
 * its own slot.
 *
 * The lines it prints come through its own ZOS_HANDLE_STDOUT; the replay
 * reads them after the parent's `ns: exec` line.
 */

#include "zos.h"

#include "ns_test.h"
#include "zos/protocol.h"

int main(int argc, char **argv)
{
	char cwd[16];
	unsigned char type = 0u;
	int handle;
	int result;

	(void)argc;
	(void)argv;

	zos_puts("nschild: stdout ok\r\n");

	/* The child inherits the parent's CWD (36.13), and its own `chdir`
	 * goes to its own CWD slot. */
	result = zos_getcwd(cwd, sizeof(cwd));
	if (result != 4 || strcmp(cwd, "/dev") != 0) {
		zos_puts("nschild: FAIL cwd\r\n");
		return 1;
	}
	zos_printf("nschild: cwd %s\r\n", cwd);
	if (zos_chdir("/SYSTEM") != 0) {
		zos_puts("nschild: FAIL chdir\r\n");
		return 1;
	}
	result = zos_getcwd(cwd, sizeof(cwd));
	if (result != 7 || strcmp(cwd, "/SYSTEM") != 0) {
		zos_puts("nschild: FAIL chdir cwd\r\n");
		return 1;
	}
	zos_printf("nschild: cwd %s\r\n", cwd);

	handle = zos_ipc_bind("/dev/console", 0u, &type);
	zos_printf("nschild: /dev/console -> %d handle %d\r\n", type, handle);
	if (handle != (int)ZOS_HANDLE_FIRST_BOUND ||
	    type != ZOS_TYPE_CONSOLE) {
		zos_puts("nschild: FAIL own table\r\n");
		return 1;
	}

	/* A number the parent owns is not in this table. */
	result = zos_ipc_unbind((unsigned char)(handle + 2));
	if (result != -ZOS_EINVAL) {
		zos_puts("nschild: FAIL foreign handle\r\n");
		return 1;
	}

	result = zos_ipc_unbind((unsigned char)handle);
	zos_printf("nschild: unbind %d -> %d\r\n", handle, result);
	zos_puts("nschild: OK\r\n");
	return 0;
}
