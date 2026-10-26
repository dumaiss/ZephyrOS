/* readertest - the reader card program (tests/reader).
 *
 * One fixture through all four adapters, with matching checksums,
 * random offsets and lengths (including past the end), a proof that
 * none of the adapters closed its source, and a framed read_at cost
 * report.  tests/test_reader.py runs it from the shell and checks the
 * prints, the byte identity and the FS2 open count.
 *
 *   readertest          the probes and the checksums
 *   readertest timing   the framed read_at cost report
 */

#include "kos.h"

#include "kos/reader.h"
#include "kos/fs.h"
#include "kos/kres.h"
#include "kos/kres_konaos.h"
#include "kos/errno.h"
#include "klibc/stdio.h"
#include "klibc/string.h"

#define CARD "/fs/CARD.DAT"
#define BUILT "/fs/READER.KPR"
#define RESOURCE "card.dat"
#define MEM_MAX 1024u
#define PROBE_MAX 128u
#define TIMING_CALLS 16u

static unsigned char memory_copy[MEM_MAX];
static unsigned char probe_a[PROBE_MAX];
static unsigned char probe_b[PROBE_MAX];
static unsigned char probe_c[PROBE_MAX];
static unsigned char probe_d[PROBE_MAX];
static unsigned char kresread_ctx;

static unsigned int seed;

static unsigned int next_rand(void)
{
	unsigned int x = seed;

	x ^= (unsigned int)(x << 7);
	x ^= (unsigned int)(x >> 9);
	x ^= (unsigned int)(x << 8);
	seed = x;
	return x;
}

/* The whole file through read_full, summed mod 2^16. */
static int checksum(kos_reader_t *r, unsigned int *sum_out)
{
	unsigned int sum = 0u;
	uint32_t at = 0u;

	while (at < r->size) {
		uint16_t chunk = (uint16_t)((r->size - at) > 64u ?
					   64u : (r->size - at));
		int got = kos_reader_read_full(r, at, probe_a, chunk);
		unsigned int i;

		if (got != (int)chunk) {
			return got < 0 ? got : -KOS_EIO;
		}
		for (i = 0u; i < (unsigned int)chunk; ++i) {
			sum = (sum + probe_a[i]) & 0xFFFFu;
		}
		at += chunk;
	}
	*sum_out = sum;
	return 0;
}

/* One probe through one adapter: returns the byte count or a
 * negative error. */
static int probe(kos_reader_t *r, uint32_t offset, uint16_t length,
		 unsigned char *dst)
{
	return r->read_at(r->context, offset, dst, length);
}

static int same(const unsigned char *a, const unsigned char *b,
		unsigned int length)
{
	return memcmp(a, b, length) == 0 ? 0 : 1;
}

/* The four adapters' results over one probe must match, including a
 * past-the-end probe where every adapter answers 0. */
static int probe_all(kos_reader_t *handle_reader,
		     kos_reader_t *stdio_reader,
		     kos_reader_t *kres_reader,
		     kos_reader_t *memory_reader,
		     uint32_t offset, uint16_t length, unsigned int index)
{
	int a = probe(handle_reader, offset, length, probe_a);
	int b = probe(stdio_reader, offset, length, probe_b);
	int c = probe(kres_reader, offset, length, probe_c);
	int d = probe(memory_reader, offset, length, probe_d);

	if (a < 0 || b < 0 || c < 0 || d < 0) {
		printf("readertest: probe %u error %d %d %d %d\r\n",
		       index, a, b, c, d);
		return 1;
	}
	if (a != b || a != c || a != d ||
	    same(probe_a, probe_b, (unsigned int)a) != 0 ||
	    same(probe_a, probe_c, (unsigned int)a) != 0 ||
	    same(probe_a, probe_d, (unsigned int)a) != 0) {
		printf("readertest: probe %u mismatch at %lu len %u "
		       "(%d %d %d %d)\r\n",
		       index, (unsigned long)offset, (unsigned int)length,
		       a, b, c, d);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	kos_handle_source_t handle_source;
	kos_memory_source_t memory_source;
	kos_reader_t handle_reader;
	kos_reader_t stdio_reader;
	kos_reader_t kres_reader;
	kos_reader_t memory_reader;
	kres_container container;
	kres_resource resource;
	FILE *file;
	unsigned int handle;
	unsigned int sum_handle = 0u;
	unsigned int sum_stdio = 0u;
	unsigned int sum_kres = 0u;
	unsigned int sum_memory = 0u;
	unsigned int probes;
	unsigned int index;
	int result;
	int timing = argc == 1 && argv != (char **)0 &&
		strcmp(argv[0], "timing") == 0;

	seed = 0x6A2Bu;

	/* 1: the OS file handle. */
	handle = open(CARD, KFS_O_READ);
	if ((int)handle < 0) {
		printf("readertest: open %d\r\n", (int)handle);
		return 1;
	}
	handle_source.handle = handle;
	result = kos_reader_from_handle(&handle_reader, &handle_source);
	if (result != 0) {
		printf("readertest: handle reader %d\r\n", result);
		return 1;
	}

	/* 2: the borrowed memory, loaded through the handle reader. */
	if (handle_reader.size > MEM_MAX) {
		printf("readertest: fixture too large\r\n");
		return 1;
	}
	result = kos_reader_read_full(&handle_reader, 0u, memory_copy,
				      (uint16_t)handle_reader.size);
	if (result != (int)handle_reader.size) {
		printf("readertest: memory load %d\r\n", result);
		return 1;
	}
	memory_source.data = memory_copy;
	memory_source.size = handle_reader.size;
	result = kos_reader_from_memory(&memory_reader, &memory_source);
	if (result != 0) {
		printf("readertest: memory reader %d\r\n", result);
		return 1;
	}

	/* 3: the buffered stream.  The FS2 provider allows two open
	 * files, so the stdio and handle sources share the card with
	 * nothing else open; the KRES phase runs after they close. */
	file = fopen(CARD, "r");
	if (file == (FILE *)0) {
		printf("readertest: fopen failed\r\n");
		return 1;
	}
	result = kos_reader_from_stdio(&stdio_reader, file);
	if (result != 0) {
		printf("readertest: stdio reader %d\r\n", result);
		return 1;
	}

	/* The three sizes agree. */
	if (handle_reader.size != stdio_reader.size ||
	    handle_reader.size != memory_reader.size) {
		printf("readertest: size mismatch %lu %lu %lu\r\n",
		       (unsigned long)handle_reader.size,
		       (unsigned long)stdio_reader.size,
		       (unsigned long)memory_reader.size);
		return 1;
	}

	/* The three checksums agree. */
	result = checksum(&handle_reader, &sum_handle);
	result |= checksum(&stdio_reader, &sum_stdio);
	result |= checksum(&memory_reader, &sum_memory);
	printf("readertest: sums %u %u %u 0 size %lu rc %d\r\n",
	       sum_handle, sum_stdio, sum_memory,
	       (unsigned long)handle_reader.size, result);
	if (result != 0 || sum_handle != sum_stdio ||
	    sum_handle != sum_memory) {
		printf("readertest: checksum mismatch\r\n");
		return 1;
	}

	/* Random offsets and lengths against the memory reference,
	 * including past the end. */
	probes = 64u;
	for (index = 0u; index < probes; ++index) {
		uint32_t offset = (uint32_t)(next_rand() %
			     (handle_reader.size + 8u));
		uint16_t length = (uint16_t)(next_rand() % 129u);

		if (probe_all(&handle_reader, &stdio_reader, &memory_reader,
			      &memory_reader, offset, length, index) != 0) {
			return 1;
		}
	}
	printf("readertest: probes %u ok\r\n", probes);

	if (timing) {
		unsigned int i;

		/* Framed by its printed lines: the harness brackets each
		 * phase and subtracts the empty loop, so the per-call
		 * read_at cost of each adapter is reported. */
		printf("reader-time begin\r\n");
		for (i = 0u; i < TIMING_CALLS; ++i) {
			(void)probe(&handle_reader, 100u, 32u, probe_a);
		}
		printf("reader-time handle\r\n");
		for (i = 0u; i < TIMING_CALLS; ++i) {
			(void)probe(&stdio_reader, 100u, 32u, probe_b);
		}
		printf("reader-time stdio\r\n");
		for (i = 0u; i < TIMING_CALLS; ++i) {
			(void)probe(&memory_reader, 100u, 32u, probe_d);
		}
		printf("reader-time memory\r\n");
		for (i = 0u; i < TIMING_CALLS; ++i) {
		}
		printf("reader-time zero\r\n");
	}

	/* No adapter closed its source: the raw sources still read. */
	if (seek(handle, 0, KFS_SEEK_SET) < 0 ||
	    read(handle, probe_a, 8u) != 8 ||
	    fseek(file, 0, SEEK_SET) != 0 ||
	    fread(probe_b, 1u, 8u, file) != 8u) {
		printf("readertest: source closed\r\n");
		return 1;
	}
	if (same(probe_a, probe_b, 8u) != 0 ||
	    same(probe_a, memory_copy, 8u) != 0) {
		printf("readertest: source bytes differ\r\n");
		return 1;
	}
	printf("readertest: sources alive\r\n");

	/* 4: the KRES resource through the container's one handle,
	 * with the card's two file slots free again. */
	(void)fclose(file);
	(void)close(handle);
	kres_install(&kres_io_konaos_read, &kres_cache_xmem, &kresread_ctx);
	result = kres_open(&container, BUILT, KRES_OPEN_READ);
	if (result < 0) {
		printf("readertest: kres open %d\r\n", result);
		return 1;
	}
	result = kres_get(&container, RESOURCE, &resource, KRES_OPEN_READ);
	if (result < 0) {
		printf("readertest: kres get %d\r\n", result);
		return 1;
	}
	result = kos_reader_from_kres(&kres_reader, &resource);
	if (result != 0) {
		printf("readertest: kres reader %d\r\n", result);
		return 1;
	}
	if (kres_reader.size != memory_reader.size) {
		printf("readertest: kres size mismatch %lu %lu\r\n",
		       (unsigned long)kres_reader.size,
		       (unsigned long)memory_reader.size);
		return 1;
	}
	result = checksum(&kres_reader, &sum_kres);
	if (result != 0 || sum_kres != sum_memory) {
		printf("readertest: kres checksum mismatch %u %u\r\n",
		       sum_kres, sum_memory);
		return 1;
	}
	for (index = 0u; index < probes; ++index) {
		uint32_t offset = (uint32_t)(next_rand() %
			     (memory_reader.size + 8u));
		uint16_t length = (uint16_t)(next_rand() % 129u);

		if (probe_all(&kres_reader, &memory_reader, &memory_reader,
			      &memory_reader, offset, length, index) != 0) {
			return 1;
		}
	}
	printf("readertest: kres probes %u ok\r\n", probes);

	/* The KRES source still reads after the reader was used. */
	if (kres_seek(&resource, 0u) != 0 ||
	    kres_read(&resource, probe_c, 8u) != 8) {
		printf("readertest: kres source closed\r\n");
		return 1;
	}
	if (same(probe_c, memory_copy, 8u) != 0) {
		printf("readertest: kres source bytes differ\r\n");
		return 1;
	}
	printf("readertest: kres alive\r\n");

	if (timing) {
		unsigned int i;

		printf("kres-time begin\r\n");
		for (i = 0u; i < TIMING_CALLS; ++i) {
			(void)probe(&kres_reader, 100u, 32u, probe_c);
		}
		printf("kres-time kres\r\n");
		for (i = 0u; i < TIMING_CALLS; ++i) {
		}
		printf("kres-time zero\r\n");
	}
	printf("readertest: reader PASS\r\n");
	return 0;
}
