/* ZephyrOS user runtime: console byte/string I/O and bounded line editing.
 *
 * Part of the reusable `user/runtime/` support linked into native ZEX
 * programs (never into the kernel).  All ordinary I/O goes through the
 * SYS_CONSOLE_* wrappers in zex_runtime.h; nothing here knows which
 * device currently backs the console.
 *
 * Conventions: transfer/length results are >= 0, errors are the negated
 * native errno.  `zrt_getc()` blocks (polling the non-blocking syscall)
 * and returns 0-255.  `zrt_read_line()` echoes typed characters, handles
 * BS/DEL editing and CR/LF termination (with CRLF suppression), rings
 * the terminal bell instead of overflowing the buffer, and always
 * returns a NUL-terminated line without the terminator.
 */

#ifndef ZEPHYROS_USER_CONSOLE_IO_H
#define ZEPHYROS_USER_CONSOLE_IO_H

/* Line-editor capacity: at most ZRT_LINE_MAX characters plus a NUL.
 * The M7 monitor contract limits command lines to 80 characters. */
#define ZRT_LINE_MAX 80

/* Write one byte / a buffer.  Returns the transferred count or a
 * negative error. */
int zrt_putc(char ch);
int zrt_write(const char *buf, unsigned int len);

/* Write a NUL-terminated string (the terminator is not sent). */
int zrt_puts(const char *text);

/* Read one console byte, blocking until one arrives.  0-255, or a
 * negative error if the console itself fails. */
int zrt_getc(void);
/* Read one edited line into `buffer` (capacity bytes, including the
 * NUL).  Returns the line length (0-79 for an 80-byte buffer) or a
 * negative console error.  Characters beyond capacity-1 are refused
 * with a bell; the next valid line continues normally. */
int zrt_read_line(char *buffer, unsigned int capacity);

#endif /* ZEPHYROS_USER_CONSOLE_IO_H */
