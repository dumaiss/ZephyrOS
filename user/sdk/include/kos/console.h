/* KonaOS SDK: console entry points as console-driver clients.
 *
 * The line helpers (getchar/console_readline/...) are the reusable console
 * front end the monitor and ZShell share, and they sit on the four
 * driver operations below.  Package 41 removed the SYS_CONSOLE_* syscalls:
 * the console is the resident console driver (the ABI's kos/protocol.h, class
 * KOS_CLASS_CONSOLE), so every function here resolves that endpoint and
 * speaks its protocol.  A program with no console driver (an acceptance
 * image whose store lists none) sees -KOS_ENOENT.
 *
 * Return convention: a non-negative result, or the negated native error
 * code.
 */

#ifndef KOS_SDK_CONSOLE_H
#define KOS_SDK_CONSOLE_H

#include "kos/types.h"
#include "kos/errno.h"
#include "kos/process.h"

/* Line-editor capacity: at most KOS_LINE_MAX characters plus a NUL. */
#define KOS_LINE_MAX 80

/* Terminate the process with `status`; does not return. */
void exit(int status);

/* Console I/O through the console driver.  console_write()/console_read()
 * transfer up to `len` bytes (one descriptor-bearing call) and return the
 * count.  console_read() is a non-blocking drain: it returns 0 when no
 * byte is pending; console_poll() returns 0/1.  putchar() writes one byte
 * without a descriptor and returns that byte.  Errors are negative. */
int console_write(const char *buf, unsigned int len);
int console_read(char *buf, unsigned int len);
int console_poll(void);
int putchar(int ch);

/* Fill `record` with the six-byte v1 information record; returns 6 or a
 * negative error. */
int getinfo(struct kos_info_record *record);

/* Write a NUL-terminated string.  Returns the transferred count or a
 * negative error (the terminator is not sent). */
int console_puts(const char *text);

/* Read one console byte, blocking (polling the non-blocking read) until
 * one arrives: 0-255, or a negative error if the console itself fails. */
int getchar(void);

/* Read one edited line into `buffer` (capacity bytes, including the NUL).
 * Handles BS/DEL editing and CR/LF termination (with CRLF suppression),
 * rings the bell instead of overflowing, and always NUL-terminates.
 * Returns the line length or a negative console error. */
int console_readline(char *buffer, unsigned int capacity);

#endif /* KOS_SDK_CONSOLE_H */
