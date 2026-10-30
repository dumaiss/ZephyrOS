/* ZTRCheck for KonaOS: the ZTR structural validator.
 *
 * libsnd's open trusts the stamped content CRC; `ztrcheck FILE.ZTR` runs
 * the full structural validation over a kos_reader_t on the open file and
 * prints either one OK summary or the first defect.  The checks live in
 * ztrcheck_core.c, shared with the host twin the tests drive.
 */

#include "kos.h"
#include "kos/reader.h"
#include "ztrcheck_core.h"

static uint8_t scratch[2048];

static void usage(void)
{
	console_puts("ZTRCHECK 1.0 for KonaOS\r\n"
		     "Usage: ztrcheck FILE.ZTR\r\n");
}

static unsigned char same(const char *left, const char *right)
{
	while (*left == *right) {
		if (*left == '\0') return 1u;
		++left;
		++right;
	}
	return 0u;
}

static void print_report(const struct ztrcheck_report *report)
{
	if (report->check == ZTRCHECK_OK) {
		printf("OK: %s / %s / %s\r\n", report->title,
		       report->author, report->source);
		printf("%u Hz speed %u, %u rows, %u orders, %u patterns, "
		       "%u instruments, macro %u B, %lu B, ",
		       report->rate, (unsigned int)report->speed,
		       report->rows, report->orders, report->patterns,
		       (unsigned int)report->instruments,
		       report->macro_bytes, report->size);
		if (report->crc_stored == 0u) {
			console_puts("crc=none\r\n");
		} else {
			printf("crc=ok %04x\r\n", report->crc_stored);
		}
	} else if (report->check == ZTRCHECK_CRC) {
		printf("FAIL crc at %04x: MISMATCH stored=%04x "
		       "computed=%04x\r\n", report->offset,
		       report->crc_stored, report->crc_computed);
	} else {
		printf("FAIL %s at %04x: %s\r\n",
		       ztrcheck_name(report->check), report->offset,
		       ztrcheck_detail(report->check));
	}
}

int main(int argc, char **argv)
{
	FILE *song_file;
	kos_reader_t reader;
	struct ztrcheck_report report;
	const char *path;
	int result;

	if (argc != 1 || argv == (char **)0 || same(argv[0], "-h") != 0u ||
	    same(argv[0], "--help") != 0u || same(argv[0], "/?") != 0u) {
		usage();
		return argc == 1 ? 0 : 1;
	}
	path = argv[0];
	song_file = fopen(path, "rb");
	if (song_file == (FILE *)0) {
		printf("ztrcheck: cannot open %s\r\n", path);
		return 2;
	}
	result = kos_reader_from_stdio(&reader, song_file);
	if (result < 0) {
		printf("ztrcheck: cannot size %s (%d)\r\n", path, result);
		(void)fclose(song_file);
		return 2;
	}
	result = ztrcheck_run(&reader, scratch, (uint16_t)sizeof(scratch),
			      &report);
	print_report(&report);
	if (fclose(song_file) < 0) return 3;
	return report.check == ZTRCHECK_OK ? 0 : 4;
}
