/* KonaOS native shell: command lookup, run and the error table.
 *
 * The kernel has no command path, so the shell owns the policy: a word
 * with a slash names a path (translated, then run); any other word is
 * searched for in the command path, first as `<dir>/<name>` and then -
 * unless the name already ends in `.exe` - as `<dir>/<name>.exe`.  A
 * missing file or a missing card moves on to the next candidate; any
 * other run result is final.  The run result is reported here too
 * (requirement 8): a failed run prints `<name>: <message>` and returns
 * the positive code, a child that exits non-zero prints `[exit <n>]`.
 */

#include "shell.h"

char shell_path[SHELL_PATH_MAX][KOS_PATH_MAX];
unsigned char shell_path_count;

static char lookup_tail[KOS_STARTUP_TAIL_MAX + 1u];

void shell_path_default(void)
{
	(void)strlcpy(shell_path[0], ".", KOS_PATH_MAX);
	(void)strlcpy(shell_path[1], "/fs/BIN", KOS_PATH_MAX);
	(void)strlcpy(shell_path[2], "/system", KOS_PATH_MAX);
	shell_path_count = 3u;
}

const char *shell_strerror(int code)
{
	static char unknown[12];

	switch (code) {
	case KOS_ENOSYS:
		return "not implemented";
	case KOS_EINVAL:
		return "invalid argument";
	case KOS_ENODEV:
		return "no device";
	case KOS_EBUSY:
		return "busy";
	case KOS_ENOENT:
		return "not found";
	case KOS_ENOSPC:
		return "no space";
	case KOS_ENOTSUP:
		return "not supported";
	case KOS_ETIMEDOUT:
		return "timed out";
	case KOS_EAGAIN:
		return "try again";
	case KOS_EIO:
		return "I/O error";
	case KOS_EINIT:
		return "init refused";
	case KOS_EFAULT:
		return "bad program";
	case KOS_EEXIST:
		return "already exists";
	case KOS_ENOTDIR:
		return "not a directory";
	case KOS_EISDIR:
		return "is a directory";
	case KOS_ENOTEMPTY:
		return "not empty";
	case KOS_EROFS:
		return "read-only";
	case KOS_ESTALE:
		return "stale";
	case KOS_EXDEV:
		return "cross-device";
	case KOS_EMFILE:
		return "too many open";
	default:
		(void)snprintf(unknown, sizeof unknown, "error %02x", code);
		return unknown;
	}
}

/* Print what a run result means and return the positive status. */
static int lookup_report(const char *name, int status)
{
	if (status < 0) {
		printf("%s: %s\r\n", name, shell_strerror(-status));
		return -status;
	}
	if (status != 0) {
		printf("[exit %u]\r\n", (unsigned int)status);
	}
	return status;
}

/* 1 when `name` ends in `.exe`, matched case-insensitively. */
static unsigned char lookup_is_exe(const char *name)
{
	unsigned int length = strlen(name);
	char fold[4];
	unsigned int i;

	if (length < 4u) {
		return 0;
	}
	for (i = 0; i < 4u; ++i) {
		char c = name[length - 4u + i];

		fold[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
	}
	return (unsigned char)(fold[0] == '.' && fold[1] == 'e' &&
			       fold[2] == 'x' && fold[3] == 'e');
}

/* <dir>/<name> into `out`; the length, or -KOS_EINVAL when it cannot
 * fit `cap` bytes. */
static int lookup_join(const char *dir, const char *name, char *out,
		       unsigned int cap)
{
	unsigned int needed = strlen(dir) + 1u + strlen(name);

	if (needed + 1u > cap) {
		return -KOS_EINVAL;
	}
	(void)strlcpy(out, dir, cap);
	(void)strlcat(out, "/", cap);
	(void)strlcat(out, name, cap);
	return (int)needed;
}

int shell_run_command(int argc, char **argv)
{
	const char *name = argv[0];
	char candidate[KOS_PATH_MAX];
	char target[KOS_PATH_MAX];
	unsigned char path;

	if (argc > 1) {
		if (shell_serialize(argv, argc, lookup_tail,
				    sizeof lookup_tail) < 0) {
			console_puts("shell: argument list too long\r\n");
			return KOS_EINVAL;
		}
	} else {
		lookup_tail[0] = '\0';
	}

	if (strchr(name, '/') != (char *)0) {
		if (shell_path_translate(name, target, sizeof target) < 0) {
			printf("%s: %s\r\n", name,
			       shell_strerror(KOS_EINVAL));
			return KOS_EINVAL;
		}
		return lookup_report(name, run(target, lookup_tail));
	}

	for (path = 0; path < shell_path_count; ++path) {
		int length;
		int status;

		length = lookup_join(shell_path[path], name, candidate,
				     sizeof candidate);
		if (length < 0) {
			continue;
		}
		status = run(candidate, lookup_tail);
		if (status == -KOS_ENOENT || status == -KOS_ENODEV) {
			if (lookup_is_exe(name) ||
			    (unsigned int)length + 4u >= sizeof candidate) {
				continue;
			}
			(void)strlcat(candidate, ".exe", sizeof candidate);
			status = run(candidate, lookup_tail);
		}
		if (status != -KOS_ENOENT && status != -KOS_ENODEV) {
			return lookup_report(name, status);
		}
	}
	printf("%s: command not found\r\n", name);
	return KOS_ENOENT;
}
