/* KonaOS native shell: the built-ins.
 *
 * `cd pwd echo path help exit` run in the shell's own process.  A
 * built-in returns 0 or a positive KOS_E* code and prints
 * `<builtin>: <path>: <message>` when it fails, and `usage: <synopsis>`
 * for a wrong argument count; `exit` leaves through exit() and never
 * returns.  The command path `path` edits is stored translated, exactly
 * the form the lookup joins its candidates from.
 */

#include "shell.h"

static int builtin_usage(const char *synopsis)
{
	printf("usage: %s\r\n", synopsis);
	return KOS_EINVAL;
}

static int builtin_cd(int argc, char **argv)
{
	char target[KOS_PATH_MAX];
	const char *shown = "/";
	int status;

	if (argc > 2) {
		return builtin_usage("cd [dir]");
	}
	if (argc == 2) {
		shown = argv[1];
		if (shell_path_translate(argv[1], target, sizeof target) < 0) {
			printf("cd: %s: %s\r\n", shown,
			       shell_strerror(KOS_EINVAL));
			return KOS_EINVAL;
		}
	} else {
		/* No argument: the display root, which lives at /fs. */
		(void)strlcpy(target, "/fs", sizeof target);
	}
	status = chdir(target);
	if (status < 0) {
		printf("cd: %s: %s\r\n", shown, shell_strerror(-status));
		return -status;
	}
	return 0;
}

static int builtin_pwd(int argc, char **argv)
{
	char cwd[KOS_PATH_MAX];
	char shown[KOS_PATH_MAX];
	int length;

	(void)argv;
	if (argc > 1) {
		return builtin_usage("pwd");
	}
	length = getcwd(cwd, sizeof cwd);
	if (length < 0) {
		printf("pwd: %s\r\n", shell_strerror(-length));
		return -length;
	}
	if (shell_path_display(cwd, shown, sizeof shown) < 0) {
		printf("pwd: %s\r\n", shell_strerror(KOS_EINVAL));
		return KOS_EINVAL;
	}
	printf("%s\r\n", shown);
	return 0;
}

static int builtin_echo(int argc, char **argv)
{
	int index;

	for (index = 1; index < argc; ++index) {
		if (index > 1) {
			console_puts(" ");
		}
		console_puts(argv[index]);
	}
	console_puts("\r\n");
	return 0;
}

/* The translated entries of a `path` edit, applied only when every one
 * of them fits. */
static char builtin_path_table[SHELL_PATH_MAX][KOS_PATH_MAX];

static int builtin_path_show(void)
{
	char shown[KOS_PATH_MAX];
	unsigned char index;

	for (index = 0; index < shell_path_count; ++index) {
		if (shell_path_display(shell_path[index], shown,
				       sizeof shown) < 0) {
			continue;
		}
		printf("%s\r\n", shown);
	}
	return 0;
}

static int builtin_path_set(int count, char **words)
{
	unsigned char index;

	if (count > SHELL_PATH_MAX) {
		return builtin_usage("path [dir ...]");
	}
	for (index = 0; index < count; ++index) {
		if (shell_path_translate(words[index], builtin_path_table[index],
					 KOS_PATH_MAX) < 0) {
			printf("path: %s: %s\r\n", words[index],
			       shell_strerror(KOS_EINVAL));
			return KOS_EINVAL;
		}
	}
	for (index = 0; index < count; ++index) {
		(void)strlcpy(shell_path[index], builtin_path_table[index],
			      KOS_PATH_MAX);
	}
	shell_path_count = (unsigned char)count;
	return 0;
}

static int builtin_path(int argc, char **argv)
{
	if (argc == 1) {
		return builtin_path_show();
	}
	return builtin_path_set(argc - 1, argv + 1);
}

static int builtin_help(int argc, char **argv)
{
	(void)argv;
	if (argc > 1) {
		return builtin_usage("help");
	}
	console_puts("built-ins: cd pwd echo path help exit reboot"
		     " ls cat stat cp mv rm mkdir rmdir\r\n");
	return 0;
}

/* 1 when `text` is a decimal exit status (0..65535); the value goes to
 * `out`. */
static unsigned char builtin_status_parse(const char *text,
					  unsigned int *out)
{
	unsigned int value = 0;
	unsigned int index;

	if (text[0] == '\0') {
		return 0;
	}
	for (index = 0; text[index] != '\0'; ++index) {
		unsigned int digit;

		if (text[index] < '0' || text[index] > '9') {
			return 0;
		}
		digit = (unsigned int)(text[index] - '0');
		/* 65535 is the largest status: refuse anything above it
		 * before the 16-bit value wraps. */
		if (value > 6553u || (value == 6553u && digit > 5u)) {
			return 0;
		}
		value = value * 10u + digit;
	}
	*out = value;
	return 1;
}

static int builtin_reboot(int argc, char **argv)
{
	(void)argv;
	if (argc > 1) {
		return builtin_usage("reboot");
	}
	reboot();
	return 0;
}

static int builtin_exit(int argc, char **argv)
{
	unsigned int status = 0;

	if (argc > 2) {
		return builtin_usage("exit [n]");
	}
	if (argc == 2 && builtin_status_parse(argv[1], &status) == 0) {
		printf("exit: %s: %s\r\n", argv[1],
		       shell_strerror(KOS_EINVAL));
		return KOS_EINVAL;
	}
	exit((int)status);
	return 0;
}

int shell_builtin(int argc, char **argv)
{
	if (strcmp(argv[0], "cd") == 0) {
		return builtin_cd(argc, argv);
	}
	if (strcmp(argv[0], "pwd") == 0) {
		return builtin_pwd(argc, argv);
	}
	if (strcmp(argv[0], "echo") == 0) {
		return builtin_echo(argc, argv);
	}
	if (strcmp(argv[0], "path") == 0) {
		return builtin_path(argc, argv);
	}
	if (strcmp(argv[0], "help") == 0) {
		return builtin_help(argc, argv);
	}
	if (strcmp(argv[0], "reboot") == 0) {
		return builtin_reboot(argc, argv);
	}
	if (strcmp(argv[0], "exit") == 0) {
		return builtin_exit(argc, argv);
	}
	/* The file built-ins are the same kind of command to everything
	 * outside this file. */
	return shell_file_command(argc, argv);
}
