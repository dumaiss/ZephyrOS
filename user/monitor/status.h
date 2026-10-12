/* KonaOS user runtime: common status/error output.
 *
 * The status vocabulary shared by the monitor and, later, the native
 * diagnostic suite.  Strings are written through the console component;
 * the transfer result is deliberately ignored because a broken console
 * is reported through the program's exit status instead.
 */

#ifndef KONAOS_USER_STATUS_H
#define KONAOS_USER_STATUS_H

/* "OK\r\n" */
void zrt_status_ok(void);

/* "ERR: ARG\r\n" - malformed, missing, oversized, or trailing argument. */
void zrt_status_err_arg(void);

/* "ERR: COMMAND\r\n" - unknown command letter. */
void zrt_status_err_command(void);

/* "ERR: <code>\r\n" - unexpected driver failure, where <code> is the
 * magnitude of the negated native error (two uppercase hex digits). */
void zrt_status_err_code(int status);

#endif /* KONAOS_USER_STATUS_H */
