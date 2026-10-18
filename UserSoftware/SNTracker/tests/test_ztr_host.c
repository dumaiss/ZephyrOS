#include "sntracker.h"

#include <stdio.h>
#include <stdlib.h>

int stat(const char *path, struct kfs_stat *info)
{
	FILE *stream = fopen(path, "rb");
	long size;
	if (stream == NULL) return -1;
	if (fseek(stream, 0L, SEEK_END) != 0) {
		(void)fclose(stream);
		return -1;
	}
	size = ftell(stream);
	(void)fclose(stream);
	if (size < 0L) return -1;
	info->size = (uint32_t)size;
	info->type = KFS_TYPE_FILE;
	return 0;
}

static unsigned int number(const char *text)
{
	return (unsigned int)strtoul(text, NULL, 10);
}

static int write_cpm_copy(const char *source, const char *destination)
{
	FILE *input = fopen(source, "rb");
	FILE *output;
	unsigned char block[512];
	size_t got;
	unsigned long length = 0ul;
	if (input == NULL) return -1;
	output = fopen(destination, "wb");
	if (output == NULL) {
		(void)fclose(input);
		return -1;
	}
	while ((got = fread(block, 1u, sizeof(block), input)) != 0u) {
		if (fwrite(block, 1u, got, output) != got) return -1;
		length += got;
	}
	while ((length & 127ul) != 0ul) {
		if (fputc(0x1A, output) == EOF) return -1;
		++length;
	}
	if (fclose(input) != 0 || fclose(output) != 0) return -1;
	return 0;
}

int main(int argc, char **argv)
{
	unsigned char minus_one[] = { 0xFFu, 0xFFu };
	unsigned char minimum[] = { 0x00u, 0x80u };
	int result;
	if (argc != 6) return 2;
	if (ztr_i16(minus_one) != -1 || ztr_i16(minimum) != -32768) return 3;
	result = ztr_load(argv[1]);
	if (result < 0) {
		fprintf(stderr, "%s: %s (%d)\n", argv[1], ztr_error, result);
		return 1;
	}
	if (ztr_song.tick_rate != number(argv[2]) ||
	    ztr_song.order_count != number(argv[3]) ||
	    ztr_song.pattern_count != number(argv[4]) ||
	    ztr_song.instrument_count != number(argv[5])) {
		fprintf(stderr, "%s: decoded metadata mismatch\n", argv[1]);
		return 1;
	}
	if (write_cpm_copy(argv[1], "build/tests/cpm-record.ztr") < 0 ||
	    ztr_load("build/tests/cpm-record.ztr") < 0) {
		fprintf(stderr, "%s: CP/M-record copy rejected: %s\n",
			argv[1], ztr_error);
		return 1;
	}
	printf("%s: %u bytes, %u patterns, valid ZTR1\n", argv[1],
	       ztr_size, ztr_song.pattern_count);
	return 0;
}
