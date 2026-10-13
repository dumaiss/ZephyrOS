/* KonaOS native monitor: the X (execute) command.
 *
 * X <path> [tail] runs an ordinary EXE from the recovery store through
 * the SDK's synchronous SYS_EXEC wrapper, prints the child's exit status
 * and returns to the prompt.  This is the launcher's own execution path:
 * no new privilege, no kernel or driver state, and the monitor's
 * standard console handles pass to the child unchanged.
 *
 * A card executable works because SYS_EXEC stages the caller's path in
 * the VFS workspace's path view in the fixed core: a path in the cold
 * half would be hidden behind the bank-6 window the `/fs` provider runs
 * in (the buffer rule, architecture 4.2).
 *
 * The tail travels to the child byte-exact (the child splits it; see
 * docs/exe-format.md section 6.1), which is how the `stage` option reaches
 * the outcome programs the exerciser starts.
 *
 * Argument problems report the monitor's ERR: ARG, a failed exec reports
 * the native code (e.g. ERR: 05 for an absent object), and a normal exit
 * prints the child's u16 status.
 */

#include "monitor.h"
#include "kos.h"
#include "hexfmt.h"
#include "status.h"

static unsigned int mon_count(const char *text)
{
	unsigned int length = 0u;

	while (text[length] != '\0') {
		++length;
	}
	return length;
}

void mon_cmd_exec(char *cursor)
{
	char *path;
	char *tail;
	unsigned int length;
	int status;

	zrt_skip_spaces(&cursor);
	path = cursor;
	while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
		++cursor;
	}
	length = (unsigned int)(cursor - path);
	if (length == 0u || length > (unsigned int)(KOS_PATH_MAX - 1u)) {
		zrt_status_err_arg();
		return;
	}
	if (*cursor != '\0') {
		*cursor = '\0';
		++cursor;
	}
	zrt_skip_spaces(&cursor);
	tail = cursor;
	if (mon_count(tail) > (unsigned int)KOS_STARTUP_TAIL_MAX) {
		zrt_status_err_arg();
		return;
	}

	status = run(path, tail);
	if (status < 0) {
		zrt_status_err_code(status);
		return;
	}
	console_puts("exec: exit status 0x");
	mon_put_hex16((unsigned int)status);
	console_puts("\r\n");
}
