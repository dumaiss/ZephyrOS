/* KonaOS SDK stream acceptance: one session on a card file.
 *
 * The program exercises the stdio subset of `klibc/stdio.h` against a
 * real file and reports one line per outcome:
 *
 *   * a writing session: three lines and a 1 KiB pattern through
 *     `fputs`/`fwrite`, with `ftell` following the logical position;
 *   * an appending session, closed and reopened;
 *   * a reading session: `fgets` for the lines, `fread` for the pattern,
 *     `ftell` after both, `feof` at the end;
 *   * positioning: `fseek` by `SEEK_CUR` and to an offset the stream's
 *     buffer does not hold, `rewind`, and a `fwrite` after a `fseek`
 *     that must land at that offset;
 *   * the whole file read back and compared byte by byte.
 *
 * `tests/test_io_sim.py` replays the transcript and requires every
 * outcome OK, the pinned check count and a zero exit status.  It runs
 * from the monitor's X as `/system/iostream.exe`, needs a writable card
 * and removes `/fs/IOSTREAM.TMP` at the end.
 */

#include "kos.h"

#define PATH "/fs/IOSTREAM.TMP"

/* The file: the three lines of `text`, then `PATTERN_BYTES` of an A..Z
 * pattern, then the appended line. */
static const char text[] = "alpha\nbravo\ncharlie\n";
static const char appended[] = "delta\n";

#define TEXT_BYTES    (sizeof(text) - 1u)
#define PATTERN_BYTES 1024u
#define APPEND_BYTES  (sizeof(appended) - 1u)
#define FILE_BYTES    (TEXT_BYTES + PATTERN_BYTES + APPEND_BYTES)

/* The offset the write-after-seek case lands on, inside the pattern. */
#define SEEK_WRITE_OFFSET 1000ul
/* The same offset inside the pattern itself, and the bytes a write puts
 * there. */
#define SEEK_WRITE_PATTERN (SEEK_WRITE_OFFSET - (unsigned long)TEXT_BYTES)
static const char replacement[] = "xyz";

static unsigned char pattern[PATTERN_BYTES];
static unsigned char scratch[64];

static unsigned int checks;
static unsigned int failures;

static void check(const char *label, int ok)
{
	++checks;
	if (ok == 0) {
		++failures;
	}
	printf("io-stream: %s %s\r\n", label, ok != 0 ? "OK" : "FAIL");
}

/* The byte the file holds at `offset`: the pattern, with the three bytes
 * the write-after-seek case replaces when `overwritten` says so. */
static unsigned char expected(unsigned long offset, int overwritten)
{
	if (offset < (unsigned long)TEXT_BYTES) {
		return (unsigned char)text[offset];
	}
	offset -= (unsigned long)TEXT_BYTES;
	if (offset < (unsigned long)PATTERN_BYTES) {
		if (overwritten != 0 &&
		    offset >= SEEK_WRITE_PATTERN &&
		    offset < SEEK_WRITE_PATTERN + (sizeof(replacement) - 1u)) {
			/* The write-after-seek case overwrote these. */
			return (unsigned char)replacement[offset -
							  SEEK_WRITE_PATTERN];
		}
		return pattern[offset];
	}
	return (unsigned char)appended[offset - (unsigned long)PATTERN_BYTES];
}

/* Write the pattern, append the last line, and read the file back into
 * one stream at a time; every group reports its own outcomes. */
static void write_session(void)
{
	FILE *stream = fopen(PATH, "w");

	if (stream == (FILE *)0) {
		check("fopen-write", 0);
		return;
	}
	check("fopen-write", 1);
	check("fputs-lines", fputs(text, stream) == 0);
	check("fwrite-pattern",
	      fwrite(pattern, 1u, PATTERN_BYTES, stream) == PATTERN_BYTES);
	check("write-ftell",
	      ftell(stream) == (long)(TEXT_BYTES + PATTERN_BYTES));
	check("write-close", fclose(stream) == 0);

	stream = fopen(PATH, "a");
	if (stream == (FILE *)0) {
		check("fopen-append", 0);
		return;
	}
	check("fopen-append", 1);
	check("append-ftell", ftell(stream) == (long)(TEXT_BYTES +
						      PATTERN_BYTES));
	check("append-fputs", fputs(appended, stream) == 0);
	check("append-ftell-end", ftell(stream) == (long)FILE_BYTES);
	check("append-close", fclose(stream) == 0);
}

/* Read the whole stream and compare every byte with `expected`; the
 * streams need no positioning here, the caller seeks first. */
static void read_back(FILE *stream, const char *label, int overwritten)
{
	unsigned long offset = 0ul;
	unsigned int bad = 0u;

	while (offset < (unsigned long)FILE_BYTES) {
		unsigned int got = (unsigned int)fread(scratch, 1u,
						       sizeof(scratch), stream);
		unsigned int i;

		if (got == 0u) {
			break;
		}
		for (i = 0u; i < got; ++i) {
			if (scratch[i] != expected(offset + i,
						  overwritten)) {
				bad = 1u;
			}
		}
		offset += got;
	}
	check(label, bad == 0u && offset == (unsigned long)FILE_BYTES);
}

static void read_session(void)
{
	FILE *stream = fopen(PATH, "r");
	char line[8];
	unsigned int eof = 0u;

	if (stream == (FILE *)0) {
		check("fopen-read", 0);
		return;
	}
	check("fopen-read", 1);

	check("fgets-line", fgets(line, sizeof(line), stream) == line &&
	      line[0] == 'a' && line[5] == '\n');
	check("fgets-ftell", ftell(stream) == 6L);
	check("fgets-second", fgets(line, sizeof(line), stream) == line &&
	      line[0] == 'b');
	check("fread-bytes",
	      fread(scratch, 1u, 8u, stream) == 8u &&
	      scratch[0] == 'c' && scratch[7] == '\n');
	check("read-ftell", ftell(stream) == (long)TEXT_BYTES);

	/* One byte forward: still inside the chunk the stream holds. */
	check("seek-cur", fseek(stream, 1L, SEEK_CUR) == 0 &&
	      ftell(stream) == (long)TEXT_BYTES + 1L);
	check("seek-cur-bytes",
	      fread(scratch, 1u, 8u, stream) == 8u &&
	      scratch[0] == pattern[1] && scratch[7] == pattern[8]);

	/* An absolute seek: the stream must drop its buffer and read the
	 * chunk holding this offset. */
	check("seek-set", fseek(stream, (long)SEEK_WRITE_OFFSET, SEEK_SET) == 0 &&
	      ftell(stream) == (long)SEEK_WRITE_OFFSET);
	check("seek-outside-bytes",
	      fread(scratch, 1u, 16u, stream) == 16u &&
	      scratch[0] == pattern[SEEK_WRITE_OFFSET - TEXT_BYTES] &&
	      scratch[15] == pattern[SEEK_WRITE_OFFSET - TEXT_BYTES + 15ul]);

	/* End of file: a short item, then EOF. */
	check("seek-end", fseek(stream, 0L, SEEK_END) == 0 &&
	      ftell(stream) == (long)FILE_BYTES);
	check("eof-flag", fread(scratch, 1u, 1u, stream) == 0u &&
	      feof(stream) != 0);

	rewind(stream);
	check("rewind", ftell(stream) == 0L && feof(stream) == 0);
	read_back(stream, "read-back-bytes", 0);
	if (fread(scratch, 1u, 1u, stream) == 0u && feof(stream) != 0) {
		eof = 1u;
	}
	check("fread-eof", eof != 0);
	check("fclose-read", fclose(stream) == 0);
}

static void seek_write_session(void)
{
	FILE *stream = fopen(PATH, "a");

	if (stream == (FILE *)0) {
		check("fopen-write-back", 0);
		return;
	}
	check("fopen-write-back", 1);
	check("write-seek", fseek(stream, (long)SEEK_WRITE_OFFSET,
				  SEEK_SET) == 0);
	check("write-after-seek",
	      fwrite(replacement, 1u, sizeof(replacement) - 1u, stream) ==
	      sizeof(replacement) - 1u &&
	      ftell(stream) == (long)SEEK_WRITE_OFFSET +
			       (long)(sizeof(replacement) - 1u));
	check("write-after-seek-close", fclose(stream) == 0);

	stream = fopen(PATH, "r");
	if (stream == (FILE *)0) {
		check("fopen-reread", 0);
		return;
	}
	check("fopen-reread", 1);
	check("reread-seek", fseek(stream, (long)SEEK_WRITE_OFFSET,
				   SEEK_SET) == 0);
	check("reread-bytes",
	      fread(scratch, 1u, 3u, stream) == 3u &&
	      scratch[0] == 'x' && scratch[1] == 'y' && scratch[2] == 'z');
	check("size-unchanged", fseek(stream, 0L, SEEK_END) == 0 &&
	      ftell(stream) == (long)FILE_BYTES);
	rewind(stream);
	read_back(stream, "reread-file-bytes", 1);
	check("fclose-reread", fclose(stream) == 0);
}

int main(void)
{
	unsigned long i;

	for (i = 0ul; i < PATTERN_BYTES; ++i) {
		pattern[i] = (unsigned char)('A' + (char)(i % 26ul));
	}
	console_puts("io-stream: begin\r\n");

	write_session();
	read_session();
	seek_write_session();

	check("unlink", unlink(PATH) == 0);
	printf("io-stream: %u checks, %u failures\r\n", checks, failures);
	console_puts(failures == 0u ? "io-stream: PASS\r\n" :
				      "io-stream: FAIL\r\n");
	return failures == 0u ? 0 : 1;
}
