/* KonaOS native shell: the program's own interfaces.
 *
 * The shell is an ordinary SDK application: it includes only "kos.h" and
 * its own headers, and every namespace call it makes is a public SDK
 * call.  One file per part - main.c the loop, parse.c the line and the
 * tail, path.c presentation and translation, lookup.c command lookup and
 * the error table, builtin.c the built-ins.
 */

#ifndef SHELL_H
#define SHELL_H

#include "kos.h"

/* The command path: at most SHELL_PATH_MAX entries of at most
 * KOS_PATH_MAX bytes, stored translated, i.e. in the VFS syntax the
 * namespace takes.  `.` (the current directory), `/fs/BIN` and
 * `/system` are the default. */
#define SHELL_PATH_MAX 4

extern char shell_path[SHELL_PATH_MAX][KOS_PATH_MAX];
extern unsigned char shell_path_count;

/* lookup.c: install the default command path. */
void shell_path_default(void);

/* builtin.c: run `argv[0]` when it names a built-in, or -1 when it names
 * none.  A built-in returns 0 or a positive KOS_E* code and prints its
 * own message; `exit` leaves through exit() and never returns. */
int shell_builtin(int argc, char **argv);

/* banner.c: print the startup banner (the text lines and, where a VIDEO
 * driver is published, the logo dump) once, before the loop. */
void shell_banner(void);

/* fscmd.c: the file built-ins (`ls cat stat cp mv rm mkdir rmdir`), run
 * exactly like `shell_builtin`'s own: 0 or a positive KOS_E* code, or -1
 * when `argv[0]` names none of them. */
int shell_file_command(int argc, char **argv);

/* fscmd.c: the target of `cp`/`mv` - `dst` itself, or `dst` and the
 * source's last component when `directory` (or a trailing slash in
 * `dst`) makes `dst` the directory to copy into.  Returns 0, or
 * -KOS_EINVAL when the result cannot fit `cap`. */
int shell_fscmd_target(const char *src, const char *dst, int directory,
		       char *out, unsigned int cap);

/* lookup.c: a command that is not a built-in: a word with a slash is a
 * path and is translated and run; a bare name is searched for in the
 * command path.  Prints the child's status (or the failed run's message)
 * and returns the positive status. */
int shell_run_command(int argc, char **argv);

/* lookup.c: the native error table (a message per KOS_E* code).  The
 * returned text lives in one static buffer, so print one message per
 * call. */
const char *shell_strerror(int code);

/* path.c: shell syntax to VFS path, and VFS path to displayed path.
 * Both return the length, or -KOS_EINVAL when the result does not fit
 * `cap` bytes. */
int shell_path_translate(const char *path, char *out, unsigned int cap);
int shell_path_display(const char *path, char *out, unsigned int cap);

/* path.c: the reserved root a VFS path sits under, as its name ("fs",
 * "system", "dev"), or NULL when it is not under one (the real
 * namespace root, or a relative path). */
const char *shell_path_root(const char *path);

/* parse.c: rebuild argv[1..] into `out` as one raw tail (words separated
 * by single spaces, quoting only where a word needs it).  Returns the
 * length, or -KOS_EINVAL when the tail would exceed KOS_STARTUP_TAIL_MAX
 * or `cap`. */
int shell_serialize(char **argv, int argc, char *out, unsigned int cap);

#endif /* SHELL_H */
