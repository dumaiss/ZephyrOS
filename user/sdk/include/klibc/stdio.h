/* KonaOS klibc: buffered file streams (the ISO C stdio subset).
 *
 * A program opens a card or store file with `fopen` and reads or writes
 * it through a 512-byte buffer: the FILE is one of a fixed pool of four
 * streams, each with its own buffer, and every call is built on the
 * SDK's `open`/`read`/`write`/`close`/`sync`/`seek` wrappers
 * (kos/fs.h), so the file is a typed handle underneath.
 *
 * The stream calls keep the ISO C signatures and meanings: `fread` and
 * `fwrite` count items, `fgetc` and `fputc` return the byte or EOF, and
 * `feof`/`ferror` report the stream's state.  A call that has no way to
 * report the error through its ISO result returns the negated native
 * error code: `fclose`, `fflush` and `fseek` return 0 on success, the
 * negated code on failure.
 *
 * Buffering and positioning:
 *
 *   - reads pull a 512-byte chunk into the stream's buffer; a read of a
 *     buffer or more goes straight into the caller's memory (one copy);
 *   - writes collect in the buffer and go out when it fills, at
 *     `fclose`, at exit and at `fflush`; a write of a buffer or more
 *     goes out directly after the pending bytes are flushed;
 *   - `fseek` flushes pending writes, discards the read buffer, calls
 *     the SDK `seek` and clears EOF; `ftell` is the logical position -
 *     it accounts for buffered bytes not yet consumed or written;
 *   - `fflush(NULL)` flushes every open stream and the console.
 *
 * A program that never calls `fopen` links none of the stream code and
 * pays nothing for it.  `kos/stream.h` adds the non-ISO measurement
 * helpers (`fmeasure`/`fstats`) and `stream_plan`.
 */

#ifndef KLIBC_STDIO_H
#define KLIBC_STDIO_H

#include "kos/types.h"
#include "kos/errno.h"
#include "kos/fs.h"

/* The platform has a 16-bit address space, so an object size is 16-bit
 * (the ISO type, defined here because --nostdinc brings no stddef.h). */
typedef unsigned int size_t;

/* The stream pool: four streams, one 512-byte buffer each. */
#define KOS_STREAM_MAX 4u
#define KOS_STREAM_BUF_SIZE 512u

/* The end-of-file and error result of the byte calls. */
#ifndef EOF
#define EOF (-1)
#endif

/* The `whence` values of `fseek`: the ISO names for the SDK's own
 * KFS_SEEK_SET/CUR/END. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* One stream.  The layout is SDK-private: a program only holds the
 * pointer `fopen` returned. */
typedef struct kos_stream FILE;

/* Open `path` and return its stream, or NULL: the mode is "r", "w" or
 * "a" with an optional 'b' that is accepted and ignored; the pool being
 * full, a bad mode and a failed open all give NULL. */
FILE *fopen(const char *path, const char *mode);

/* Flush, close and free the stream: 0, or the negated error. */
int fclose(FILE *stream);

/* Read up to `size * nmemb` bytes and return the count of whole items
 * read (< nmemb at end of file). */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
/* Write `size * nmemb` bytes and return the count of whole items
 * written. */
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

/* One byte: 0-255, or EOF at end of file or on error (check `feof` and
 * `ferror`). */
int fgetc(FILE *stream);
int getc(FILE *stream);
int fputc(int c, FILE *stream);
int putc(int c, FILE *stream);

/* One line: at most `size - 1` bytes, the newline kept, always
 * NUL-terminated; NULL at end of file before any byte or on error. */
char *fgets(char *dst, int size, FILE *stream);
/* Write the string without its terminator: 0, or EOF. */
int fputs(const char *src, FILE *stream);

/* Flush pending writes: of one stream, or of every stream and the
 * console when `stream` is NULL.  0, or the negated error. */
int fflush(FILE *stream);

/* Non-zero when the last read hit end of file / when an error was
 * seen. */
int feof(FILE *stream);
int ferror(FILE *stream);

/* Position the stream: `fseek` returns 0 or the negated error,
 * `ftell` the logical position or -1. */
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);

#endif /* KLIBC_STDIO_H */
