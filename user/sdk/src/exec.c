/* KonaOS libkos: synchronous SYS_EXEC wrapper.
 *
 * Shares the namespace wrappers' argument block and gate stub, so the
 * string-length rule and the negated-errno convention are identical.  The
 * kernel does the binding (it opens the image as a FILE handle with
 * `bind`, streams it through FILE calls and unbinds before launch), so
 * there is no client-side file work here.
 * `args` is the raw command tail: the kernel copies it byte-exact into
 * the child's startup block and never splits or quotes it.
 */

#include "kos.h"
#include "kos/syscall.h"

#include "kos_internal.h"

/* Raw argument-tail length: 0 for null/empty, the byte count, or 0xFFFF
 * when it exceeds KOS_STARTUP_TAIL_MAX. */
static unsigned int exeec_tail_len(const char *args)
{
	unsigned int n = 0;

	if (args == (const char *)0 || args[0] == '\0') {
		return 0u;
	}
	while (args[n] != '\0') {
		if (n >= (unsigned int)KOS_STARTUP_TAIL_MAX) {
			return 0xFFFFu;
		}
		++n;
	}
	return n;
}

static unsigned int exeec_path_len(const char *path)
{
	unsigned int n = 0;

	if (path == (const char *)0) {
		return 0xFFFFu;
	}
	while (path[n] != '\0') {
		if (n >= (unsigned int)(KOS_PATH_MAX - 1u)) {
			return 0xFFFFu;
		}
		++n;
	}
	if (n == 0u) {
		return 0xFFFFu;
	}
	return n;
}

int run(const char *path, const char *args)
{
	unsigned int n = exeec_path_len(path);
	unsigned int an = exeec_tail_len(args);
	if (n == 0xFFFFu || an == 0xFFFFu) {
		return -KOS_EINVAL;
	}
	/* The child starts with its own console handles and its own output
	 * buffer, so anything this program has written must be on the
	 * console first: a child never prints ahead of its parent. */
	(void)console_flush();
	kos_scratch.exec.path = (unsigned int)path;
	kos_scratch.exec.length = n;
	kos_scratch.exec.args = (unsigned int)args;
	kos_scratch.exec.args_length = an;
	return kos_ipc_raw(KOS_SYSCALL_EXEC, &kos_scratch.exec);
}
