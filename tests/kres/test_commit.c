/* KRES host tests: the commit order (§5.6) and its failure model.
 *
 * Every adapter write and every sync a flush performs is failed in
 * turn - a flush that writes into the inactive catalog area, and one
 * that relocates that area to the allocation mark because the catalog
 * outgrew it.  After each failure the container is abandoned like a
 * crash and must still open on the previous committed catalog, verify,
 * and accept the same mutation again.  A torn header (any prefix of
 * the new header over the old one) is refused, never misread. */

#include "kres_test.h"

#include "kos/kres.h"

static uint8_t ctx;

static void install(void)
{
	mem_io_reset();
	mem_truncate_refuses_growth(1);
	kres_install_mut(mem_io(), &kres_cache_mem, &ctx);
}

/* A fresh container with `committed` resources c0..c(n-1), each
 * holding its name. */
static void make_base(unsigned int committed)
{
	kres_container c;
	kres_resource r;
	unsigned int i;

	install();
	CHECK_EQ(kres_create_container("c.kpr"), 0);
	CHECK_EQ(kres_open(&c, "c.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	for (i = 0u; i < committed; ++i) {
		char name[16];

		sprintf(name, "c%02u", i);
		CHECK_EQ(kres_create(&c, name, 1u, &r), 0);
		CHECK_EQ(kres_write(&r, name, 3), 3);
		CHECK_EQ(kres_rclose(&r), 0);
	}
	CHECK_EQ(kres_close(&c), 0);
}

/* The pending mutation: two new resources (the second takes the
 * catalog past the next record boundary when committed is 7 or 8). */
static void mutate(kres_container *c)
{
	kres_resource r;

	CHECK_EQ(kres_create(c, "lose1", 1u, &r), 0);
	CHECK_EQ(kres_write(&r, "LOSE", 4), 4);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_create(c, "a-first", 2u, &r), 0);
	CHECK_EQ(kres_write(&r, "AFIRST", 6), 6);
	CHECK_EQ(kres_rclose(&r), 0);
}

/* The committed state is exactly `committed` resources. */
static void expect_previous(unsigned int committed)
{
	kres_container c;
	kres_resource r;
	unsigned int i;

	CHECK_EQ(kres_verify("c.kpr", (struct kres_usage *)0), 0);
	CHECK_EQ(kres_open(&c, "c.kpr", KRES_OPEN_READ), 0);
	CHECK_EQ(c.resource_count, committed);
	for (i = 0u; i < committed; ++i) {
		char name[16];
		char got[4];

		sprintf(name, "c%02u", i);
		CHECK_EQ(kres_get(&c, name, &r, KRES_OPEN_READ), 0);
		CHECK_EQ(kres_read(&r, got, 3), 3);
		CHECK(memcmp(got, name, 3) == 0);
		CHECK_EQ(kres_rclose(&r), 0);
	}
	CHECK_EQ(kres_get(&c, "lose1", &r, KRES_OPEN_READ), -KOS_ENOENT);
	CHECK_EQ(kres_get(&c, "a-first", &r, KRES_OPEN_READ), -KOS_ENOENT);
	CHECK_EQ(kres_close(&c), 0);
}

/* After a crash the same mutation commits cleanly. */
static void expect_recovers(unsigned int committed)
{
	kres_container c;
	kres_resource r;

	CHECK_EQ(kres_open(&c, "c.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	mutate(&c);
	CHECK_EQ(kres_close(&c), 0);
	CHECK_EQ(kres_verify("c.kpr", (struct kres_usage *)0), 0);
	CHECK_EQ(kres_open(&c, "c.kpr", KRES_OPEN_READ), 0);
	CHECK_EQ(c.resource_count, committed + 2u);
	CHECK_EQ(kres_get(&c, "a-first", &r, KRES_OPEN_READ), 0);
	CHECK_EQ(r.used_bytes, 6u);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_close(&c), 0);
}

/* How many crashes left the new state committed: only a failed header
 * sync may (the header write itself completed). */
static unsigned int committed_anyway;

/* Fail the `fail_at`-th write (or sync) of the flush; returns 1 when
 * the flush failed (the fault landed inside it). */
static int crash_once(unsigned int committed, unsigned int fail_at,
		      int on_sync, uint16_t *relocated)
{
	kres_container c;
	uint16_t inactive_start;
	int result;

	make_base(committed);
	CHECK_EQ(kres_open(&c, "c.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	mutate(&c);
	inactive_start = c.catalog_start[1u - c.active_area];
	if (on_sync) {
		mem_fail_sync(mem_count_syncs() + fail_at, -KOS_EIO);
	} else {
		mem_fail_write(mem_count_writes() + fail_at, -KOS_EIO);
	}
	result = kres_flush(&c);
	mem_fail_sync(0xFFFFFFFFu, 0);
	mem_fail_write(0xFFFFFFFFu, 0);
	if (result == 0) {
		*relocated = (uint16_t)(c.catalog_start[c.active_area] !=
					inactive_start);
		CHECK_EQ(kres_close(&c), 0);
		return 0;
	}
	/* The crash: the process state goes, the file stays. */
	CHECK_EQ(mem_io()->close(&c), 0);
	(void)kres_cache_mem.release(&c);
	{
		const struct mem_file *f = mem_file_by_name("c.kpr");

		if (f->data[0x12] == committed + 2u) {
			/* The header reached the file before the failed
			 * sync: the new state, complete. */
			CHECK(on_sync);
			++committed_anyway;
			CHECK_EQ(kres_verify("c.kpr",
					     (struct kres_usage *)0), 0);
			return 1;
		}
	}
	expect_previous(committed);
	expect_recovers(committed);
	return 1;
}

static void sweep(unsigned int committed, int expect_relocation)
{
	unsigned int fail_at;
	unsigned int failed_writes = 0u;
	unsigned int failed_syncs = 0u;
	uint16_t relocated = 0xFFFFu;

	committed_anyway = 0u;
	for (fail_at = 0u; fail_at < 64u; ++fail_at) {
		if (crash_once(committed, fail_at, 0, &relocated) == 0) {
			break;
		}
		++failed_writes;
	}
	for (fail_at = 0u; fail_at < 64u; ++fail_at) {
		if (crash_once(committed, fail_at, 1, &relocated) == 0) {
			break;
		}
		++failed_syncs;
	}
	/* The catalog record(s) and the header; the payload, catalog
	 * and header syncs. */
	CHECK(failed_writes >= 2u);
	CHECK_EQ(failed_syncs, 3u);
	CHECK_EQ(committed_anyway, 1u); /* the header sync, only */
	CHECK_EQ(relocated, (uint16_t)expect_relocation);
}

/* A torn header: the first `k` bytes of the new header over the old
 * one.  Either it equals one of them, or the open refuses it. */
static void torn_headers(void)
{
	kres_container c;
	uint8_t before[KRES_RECORD_SIZE];
	uint8_t after[KRES_RECORD_SIZE];
	uint8_t *image;
	const struct mem_file *f;
	uint32_t size;
	unsigned int k;
	unsigned int refused = 0u;

	make_base(3u);
	f = mem_file_by_name("c.kpr");
	memcpy(before, f->data, KRES_RECORD_SIZE);
	CHECK_EQ(kres_open(&c, "c.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	mutate(&c);
	CHECK_EQ(kres_close(&c), 0);
	f = mem_file_by_name("c.kpr");
	memcpy(after, f->data, KRES_RECORD_SIZE);
	size = f->size;
	image = (uint8_t *)malloc(size);
	for (k = 1u; k < KRES_RECORD_SIZE; ++k) {
		memcpy(image, f->data, size);
		memcpy(image, after, k);
		memcpy(image + k, before + k, KRES_RECORD_SIZE - k);
		if (memcmp(image, before, KRES_RECORD_SIZE) == 0 ||
		    memcmp(image, after, KRES_RECORD_SIZE) == 0) {
			continue;
		}
		mem_file_new("torn.kpr", image, size);
		CHECK_EQ(kres_open(&c, "torn.kpr", KRES_OPEN_READ),
			 -KRES_E_BAD);
		CHECK_EQ(kres_verify("torn.kpr", (struct kres_usage *)0),
			 -KRES_E_BAD);
		++refused;
		f = mem_file_by_name("c.kpr");
	}
	CHECK(refused > 0u);
	free(image);
}

int main(void)
{
	/* The inactive area has room: no relocation. */
	sweep(1u, 0);
	/* Seven committed plus two: nine entries outgrow the one-record
	 * inactive area, which relocates to the allocation mark. */
	sweep(7u, 1);
	/* Fifteen committed (both areas grown to two records) plus two:
	 * seventeen outgrow them again. */
	sweep(15u, 1);
	torn_headers();
	TEST_DONE();
}
