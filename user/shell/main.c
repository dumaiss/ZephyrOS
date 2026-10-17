/* KonaOS native shell: the read-run loop.
 *
 * The shell is an ordinary application, not part of the OS: it uses only
 * the public SDK, prints the displayed CWD as its prompt, reads one line
 * with its line editor (console_readline's editing, plus CTRL-L and a
 * command history), parses it with the splitter crt0 runs over every
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

/* --- line editor --------------------------------------------------------
 *
 * console_readline's editing (backspace, CR/LF, the bell when the line is
 * full, other control bytes ignored), plus what an interactive shell
 * wants: CTRL-L clears the screen and redraws the prompt and the line so
 * far, and the up and down arrows (VT100 ESC [ A / ESC [ B, as the
 * keyboard sends them) walk the last SHELL_HISTORY lines; down past the
 * newest gives an empty line.  Other escape sequences are swallowed.  The
 * history lasts for the session.
 */
#define SHELL_HISTORY 5

#define SHELL_KEY_BS  0x08
#define SHELL_KEY_LF  0x0A
#define SHELL_KEY_FF  0x0C /* CTRL-L */
#define SHELL_KEY_CR  0x0D
#define SHELL_KEY_ESC 0x1B
#define SHELL_KEY_DEL 0x7F

static char shell_history_store[SHELL_HISTORY][SHELL_LINE_MAX];
/* The store through a pointer captured at start: the packer refuses an
 * address computed from a static array and a run-time index. */
static char *shell_history;
static unsigned char shell_history_count; /* lines kept, 0..SHELL_HISTORY */
static unsigned char shell_history_next;  /* the slot the next line takes */

/* History line `back` (1 = the newest). */
static char *shell_history_line(unsigned char back)
{
	unsigned char slot = (unsigned char)(shell_history_next +
					     SHELL_HISTORY - back);

	if (slot >= SHELL_HISTORY) {
		slot = (unsigned char)(slot - SHELL_HISTORY);
	}
	return shell_history + (unsigned int)slot * SHELL_LINE_MAX;
}

static void shell_history_add(const char *line)
{
	char *slot;

	if (shell_history_count != 0u &&
	    strcmp(shell_history_line(1u), line) == 0) {
		return; /* the same line again is kept once */
	}
	slot = shell_history + (unsigned int)shell_history_next * SHELL_LINE_MAX;
	(void)strlcpy(slot, line, SHELL_LINE_MAX);
	if (++shell_history_next == SHELL_HISTORY) {
		shell_history_next = 0u;
	}
	if (shell_history_count < SHELL_HISTORY) {
		++shell_history_count;
	}
}

/* Replace the line on screen and in `buffer` with `text`. */
static void shell_line_show(char *buffer, unsigned int *length,
			    unsigned int capacity, const char *text)
{
	while (*length != 0u) {
		console_write("\b \b", 3u);
		--*length;
	}
	while (*text != '\0' && *length < capacity - 1u) {
		buffer[*length] = *text;
		++*length;
		++text;
	}
	console_write(buffer, *length);
}

static int shell_readline(char *buffer, unsigned int capacity)
{
	static unsigned char cr_pending; /* CR consumed: drop a CRLF's LF */
	unsigned int length = 0u;
	unsigned char back = 0u;   /* history lines up from the typed one */
	unsigned char esc = 0u;    /* 1 after ESC, 2 inside ESC [ ... */

	for (;;) {
		int ch = getchar();
		unsigned char byte;

		if (ch < 0) {
			return ch;
		}
		byte = (unsigned char)ch;
		if (esc == 2u) {
			if ((byte >= '0' && byte <= '9') || byte == ';') {
				continue; /* a parameter: wait for the final */
			}
			esc = 0u;
			if (byte == 'A' && back < shell_history_count) {
				++back;
				shell_line_show(buffer, &length, capacity,
						shell_history_line(back));
			} else if (byte == 'B' && back != 0u) {
				--back;
				shell_line_show(buffer, &length, capacity,
						(back != 0u)
						? (const char *)shell_history_line(back)
						: "");
			}
			continue;
		}
		if (esc == 1u) {
			esc = 0u;
			if (byte == '[') {
				esc = 2u;
				continue;
			}
		}
		if (byte == SHELL_KEY_ESC) {
			esc = 1u;
			continue;
		}
		if (byte == SHELL_KEY_LF && cr_pending != 0u && length == 0u) {
			cr_pending = 0u; /* second half of the previous CRLF */
			continue;
		}
		cr_pending = 0u;
		if (byte == SHELL_KEY_CR) {
			cr_pending = 1u;
			break;
		}
		if (byte == SHELL_KEY_LF) {
			break;
		}
		if (byte == SHELL_KEY_FF) {
			console_write("\f", 1u);
			shell_prompt();
			console_write(buffer, length);
			continue;
		}
		if (byte == SHELL_KEY_BS || byte == SHELL_KEY_DEL) {
			if (length != 0u) {
				--length;
				console_write("\b \b", 3u);
			}
			continue;
		}
		if (byte < 0x20u) {
			continue; /* other control bytes are ignored */
		}
		if (length >= capacity - 1u) {
			putchar(0x07); /* BEL: the line is full */
			continue;
		}
		buffer[length] = (char)byte;
		++length;
		putchar((int)byte);
	}
	buffer[length] = '\0';
	console_write("\r\n", 2u);
	if (length != 0u) {
		shell_history_add(buffer);
	}
	return (int)length;
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
	shell_history = &shell_history_store[0][0];
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
		length = shell_readline(shell_line, sizeof shell_line);
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
