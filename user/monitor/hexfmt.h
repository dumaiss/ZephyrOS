/* KonaOS user runtime: hexadecimal parsing and formatting.
 *
 * Reusable support for native diagnostic programs.  Parsing mirrors the
 * familiar monitor behavior: leading spaces/tabs are skipped, at most
 * four hex digits are accepted for a word, malformed or oversized input
 * is rejected without partially updating the caller's cursor, and a
 * byte parse additionally rejects values above FFh.  Formatting emits
 * uppercase digits into a caller-supplied buffer.
 */

#ifndef KONAOS_USER_HEXFMT_H
#define KONAOS_USER_HEXFMT_H

/* Skip spaces and tabs at *cursor. */
void zrt_skip_spaces(char **cursor);

/* Parse 1-4 hex digits (case-insensitive) into *value and advance the
 * cursor.  Returns 1 on success, 0 when no digits or more than four
 * were present. */
unsigned char zrt_parse_hex(char **cursor, unsigned int *value);

/* Parse a byte: like zrt_parse_hex but rejects values above FFh. */
unsigned char zrt_parse_hex8(char **cursor, unsigned int *value);

/* Skip spaces and report whether the line is exhausted (1) or more
 * trailing text remains (0). */
unsigned char zrt_line_end(char **cursor);

/* Write two uppercase hex digits + NUL / four digits + NUL. */
void zrt_fmt_hex8(char *dst, unsigned int value);
void zrt_fmt_hex16(char *dst, unsigned int value);

#endif /* KONAOS_USER_HEXFMT_H */
