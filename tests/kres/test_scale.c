/* KRES host tests: catalogs of any size.
 *
 * - Resource counts 0, 1, 8, 31, 32, 33, 64, 128, 255, 256, 257, 512
 *   and 1,024, sealed (built in a scrambled order) and mutable
 *   (created in a scrambled order across several commits): verify,
 *   sorted enumeration, every payload byte, compaction and sealing.
 * - Lookup: first, middle, last, missing and near-match names; each
 *   read-only lookup reads at most ceil(log2(N + 1)) catalog records,
 *   and a full enumeration reads each catalog record once.
 * - Byte order: case, slash, CP850 bytes above 7Fh, prefixes.
 * - Mutation: sorted insertion, a rename that moves, deletes at the
 *   ends and the middle, writes through a descriptor whose entry
 *   moved, relocation on growth, repeated catalog relocation, reuse
 *   of a large enough inactive area, compaction after relocations.
 *
 * Built twice: on the host memory store, and (KRES_TEST_XMEM) on the
 * KonaOS XMEM store over the XMEM shim, where the store's 4,096-entry
 * ceiling and its failure are checked too. */

#include "kres_test.h"

#include "kos/kres.h"

#if defined(KRES_TEST_XMEM)
#include "kos/kres_konaos.h"
#include "kos.h"
#define STORE kres_cache_xmem
#else
#define STORE kres_cache_mem
#endif

static uint8_t ctx;

static void install(void)
{
	mem_io_reset();
	mem_truncate_refuses_growth(1);
	kres_install_mut(mem_io(), &STORE, &ctx);
}

/* Resource i's name: four families of differing lengths and bytes. */
static void name_of(unsigned int i, char *out)
{
	switch (i % 4u) {
	case 0u:
		sprintf(out, "scene/%04u.zvg", i);
		break;
	case 1u:
		sprintf(out, "Scene/%u", i);
		break;
	case 2u:
		sprintf(out, "music/\x82%u", i);
		break;
	default:
		sprintf(out, "%u", i * 7u);
		break;
	}
}

static uint32_t payload_size(unsigned int i)
{
	return (uint32_t)((i * 37u) % 700u);
}

static uint8_t payload_byte(unsigned int i, uint32_t j)
{
	return (uint8_t)(i * 31u + j * 7u + 1u);
}

/* A scrambled order of 0..n-1 (n <= 1024): a fixed shuffle. */
static unsigned int order[1024];

static void scramble(unsigned int n)
{
	uint32_t state = 0x2468ACE1u + n;
	unsigned int i;

	for (i = 0u; i < n; ++i) {
		order[i] = i;
	}
	for (i = n; i > 1u; --i) {
		unsigned int j;
		unsigned int t;

		state = state * 1103515245u + 12345u;
		j = (unsigned int)((state >> 8) % i);
		t = order[i - 1u];
		order[i - 1u] = order[j];
		order[j] = t;
	}
}

static unsigned int log2_ceil(unsigned int n)
{
	unsigned int bits = 0u;

	while ((1u << bits) < n) {
		++bits;
	}
	return bits;
}

/* The bytes of resource i, written through `r`. */
static void write_payload(kres_resource *r, unsigned int i)
{
	uint8_t buf[700];
	uint32_t j;
	uint32_t size = payload_size(i);

	for (j = 0u; j < size; ++j) {
		buf[j] = payload_byte(i, j);
	}
	if (size != 0u) {
		CHECK_EQ(kres_write(r, buf, (uint16_t)size), (int)size);
	}
}

/* Check every property of a container holding resources 0..n-1. */
static void check_all(const char *path, unsigned int n)
{
	kres_container c;
	kres_resource r;
	struct kres_iter it;
	struct kres_info info;
	struct kres_usage usage;
	char previous[KRES_NAME_MAX + 1u];
	char name[KRES_NAME_MAX + 1u];
	unsigned int seen = 0u;
	unsigned int reads;
	unsigned int i;
	unsigned int bound = log2_ceil(n + 1u);

	CHECK_EQ(kres_verify(path, &usage), 0);
	CHECK_EQ(usage.resources, n);
	CHECK_EQ(kres_open(&c, path, KRES_OPEN_READ), 0);
	CHECK_EQ(c.resource_count, n);

	/* A sorted walk, each catalog record read once. */
	reads = mem_count_reads();
	CHECK_EQ(kres_iter_begin(&c, &it), 0);
	previous[0] = '\0';
	while (kres_iter_next(&it, &info) > 0) {
		if (seen != 0u) {
			CHECK(strcmp(previous, (const char *)info.name) < 0);
		}
		strcpy(previous, (const char *)info.name);
		++seen;
	}
	CHECK_EQ(seen, n);
	CHECK(mem_count_reads() - reads <= (n + 7u) / 8u);

	/* Every resource by name: bounded catalog reads per lookup,
	 * then the bytes. */
	for (i = 0u; i < n; ++i) {
		uint8_t buf[700];
		uint32_t size = payload_size(i);
		uint32_t j;
		int got;

		name_of(i, name);
		reads = mem_count_reads();
		got = kres_get(&c, name, &r, KRES_OPEN_READ);
		CHECK_EQ(got, 0);
		CHECK(mem_count_reads() - reads <= bound);
		if (got != 0) {
			continue;
		}
		CHECK_EQ(r.used_bytes, size);
		got = kres_read(&r, buf, 700u);
		CHECK_EQ((uint32_t)got, size);
		for (j = 0u; j < size && got == (int)size; ++j) {
			if (buf[j] != payload_byte(i, j)) {
				CHECK(buf[j] == payload_byte(i, j));
				break;
			}
		}
		CHECK_EQ(kres_rclose(&r), 0);
	}
	/* Missing and near-match names. */
	CHECK_EQ(kres_get(&c, "zzzz", &r, KRES_OPEN_READ), -KOS_ENOENT);
	CHECK_EQ(kres_get(&c, "!", &r, KRES_OPEN_READ), -KOS_ENOENT);
	CHECK_EQ(kres_get(&c, "scene/", &r, KRES_OPEN_READ), -KOS_ENOENT);
	CHECK_EQ(kres_get(&c, "scene/0000.zvgx", &r, KRES_OPEN_READ),
		 -KOS_ENOENT);
	CHECK_EQ(kres_get(&c, "SCENE/1", &r, KRES_OPEN_READ), -KOS_ENOENT);
	CHECK_EQ(kres_get(&c, "Scene/1/", &r, KRES_OPEN_READ), -KOS_ENOENT);
	CHECK_EQ(kres_close(&c), 0);
}

static void sealed_case(unsigned int n)
{
	struct kres_usage usage;
	unsigned int k;

	install();
	scramble(n);
	CHECK_EQ(kres_build_begin("sealed.kpr"), 0);
	for (k = 0u; k < n; ++k) {
		unsigned int i = order[k];
		char name[KRES_NAME_MAX + 1u];
		uint8_t buf[700];
		uint32_t j;

		name_of(i, name);
		CHECK_EQ(kres_build_add(name), 0);
		for (j = 0u; j < payload_size(i); ++j) {
			buf[j] = payload_byte(i, j);
		}
		if (payload_size(i) != 0u) {
			CHECK_EQ(kres_build_write(buf,
					(uint16_t)payload_size(i)),
				 (int)payload_size(i));
		}
	}
	CHECK_EQ(kres_build_end(), 0);
	check_all("sealed.kpr", n);
	CHECK_EQ(kres_verify("sealed.kpr", &usage), 0);
	CHECK_EQ(usage.catalog_records, (n + 7u) / 8u);
	CHECK_EQ(usage.abandoned_records, 0u);
	{
		uint32_t records = 0u;

		for (k = 0u; k < n; ++k) {
			uint32_t size = payload_size(k);

			records += size != 0u ? (size + 511u) / 512u : 1u;
		}
		CHECK_EQ(usage.live_records, records);
		CHECK_EQ(usage.next_record, 1u + records + (n + 7u) / 8u);
	}
}

static void mutable_case(unsigned int n)
{
	kres_container c;
	kres_resource r;
	struct kres_usage usage;
	unsigned int k;

	install();
	scramble(n);
	CHECK_EQ(kres_create_container("mut.kpr"), 0);
	CHECK_EQ(kres_open(&c, "mut.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	for (k = 0u; k < n; ++k) {
		unsigned int i = order[k];
		char name[KRES_NAME_MAX + 1u];

		name_of(i, name);
		CHECK_EQ(kres_create(&c, name, 1u, &r), 0);
		write_payload(&r, i);
		CHECK_EQ(kres_rclose(&r), 0);
		if (k % 50u == 49u) {
			/* Commit and reopen: the catalog relocates as it
			 * outgrows each area. */
			CHECK_EQ(kres_close(&c), 0);
			CHECK_EQ(kres_open(&c, "mut.kpr",
					   KRES_OPEN_READ | KRES_OPEN_WRITE), 0);
		}
	}
	CHECK_EQ(kres_close(&c), 0);
	check_all("mut.kpr", n);
	/* Writable lookups go through the store. */
	CHECK_EQ(kres_open(&c, "mut.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	for (k = 0u; k < n; k += 1u + n / 16u) {
		char name[KRES_NAME_MAX + 1u];

		name_of(k, name);
		CHECK_EQ(kres_get(&c, name, &r, KRES_OPEN_READ), 0);
		CHECK_EQ(r.used_bytes, payload_size(k));
		CHECK_EQ(kres_rclose(&r), 0);
	}
	CHECK_EQ(kres_close(&c), 0);
	/* Compaction reclaims the abandoned catalog areas. */
	CHECK_EQ(kres_compact("mut.kpr", 0u), 0);
	CHECK_EQ(kres_verify("mut.kpr", &usage), 0);
	CHECK_EQ(usage.abandoned_records, 0u);
	CHECK_EQ(usage.catalog_records,
		 2u * (n != 0u ? (n + 7u) / 8u : 1u));
	check_all("mut.kpr", n);
	CHECK_EQ(kres_compact("mut.kpr", KRES_SEAL), 0);
	check_all("mut.kpr", n);
}

static const unsigned int counts[] = {
	0u, 1u, 8u, 31u, 32u, 33u, 64u, 128u, 255u, 256u, 257u, 512u, 1024u,
};

/* --- byte order -------------------------------------------------------- */

static int name_cmp(const char *a, const char *b)
{
	return strcmp(a, b); /* unsigned bytes, a prefix first (C99) */
}

static void byte_order(void)
{
	static const char *const names[] = {
		"b", "a", "B", "A", "a/", "a/b", "ab", "\x82", "\x7f",
		"\x80", "\xff", "a.b", "src/MAIN.MOD", "src/MAIN.MO",
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
	};
	const unsigned int n = sizeof(names) / sizeof(names[0]);
	const char *sorted[sizeof(names) / sizeof(names[0])];
	kres_container c;
	kres_resource r;
	struct kres_iter it;
	struct kres_info info;
	unsigned int i;
	unsigned int j;

	for (i = 0u; i < n; ++i) {
		sorted[i] = names[i];
	}
	for (i = 1u; i < n; ++i) { /* insertion sort, unsigned bytes */
		for (j = i; j > 0u && name_cmp(sorted[j - 1u], sorted[j]) > 0;
		     --j) {
			const char *t = sorted[j];

			sorted[j] = sorted[j - 1u];
			sorted[j - 1u] = t;
		}
	}
	for (j = 0u; j < 2u; ++j) {
		install();
		if (j == 0u) {
			CHECK_EQ(kres_create_container("order.kpr"), 0);
			CHECK_EQ(kres_open(&c, "order.kpr", KRES_OPEN_READ |
							    KRES_OPEN_WRITE),
				 0);
			for (i = 0u; i < n; ++i) {
				CHECK_EQ(kres_create(&c, names[i], 1u, &r), 0);
			}
			CHECK_EQ(kres_close(&c), 0);
		} else {
			CHECK_EQ(kres_build_begin("order.kpr"), 0);
			for (i = 0u; i < n; ++i) {
				CHECK_EQ(kres_build_add(names[i]), 0);
			}
			CHECK_EQ(kres_build_end(), 0);
		}
		CHECK_EQ(kres_verify("order.kpr", (struct kres_usage *)0), 0);
		CHECK_EQ(kres_open(&c, "order.kpr", KRES_OPEN_READ), 0);
		CHECK_EQ(kres_iter_begin(&c, &it), 0);
		for (i = 0u; i < n; ++i) {
			CHECK_EQ(kres_iter_next(&it, &info), 1);
			CHECK(strcmp((const char *)info.name, sorted[i]) == 0);
			CHECK_EQ(kres_get(&c, sorted[i], &r, KRES_OPEN_READ),
				 0);
		}
		CHECK_EQ(kres_iter_next(&it, &info), 0);
		CHECK_EQ(kres_get(&c, "a/b/", &r, KRES_OPEN_READ),
			 -KOS_ENOENT);
		CHECK_EQ(kres_get(&c, "A/", &r, KRES_OPEN_READ), -KOS_ENOENT);
		CHECK_EQ(kres_get(&c, "\x81", &r, KRES_OPEN_READ),
			 -KOS_ENOENT);
		CHECK_EQ(kres_close(&c), 0);
	}
}

/* --- mutation ---------------------------------------------------------- */

/* The names, in order, of the open container. */
static void expect_order(kres_container *c, const char *const *want,
			 unsigned int n)
{
	struct kres_iter it;
	struct kres_info info;
	unsigned int i;

	CHECK_EQ(c->resource_count, n);
	CHECK_EQ(kres_iter_begin(c, &it), 0);
	for (i = 0u; i < n; ++i) {
		CHECK_EQ(kres_iter_next(&it, &info), 1);
		CHECK(strcmp((const char *)info.name, want[i]) == 0);
	}
	CHECK_EQ(kres_iter_next(&it, &info), 0);
}

static void mutations(void)
{
	static const char *const after_inserts[] = { "a", "b", "m", "z" };
	static const char *const after_rename[] = { "b", "m", "y", "z" };
	static const char *const after_deletes[] = { "m", "y" };
	kres_container c;
	kres_resource writer;
	kres_resource r;
	struct kres_info info;
	struct kres_usage usage;
	uint32_t old_start;
	unsigned int i;

	install();
	CHECK_EQ(kres_create_container("mut.kpr"), 0);
	CHECK_EQ(kres_open(&c, "mut.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	/* Sorted insertion, with a writer open on "m" while entries
	 * shift around it. */
	CHECK_EQ(kres_create(&c, "m", 2u, &writer), 0);
	CHECK_EQ(kres_write(&writer, "mm", 2), 2);
	CHECK_EQ(kres_create(&c, "z", 1u, &r), 0);
	CHECK_EQ(kres_create(&c, "a", 1u, &r), 0);
	CHECK_EQ(kres_create(&c, "b", 1u, &r), 0);
	expect_order(&c, after_inserts, 4u);
	CHECK_EQ(kres_write(&writer, "MMM", 3), 3);
	CHECK_EQ(kres_stat(&c, "m", &info), 0);
	CHECK_EQ(info.used_bytes, 5u);
	/* Replacement within capacity: the extent stays. */
	old_start = writer.start_record;
	CHECK_EQ(kres_seek(&writer, 0u), 0);
	CHECK_EQ(kres_write(&writer, "12345", 5), 5);
	CHECK_EQ(writer.start_record, old_start);
	/* A rename that moves the entry ("a" -> "y"). */
	CHECK_EQ(kres_rename(&c, "a", "y"), 0);
	expect_order(&c, after_rename, 4u);
	CHECK_EQ(kres_rename(&c, "y", "z"), -KOS_EEXIST);
	CHECK_EQ(kres_rename(&c, "q", "r"), -KOS_ENOENT);
	/* Relocation on growth, through the descriptor whose entry has
	 * moved twice. */
	{
		uint8_t big[1500];

		memset(big, 'G', sizeof(big));
		CHECK_EQ(kres_seek(&writer, 5u), 0);
		CHECK_EQ(kres_write(&writer, big, 1500), 1500);
		CHECK(writer.start_record != old_start);
		CHECK_EQ(kres_stat(&c, "m", &info), 0);
		CHECK_EQ(info.used_bytes, 1505u);
		CHECK_EQ(info.allocated_records, writer.allocated_records);
	}
	/* Deletes: the first, then the last. */
	CHECK_EQ(kres_delete(&c, "b"), 0);
	CHECK_EQ(kres_delete(&c, "z"), 0);
	expect_order(&c, after_deletes, 2u);
	/* A delete under an open descriptor: its writes are refused. */
	CHECK_EQ(kres_create(&c, "n", 1u, &r), 0);
	CHECK_EQ(kres_delete(&c, "n"), 0);
	CHECK_EQ(kres_write(&r, "x", 1), -KOS_ENOENT);
	CHECK_EQ(kres_rclose(&writer), 0);
	CHECK_EQ(kres_close(&c), 0);
	CHECK_EQ(kres_verify("mut.kpr", (struct kres_usage *)0), 0);
	CHECK_EQ(kres_open(&c, "mut.kpr", KRES_OPEN_READ), 0);
	expect_order(&c, after_deletes, 2u);
	CHECK_EQ(kres_get(&c, "m", &r, KRES_OPEN_READ), 0);
	{
		uint8_t back[1505];

		CHECK_EQ(kres_read(&r, back, 1505), 1505);
		CHECK(memcmp(back, "12345", 5) == 0);
		CHECK_EQ(back[1504], (uint8_t)'G');
	}
	CHECK_EQ(kres_close(&c), 0);

	/* A middle delete. */
	CHECK_EQ(kres_open(&c, "mut.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	CHECK_EQ(kres_create(&c, "x", 1u, &r), 0);
	CHECK_EQ(kres_delete(&c, "x"), 0);
	CHECK_EQ(kres_delete(&c, "m"), 0);
	CHECK_EQ(c.resource_count, 1u);
	CHECK_EQ(kres_close(&c), 0);

	/* Repeated catalog relocation: each time the catalog outgrows
	 * the inactive area, it moves to the allocation mark with the
	 * larger capacity doubled; the active area stays intact. */
	install();
	CHECK_EQ(kres_create_container("grow.kpr"), 0);
	{
		uint16_t capacity = 1u;
		unsigned int created = 0u;
		unsigned int target;

		for (target = 9u; target <= 600u; target = target * 2u + 1u) {
			uint32_t mark;
			uint16_t inactive_start;
			uint8_t inactive;

			CHECK_EQ(kres_open(&c, "grow.kpr",
					   KRES_OPEN_READ | KRES_OPEN_WRITE), 0);
			while (created < target) {
				char name[16];

				sprintf(name, "g%05u", (created * 7919u) % 100000u);
				CHECK_EQ(kres_create(&c, name, 1u, &r), 0);
				++created;
			}
			inactive = (uint8_t)(1u - c.active_area);
			inactive_start = c.catalog_start[inactive];
			mark = c.next_record;
			CHECK_EQ(kres_flush(&c), 0);
			CHECK_EQ(c.active_area, inactive);
			CHECK(c.catalog_start[inactive] != inactive_start);
			CHECK_EQ(c.catalog_start[inactive], mark);
			while (capacity * 8u < created) {
				capacity = (uint16_t)(capacity * 2u);
			}
			CHECK_EQ(c.catalog_records[inactive], capacity);
			/* The other area is then too small too; the next
			 * flush relocates it to the same capacity. */
			CHECK_EQ(kres_create(&c, "zz-one-more", 1u, &r), 0);
			CHECK_EQ(kres_delete(&c, "zz-one-more"), 0);
			CHECK_EQ(kres_flush(&c), 0);
			CHECK_EQ(c.catalog_records[1u - inactive], capacity);
			CHECK_EQ(kres_close(&c), 0);
			CHECK_EQ(kres_verify("grow.kpr", &usage), 0);
			CHECK(usage.abandoned_records > 0u);
		}
	}
	/* Reuse: with room in the inactive area, a flush relocates
	 * nothing and allocates nothing. */
	CHECK_EQ(kres_open(&c, "grow.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	for (i = 0u; i < 4u; ++i) {
		uint16_t starts[2];
		uint32_t mark;
		char name[16];

		sprintf(name, "reuse%u", i);
		CHECK_EQ(kres_create(&c, name, 1u, &r), 0);
		starts[0] = c.catalog_start[0];
		starts[1] = c.catalog_start[1];
		mark = c.next_record;
		CHECK_EQ(kres_flush(&c), 0);
		CHECK_EQ(c.catalog_start[0], starts[0]);
		CHECK_EQ(c.catalog_start[1], starts[1]);
		CHECK_EQ(c.next_record, mark);
	}
	CHECK_EQ(kres_close(&c), 0);
	/* Compaction after the relocations. */
	CHECK_EQ(kres_verify("grow.kpr", &usage), 0);
	CHECK(usage.abandoned_records > 0u);
	i = usage.resources;
	CHECK_EQ(kres_compact("grow.kpr", KRES_TIGHT), 0);
	CHECK_EQ(kres_verify("grow.kpr", &usage), 0);
	CHECK_EQ(usage.resources, i);
	CHECK_EQ(usage.abandoned_records, 0u);
	CHECK_EQ(usage.catalog_records, 2u * ((i + 7u) / 8u));
	CHECK_EQ(usage.next_record, 1u + usage.catalog_records + i);
}

#if defined(KRES_TEST_XMEM)
/* The XMEM store's ceiling: 4,096 entries (16 chunks); the 4,097th
 * create is refused, the container unchanged. */
static void xmem_ceiling(void)
{
	kres_container c;
	kres_resource r;
	unsigned int i;

	install();
	CHECK_EQ(kres_create_container("big.kpr"), 0);
	CHECK_EQ(kres_open(&c, "big.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	for (i = 0u; i < 4096u; ++i) {
		char name[16];
		int result;

		sprintf(name, "e%05u", i);
		result = kres_create(&c, name, 1u, &r);
		CHECK_EQ(result, 0);
		if (result != 0) {
			break;
		}
	}
	CHECK_EQ(xmem_shim_objects(), 16u);
	CHECK_EQ(kres_create(&c, "e99999", 1u, &r), -KOS_ENOSPC);
	CHECK_EQ(c.resource_count, 4096u);
	CHECK_EQ(kres_close(&c), 0);
	CHECK_EQ(xmem_shim_objects(), 0u);
	CHECK_EQ(kres_verify("big.kpr", (struct kres_usage *)0), 0);
	/* Exhausted XMEM: the open of a 4,096-entry container fails
	 * cleanly and frees what it took. */
	xmem_shim_limit(64u * 1024u);
	CHECK_EQ(kres_open(&c, "big.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 -KOS_ENOSPC);
	CHECK_EQ(xmem_shim_objects(), 0u);
	xmem_shim_limit(0u);
	/* A read-only open needs no XMEM at all. */
	CHECK_EQ(kres_open(&c, "big.kpr", KRES_OPEN_READ), 0);
	CHECK_EQ(kres_get(&c, "e04095", &r, KRES_OPEN_READ), 0);
	CHECK_EQ(xmem_shim_objects(), 0u);
	CHECK_EQ(kres_close(&c), 0);
	printf("xmem: peak %lu bytes over the run\n", xmem_shim_peak());
}
#endif

int main(void)
{
	unsigned int i;

	for (i = 0u; i < sizeof(counts) / sizeof(counts[0]); ++i) {
		sealed_case(counts[i]);
		mutable_case(counts[i]);
	}
	byte_order();
	mutations();
#if defined(KRES_TEST_XMEM)
	xmem_ceiling();
#endif
	TEST_DONE();
}
