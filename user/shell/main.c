/* KonaOS native shell: the read-run loop.
 *
 * The shell is an ordinary application, not part of the OS: it uses only
 * the public SDK, prints the displayed CWD as its prompt, reads one line
 * with console_readline, parses it with the splitter crt0 runs over every
 * child's tail, and runs the result - a built-in or a program.  An empty
 * line does nothing; every other line leaves its status in the loop.
 */

#include "kos.h"
#include "shell.h"

/* The line-editor buffer: the console front end's capacity. */
#define SHELL_LINE_MAX 128

static char shell_line[SHELL_LINE_MAX];
static char *shell_argv[KOS_ARGV_MAX];

/* The prompt: the displayed CWD, then "> ". */
static void shell_prompt(void)
{
	char cwd[KOS_PATH_MAX];
	char shown[KOS_PATH_MAX];

	if (getcwd(cwd, sizeof cwd) < 0 ||
	    shell_path_display(cwd, shown, sizeof shown) < 0) {
		console_puts("?> ");
		return;
	}
	console_puts(shown);
	console_puts("> ");
}

/* One command: a built-in, or a program through the lookup.  The status
 * is positive in both cases, which is the form the caller keeps. */
static int shell_command(int argc, char **argv)
{
	int status = shell_builtin(argc, argv);

	if (status >= 0) {
		return status;
	}
	return shell_run_command(argc, argv);
}

int main(int argc, char **argv)
{
	int status = 0;

	(void)argc;
	(void)argv;
	shell_path_default();
	/* The display root is /fs; a machine with no card keeps /system,
	 * the store every image carries. */
	if (chdir("/fs") < 0) {
		(void)chdir("/system");
		console_puts("shell: no filesystem, starting in /system\r\n");
	}
	for (;;) {
		int count;
		int length;

		shell_prompt();
		length = console_readline(shell_line, sizeof shell_line);
		if (length < 0) {
			/* The console is gone: leave with its code. */
			status = -length;
			break;
		}
		count = splitargs(shell_line, shell_argv, KOS_ARGV_MAX);
		if (count == 0) {
			continue;               /* an empty line does nothing */
		}
		if (count > KOS_ARGV_MAX) {
			console_puts("shell: too many arguments\r\n");
			status = KOS_EINVAL;
			continue;
		}
		status = shell_command(count, shell_argv);
	}
	return status;
}
