/* ZephyrOS SDK: console, process-identity and exit wrappers.
 *
 * C-facing wrappers over the stable register ABI (docs/abi.md).  The
 * wrappers own the SDK's shared low-window scratch block, convert a
 * carry-set syscall error to the negated native errno, and never expose
 * SDCC calling conventions.  Return convention: a non-negative result,
 * or the negated native error code.
 *
 * The line helpers (zos_getc/zos_read_line/...) are the reusable console
 * front end the monitor and, later, ZShell share.
 */

#ifndef ZOS_SDK_CONSOLE_H
#define ZOS_SDK_CONSOLE_H

#include "zos/types.h"
#include "zos/errno.h"
#include "zos/process.h"

/* Line-editor capacity: at most ZOS_LINE_MAX characters plus a NUL. */
#define ZOS_LINE_MAX 80

/* Terminate the process with `status`; does not return. */
void zex_exit(unsigned int status);

/* Console services through the bound normal console device.
 * zex_write()/zex_read() transfer up to `len` bytes and return the
 * count.  zex_read() is a non-blocking drain: it returns 0 when no byte
 * is pending; zex_poll() returns 0/1.  Errors are negative. */
int zex_write(const char *buf, unsigned int len);
int zex_read(char *buf, unsigned int len);
int zex_poll(void);

/* Fill `record` with the six-byte v1 information record; returns 6 or a
 * negative error. */
int zex_getinfo(struct zos_info_record *record);

/* Write one byte / a buffer / a NUL-terminated string.  Returns the
 * transferred count or a negative error (the terminator is not sent). */
int zos_putc(char ch);
int zos_console_write(const char *buf, unsigned int len);
int zos_puts(const char *text);

/* Read one console byte, blocking (polling the non-blocking read) until
 * one arrives: 0-255, or a negative error if the console itself fails. */
int zos_getc(void);

/* Read one edited line into `buffer` (capacity bytes, including the NUL).
 * Handles BS/DEL editing and CR/LF termination (with CRLF suppression),
 * rings the bell instead of overflowing, and always NUL-terminates.
 * Returns the line length or a negative console error. */
int zos_read_line(char *buffer, unsigned int capacity);

#endif /* ZOS_SDK_CONSOLE_H */
