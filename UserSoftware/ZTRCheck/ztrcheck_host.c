/* ztrcheck_host - the host twin of ztrcheck.exe: same core, same output,
 * driven by the test suites without the simulator.  Build via the
 * Makefile's host target. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kos/reader.h"
#include "ztrcheck_core.h"

static unsigned char scratch[4096];

static int memory_read(void *context, uint32_t offset, void *dst,
		       uint16_t requested)
{
	memcpy(dst, (const unsigned char *)context + offset, requested);
	return (int)requested;
}

static void print_report(const struct ztrcheck_report *report)
{
	if (report->check == ZTRCHECK_OK) {
		printf("OK: %s / %s / %s\n", report->title,
		       report->author, report->source);
		printf("%u Hz speed %u, %u rows, %u orders, %u patterns, "
		       "%u instruments, macro %u B, %lu B, ",
		       (unsigned int)report->rate, (unsigned int)report->speed,
		       (unsigned int)report->rows, (unsigned int)report->orders,
		       (unsigned int)report->patterns,
		       (unsigned int)report->instruments,
		       (unsigned int)report->macro_bytes,
		       (unsigned long)report->size);
		if (report->crc_stored == 0u)
			printf("crc=none\n");
		else
			printf("crc=ok %04X\n", (unsigned int)report->crc_stored);
	} else if (report->check == ZTRCHECK_CRC) {
		printf("FAIL crc at %04X: MISMATCH stored=%04X "
		       "computed=%04X\n", (unsigned int)report->offset,
		       (unsigned int)report->crc_stored,
		       (unsigned int)report->crc_computed);
	} else {
		printf("FAIL %s at %04X: %s\n",
		       ztrcheck_name(report->check),
		       (unsigned int)report->offset,
		       ztrcheck_detail(report->check));
	}
}

int main(int argc, char **argv)
{
	FILE *stream;
	unsigned char *data;
	long length;
	kos_reader_t reader;
	struct ztrcheck_report report;

	if (argc != 2 || strcmp(argv[1], "-h") == 0 ||
	    strcmp(argv[1], "--help") == 0) {
		printf("ztrcheck 1.0 (host)\nusage: ztrcheck FILE.ZTR\n");
		return argc == 2 ? 0 : 1;
	}
	stream = fopen(argv[1], "rb");
	if (stream == NULL) {
		printf("ztrcheck: cannot open %s\n", argv[1]);
		return 2;
	}
	if (fseek(stream, 0, SEEK_END) != 0 || (length = ftell(stream)) < 0 ||
	    fseek(stream, 0, SEEK_SET) != 0) {
		fclose(stream);
		return 2;
	}
	data = malloc(length == 0 ? 1u : (size_t)length);
	if (data == NULL ||
	    (length > 0 && fread(data, (size_t)length, 1, stream) != 1)) {
		free(data);
		fclose(stream);
		printf("ztrcheck: cannot read %s\n", argv[1]);
		return 2;
	}
	fclose(stream);
	reader.context = data;
	reader.read_at = memory_read;
	reader.size = (uint32_t)length;
	(void)ztrcheck_run(&reader, scratch, (uint16_t)sizeof(scratch),
			   &report);
	print_report(&report);
	free(data);
	return report.check == ZTRCHECK_OK ? 0 : 4;
}
