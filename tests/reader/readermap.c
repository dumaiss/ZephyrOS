/* readermap - the memory-adapter-only link proof (tests/reader).
 *
 * Links exactly one reader adapter: the map must carry the memory
 * member and nothing else from the reader series - no handle, stdio
 * or KRES adapter, no KRES/fs/stdio members.  tests/test_reader.py
 * checks the map's symbol set. */

#include "kos.h"

#include "kos/reader.h"

static const unsigned char bytes[16] = {
	0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
	8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u
};

int main(void)
{
	kos_memory_source_t source;
	kos_reader_t reader;
	unsigned char out[4];
	int got;

	source.data = bytes;
	source.size = sizeof(bytes);
	if (kos_reader_from_memory(&reader, &source) != 0) {
		printf("readermap: construct failed\r\n");
		return 1;
	}
	got = reader.read_at(reader.context, 3u, out, 4u);
	printf("readermap: read %d size %lu\r\n", got,
	       (unsigned long)reader.size);
	if (got != 4 || out[0] != 3u || out[1] != 4u || out[2] != 5u ||
	    out[3] != 6u) {
		printf("readermap: mismatch\r\n");
		return 1;
	}
	printf("readermap: map PASS\r\n");
	return 0;
}
