/* host_reader - the reader host test (tests/reader/host).
 *
 * Builds the memory adapter and read_full with the host compiler and
 * drives them against a fake reader that returns short chunks,
 * transient errors and an early end.  The target adapters (handle,
 * stdio, KRES) are exercised on the card by readertest. */

#include "kos.h"

#include <stdio.h>
#include <string.h>

static unsigned char pattern[256];
static unsigned char out[256];

/* The memory adapter's own bounds and bytes. */
static int test_memory(void)
{
	kos_memory_source_t source;
	kos_reader_t reader;
	unsigned int i;

	for (i = 0u; i < sizeof(pattern); ++i) {
		pattern[i] = (unsigned char)(i * 7u + 3u);
	}
	source.data = pattern;
	source.size = sizeof(pattern);
	if (kos_reader_from_memory(&reader, &source) != 0) {
		printf("host: construct failed\n");
		return 1;
	}
	if (reader.size != sizeof(pattern)) {
		printf("host: bad size\n");
		return 1;
	}
	memset(out, 0, sizeof(out));
	if (reader.read_at(reader.context, 10u, out, 50u) != 50 ||
	    memcmp(out, pattern + 10, 50u) != 0) {
		printf("host: middle read wrong\n");
		return 1;
	}
	memset(out, 0, sizeof(out));
	if (reader.read_at(reader.context, 240u, out, 40u) != 16 ||
	    memcmp(out, pattern + 240, 16u) != 0) {
		printf("host: tail read wrong\n");
		return 1;
	}
	if (reader.read_at(reader.context, 256u, out, 40u) != 0) {
		printf("host: past the end\n");
		return 1;
	}
	return 0;
}

/* The fake reader: chunked, with injected failures. */
static unsigned char fake_data[100];

struct fake_ctx {
	int calls;     /* the read-at call index */
	int short_cap; /* cap every read at this many bytes */
	int fail_at;   /* return -KOS_EIO at this call index */
};

static int fake_read_at(void *context, uint32_t offset, void *dst,
			uint16_t requested)
{
	struct fake_ctx *ctx = (struct fake_ctx *)context;

	if (ctx->fail_at >= 0 && ctx->calls == ctx->fail_at) {
		ctx->calls++;
		return -KOS_EIO;
	}
	ctx->calls++;
	if (offset >= sizeof(fake_data)) {
		return 0;
	}
	if ((uint32_t)requested > sizeof(fake_data) - offset) {
		requested = (uint16_t)(sizeof(fake_data) - offset);
	}
	if (ctx->short_cap > 0 && requested > (uint16_t)ctx->short_cap) {
		requested = (uint16_t)ctx->short_cap;
	}
	memcpy(dst, fake_data + offset, requested);
	return (int)requested;
}

static int test_read_full(void)
{
	kos_reader_t reader;
	struct fake_ctx ctx;
	unsigned int i;

	for (i = 0u; i < sizeof(fake_data); ++i) {
		fake_data[i] = (unsigned char)(0xA5u - i);
	}
	ctx.calls = 0;
	ctx.short_cap = 0;
	ctx.fail_at = -1;
	reader.context = &ctx;
	reader.read_at = fake_read_at;
	reader.size = sizeof(fake_data);

	/* Whole range through 3-byte chunks. */
	ctx.calls = 0;
	ctx.short_cap = 3;
	memset(out, 0, sizeof(out));
	if (kos_reader_read_full(&reader, 0u, out,
				 (uint16_t)sizeof(fake_data)) !=
	    (int)sizeof(fake_data) ||
	    memcmp(out, fake_data, sizeof(fake_data)) != 0) {
		printf("host: read_full chunks\n");
		return 1;
	}

	/* A transient error on the third call propagates. */
	ctx.calls = 0;
	ctx.short_cap = 20;
	ctx.fail_at = 2;
	memset(out, 0, sizeof(out));
	if (kos_reader_read_full(&reader, 0u, out, 50u) != -KOS_EIO) {
		printf("host: read_full transient\n");
		return 1;
	}

	/* An early end (zero before the range completes). */
	ctx.calls = 0;
	ctx.fail_at = -1;
	reader.size = 200u;
	if (kos_reader_read_full(&reader, 0u, out, 150u) != -KOS_EIO) {
		printf("host: read_full early end\n");
		return 1;
	}
	reader.size = sizeof(fake_data);

	/* A range that starts inside and runs past the end. */
	ctx.calls = 0;
	if (kos_reader_read_full(&reader, 90u, out, 20u) != -KOS_EIO) {
		printf("host: read_full past end\n");
		return 1;
	}
	return 0;
}

int main(void)
{
	if (test_memory() != 0 || test_read_full() != 0) {
		printf("host: reader FAIL\n");
		return 1;
	}
	printf("host: reader PASS\n");
	return 0;
}
