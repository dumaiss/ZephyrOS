/* KRES host tests: verify refusals (§10).  Deterministic corruption
 * of a valid container: the magic, the version, the record size, the
 * catalog size, sealed inconsistencies, bad names, duplicates,
 * out-of-range and overlapping extents, oversized used_bytes. */

#include "kres_test.h"

#include "kos/kres.h"

static void install(void)
{
	static uint8_t ctx;

	mem_io_reset();
	mem_truncate_refuses_growth(1);
	kres_install(mem_io(), &kres_cache_mem, &ctx);
}

static const struct mem_file *build_valid(const char *path)
{
	kres_container c;
	kres_resource r;

	CHECK_EQ(kres_create_container(path), 0);
	CHECK_EQ(kres_open(&c, path, KRES_OPEN_READ | KRES_OPEN_WRITE), 0);
	CHECK_EQ(kres_create(&c, "alpha", 2u, &r), 0);
	CHECK_EQ(kres_write(&r, "AAAA", 4), 4);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_create(&c, "beta", 1u, &r), 0);
	CHECK_EQ(kres_write(&r, "BB", 2), 2);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_close(&c), 0);
	CHECK_EQ(kres_verify(path), 0);
	return mem_file_by_name(path);
}

/* Run verify on a corrupted copy; it must refuse. */
static void expect_refused(const uint8_t *good, uint32_t size,
			   unsigned int at, uint8_t value)
{
	uint8_t *bad = (uint8_t *)malloc(size);

	memcpy(bad, good, size);
	bad[at] = value;
	mem_file_new("bad.kpr", bad, size);
	free(bad);
	CHECK(kres_verify("bad.kpr") < 0);
}

static void put32(uint8_t *at, uint32_t value)
{
	at[0] = (uint8_t)value;
	at[1] = (uint8_t)(value >> 8);
	at[2] = (uint8_t)(value >> 16);
	at[3] = (uint8_t)(value >> 24);
}

int main(void)
{
	const struct mem_file *good;
	uint8_t *bytes;
	uint32_t size;
	unsigned int entry_off; /* catalog record 5, entry 0 (active B) */

	install();
	good = build_valid("good.kpr");
	CHECK(good != (const struct mem_file *)0);
	if (good == (const struct mem_file *)0) {
		TEST_DONE();
	}
	size = good->size;
	bytes = (uint8_t *)malloc(size);
	memcpy(bytes, good->data, size);

	/* Header corruptions. */
	expect_refused(bytes, size, 0u, (uint8_t)'X');   /* magic */
	expect_refused(bytes, size, 4u, (uint8_t)2);     /* major */
	expect_refused(bytes, size, 9u, (uint8_t)0x01);  /* record size */
	expect_refused(bytes, size, 10u, (uint8_t)8);    /* catalog recs */
	/* A mutable header that claims the sealed flag while the
	 * active area is B: refused. */
	{
		uint8_t *sealed = (uint8_t *)malloc(size);

		memcpy(sealed, bytes, size);
		put32(sealed + 0x18, 1u); /* SEALED */
		sealed[0x10] = 1u;        /* active B */
		mem_file_new("bad.kpr", sealed, size);
		free(sealed);
		CHECK(kres_verify("bad.kpr") < 0);
	}
	/* A sealed container with a nonzero generation is refused. */
	{
		uint8_t *sealed = (uint8_t *)malloc(size);

		memcpy(sealed, bytes, size);
		put32(sealed + 0x18, 1u);
		sealed[0x10] = 0u;
		put32(sealed + 0x0C, 3u); /* generation */
		mem_file_new("bad.kpr", sealed, size);
		free(sealed);
		CHECK(kres_verify("bad.kpr") < 0);
	}

	/* Catalog corruptions: the active area is B, record 5. */
	entry_off = 5u * 512u;
	expect_refused(bytes, size, entry_off + 0x3Cu, (uint8_t)0); /* empty */
	expect_refused(bytes, size, entry_off + 0x3Cu, (uint8_t)49); /* long */
	expect_refused(bytes, size, entry_off + 0x3Du, (uint8_t)1); /* flags */
	expect_refused(bytes, size, entry_off + 0x3Eu, (uint8_t)1); /* reserved */
	/* Out-of-range extents. */
	{
		uint8_t *bad = (uint8_t *)malloc(size);

		memcpy(bad, bytes, size);
		put32(bad + entry_off + 0x30u, 60000u); /* start */
		mem_file_new("bad.kpr", bad, size);
		free(bad);
		CHECK(kres_verify("bad.kpr") < 0);
	}
	{
		uint8_t *bad = (uint8_t *)malloc(size);

		memcpy(bad, bytes, size);
		put32(bad + entry_off + 0x34u, 5000u); /* allocated */
		mem_file_new("bad.kpr", bad, size);
		free(bad);
		CHECK(kres_verify("bad.kpr") < 0);
	}
	{
		uint8_t *bad = (uint8_t *)malloc(size);

		memcpy(bad, bytes, size);
		put32(bad + entry_off + 0x38u, 4096u); /* used > allocated */
		mem_file_new("bad.kpr", bad, size);
		free(bad);
		CHECK(kres_verify("bad.kpr") < 0);
	}
	/* Duplicate names. */
	{
		uint8_t *bad = (uint8_t *)malloc(size);

		memcpy(bad, bytes, size);
		memcpy(bad + entry_off + 64u, bad + entry_off, 64u);
		mem_file_new("bad.kpr", bad, size);
		free(bad);
		CHECK(kres_verify("bad.kpr") < 0);
	}
	/* Overlapping extents: entry 1 starts inside entry 0. */
	{
		uint8_t *bad = (uint8_t *)malloc(size);

		memcpy(bad, bytes, size);
		put32(bad + entry_off + 64u + 0x30u, 9u);
		mem_file_new("bad.kpr", bad, size);
		free(bad);
		CHECK(kres_verify("bad.kpr") < 0);
	}
	/* A file too short for the header and catalog. */
	{
		uint8_t *shorty = (uint8_t *)malloc(1024u);

		memcpy(shorty, bytes, 1024u);
		mem_file_new("bad.kpr", shorty, 1024u);
		free(shorty);
		CHECK(kres_verify("bad.kpr") < 0);
	}

	free(bytes);
	TEST_DONE();
}
