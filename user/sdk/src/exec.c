/* ZephyrOS libzos: synchronous SYS_EXEC wrapper.
 *
 * Shares the filesystem wrappers' argument block and gate stub, so the
 * string-length rule and the negated-errno convention are identical.
 * `args` is the raw command tail: the kernel copies it byte-exact into
 * the child's startup block and never splits or quotes it.
 */

#include "zos.h"
#include "zos_internal.h"

/* Raw argument-tail length: 0 for null/empty, the byte count, or 0xFFFF
 * when it exceeds ZOS_STARTUP_TAIL_MAX. */
static unsigned int zexec_tail_len(const char *args)
{
	unsigned int n = 0;

	if (args == (const char *)0 || args[0] == '\0') {
		return 0u;
	}
	while (args[n] != '\0') {
		if (n >= (unsigned int)ZOS_STARTUP_TAIL_MAX) {
			return 0xFFFFu;
		}
		++n;
	}
	return n;
}

static unsigned int zexec_path_len(const char *path)
{
	unsigned int n = 0;

	if (path == (const char *)0) {
		return 0xFFFFu;
	}
	while (path[n] != '\0') {
		if (n >= (unsigned int)(ZOS_PATH_MAX - 1u)) {
			return 0xFFFFu;
		}
		++n;
	}
	if (n == 0u) {
		return 0xFFFFu;
	}
	return n;
}

int zos_exec(const char *path, const char *args)
{
	unsigned int n = zexec_path_len(path);
	unsigned int an = zexec_tail_len(args);
	struct zos_fs_call_req req;

	if (n == 0xFFFFu || an == 0xFFFFu) {
		return -ZOS_EINVAL;
	}
	zos_scratch.exec.path = (unsigned int)path;
	zos_scratch.exec.length = n;
	zos_scratch.exec.args = (unsigned int)args;
	zos_scratch.exec.args_length = an;
	req.number = (unsigned char)ZOS_SYSCALL_EXEC;
	req.block = &zos_scratch.exec;
	return zos_fs_call(&req);
}
