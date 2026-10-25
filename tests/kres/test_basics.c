/* KRES host tests: the design §11 basic rows — create/open/list,
 * names, logical cursors, contiguous allocation, update in capacity,
 * overflow growth, resize, delete/rename, catalog full, buffered I/O
 * across records and the partial final record. */

#include "kres_test.h"

#include "kos/kres.h"

static void install(void)
{
	static uint8_t ctx;

	mem_io_reset();
	mem_truncate_refuses_growth(1);
	kres_install(mem_io(), &kres_cache_mem, &ctx);
}

static uint8_t pattern[1500];

static void fill_pattern(void)
{
	unsigned int i;

	for (i = 0u; i < sizeof(pattern); ++i) {
		pattern[i] = (uint8_t)(i * 31u + 7u);
	}
}

static int make_container(const char *path)
{
	int result = kres_create_container(path);

	CHECK_EQ(result, 0);
	return result;
}

int main(void)
{
	kres_container c;
	kres_resource r;
	kres_resource r2;
	struct kres_iter it;
	struct kres_info info;
	uint8_t buf[1600];
	unsigned int i;
	int result;

	install();
	fill_pattern();

	/* Create/open/list. */
	CHECK_EQ(make_container("base.kpr"), 0);
	CHECK_EQ(kres_open(&c, "base.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	CHECK_EQ(c.resource_count, 0u);
	CHECK_EQ(kres_create(&c, "one", 2u, &r), 0);
	CHECK_EQ(kres_write(&r, "hello", 5), 5);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_create(&c, "two", 1u, &r), 0);
	CHECK_EQ(kres_write(&r, "world!", 6), 6);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_close(&c), 0);
	CHECK_EQ(kres_open(&c, "base.kpr", KRES_OPEN_READ), 0);
	CHECK_EQ(c.resource_count, 2u);
	CHECK_EQ(kres_iter_begin(&c, &it), 0);
	i = 0u;
	while (kres_iter_next(&it, &info) > 0) {
		++i;
	}
	CHECK_EQ(i, 2u);
	CHECK_EQ(kres_close(&c), 0);
	/* The header bytes (§4.2). */
	{
		const struct mem_file *f = mem_file_by_name("base.kpr");

		CHECK(f != (const struct mem_file *)0);
		CHECK(f != (const struct mem_file *)0 &&
		      memcmp(f->data, "KRES", 4) == 0);
		CHECK(f != (const struct mem_file *)0 &&
		      f->data[4] == 0x01 && f->data[5] == 0x00);
		CHECK(f != (const struct mem_file *)0 &&
		      f->data[8] == 0x00 && f->data[9] == 0x02);
		CHECK(f != (const struct mem_file *)0 &&
		      f->data[10] == 0x04 && f->data[11] == 0x00);
	}

	/* Names. */
	CHECK_EQ(kres_open(&c, "base.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	CHECK_EQ(kres_create(&c, "a", 1u, &r), 0);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_create(&c,
		"123456789012345678901234567890123456789012345678",
		1u, &r), 0);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_create(&c,
		"1234567890123456789012345678901234567890123456789",
		1u, &r), -KOS_EINVAL); /* 49 bytes */
	CHECK_EQ(kres_create(&c, "", 1u, &r), -KOS_EINVAL);
	CHECK_EQ(kres_create(&c, "src/MAIN.MOD", 1u, &r), 0); /* '/' opaque */
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_create(&c, "one", 1u, &r), -KOS_EEXIST);
	CHECK_EQ(kres_create(&c, "one\0two", 1u, &r), -KOS_EEXIST);
	/* No NUL inside a name: the C string ends at the NUL, so the
	 * duplicate "one" is what the check reports. */
	CHECK_EQ(kres_close(&c), 0);

	/* Independent cursors on one physical handle. */
	{
		CHECK_EQ(kres_open(&c, "base.kpr", KRES_OPEN_READ), 0);
		CHECK_EQ(kres_get(&c, "one", &r, KRES_OPEN_READ), 0);
		CHECK_EQ(kres_get(&c, "one", &r2, KRES_OPEN_READ), 0);
		CHECK_EQ(kres_read(&r, buf, 2), 2);
		CHECK_EQ(kres_read(&r2, buf, 5), 5);
		CHECK_EQ(buf[0], (uint8_t)'h');
		CHECK_EQ(kres_seek(&r, 1u), 0);
		CHECK_EQ(kres_read(&r, buf, 3), 3);
		CHECK_EQ(buf[0], (uint8_t)'e');
		CHECK_EQ(kres_rclose(&r), 0);
		CHECK_EQ(kres_rclose(&r2), 0);
		CHECK_EQ(kres_close(&c), 0);
	}

	/* Contiguous allocation and the allocation mark. */
	CHECK_EQ(kres_open(&c, "base.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	CHECK_EQ(kres_create(&c, "A", 2u, &r), 0);
	CHECK_EQ(r.start_record, 15u);
	CHECK_EQ(r.allocated_records, 2u);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_create(&c, "B", 1u, &r), 0);
	CHECK_EQ(r.start_record, 17u);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_create(&c, "C", 3u, &r), 0);
	CHECK_EQ(r.start_record, 18u);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(c.next_record, 21u);

	/* Update in capacity: the extent start does not move. */
	CHECK_EQ(kres_get(&c, "A", &r, KRES_OPEN_WRITE), 0);
	CHECK_EQ(r.start_record, 15u);
	CHECK_EQ(kres_write(&r, pattern, 1000), 1000);
	CHECK_EQ(r.start_record, 15u);
	CHECK_EQ(kres_rclose(&r), 0);

	/* Overflow growth: a fresh extent at the mark, bytes preserved. */
	CHECK_EQ(kres_get(&c, "A", &r, KRES_OPEN_WRITE), 0);
	CHECK_EQ(kres_write(&r, pattern, 1200), 1200);
	CHECK_EQ(r.start_record, 21u);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(c.next_record, 25u); /* grown capacity: 2 -> 3 -> 4 */
	CHECK_EQ(kres_get(&c, "A", &r, KRES_OPEN_READ), 0);
	CHECK_EQ(r.used_bytes, 1200u);
	CHECK_EQ(kres_seek(&r, 0u), 0);
	CHECK_EQ(kres_read(&r, buf, 1200), 1200);
	CHECK(memcmp(buf, pattern, 1200) == 0);
	CHECK_EQ(kres_rclose(&r), 0);

	/* Resize: grow zero-fills, shrink keeps the capacity and the EOF
	 * honors the logical length. */
	CHECK_EQ(kres_get(&c, "two", &r, KRES_OPEN_WRITE), 0);
	CHECK_EQ(kres_resize(&r, 1000u), 0);
	CHECK_EQ(kres_seek(&r, 0u), 0);
	CHECK_EQ(kres_read(&r, buf, 16), 16); /* six bytes, then zeros */
	CHECK_EQ(buf[0], (uint8_t)'w');
	CHECK_EQ(buf[5], (uint8_t)'!');
	CHECK_EQ(buf[6], (uint8_t)0);
	CHECK_EQ(kres_seek(&r, 996u), 0);
	CHECK_EQ(kres_read(&r, buf, 16), 4); /* the zero-filled tail */
	CHECK_EQ(buf[0], (uint8_t)0);
	CHECK_EQ(kres_resize(&r, 3u), 0);
	CHECK_EQ(kres_seek(&r, 0u), 0);
	CHECK_EQ(kres_read(&r, buf, 64), 3);
	CHECK_EQ(r.allocated_records, 2u); /* capacity kept */
	CHECK_EQ(kres_rclose(&r), 0);

	/* Delete/rename: catalog-only. */
	CHECK_EQ(kres_get(&c, "B", &r, KRES_OPEN_READ), 0);
	CHECK_EQ(r.start_record, 17u);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_rename(&c, "B", "B2"), 0);
	CHECK_EQ(kres_get(&c, "B", &r, KRES_OPEN_READ), -KOS_ENOENT);
	CHECK_EQ(kres_get(&c, "B2", &r, KRES_OPEN_READ), 0);
	CHECK_EQ(r.start_record, 17u);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_delete(&c, "B2"), 0);
	CHECK_EQ(kres_get(&c, "B2", &r, KRES_OPEN_READ), -KOS_ENOENT);
	CHECK_EQ(kres_close(&c), 0);

	/* Catalog full: the 33rd create. */
	CHECK_EQ(make_container("full.kpr"), 0);
	CHECK_EQ(kres_open(&c, "full.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	for (i = 0u; i < KRES_RESOURCE_MAX; ++i) {
		char name[8];

		sprintf(name, "r%u", i);
		result = kres_create(&c, name, 1u, &r);
		CHECK_EQ(result, 0);
		if (result == 0) {
			CHECK_EQ(kres_rclose(&r), 0);
		}
	}
	CHECK_EQ(c.resource_count, 32u);
	CHECK_EQ(kres_create(&c, "r32", 1u, &r), -KRES_E_FULL);
	CHECK_EQ(kres_close(&c), 0);

	/* Buffered I/O across records and the partial final record. */
	CHECK_EQ(kres_open(&c, "base.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	CHECK_EQ(kres_create(&c, "buf", 1u, &r), 0);
	CHECK_EQ(kres_write(&r, pattern, 1500), 1500);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_get(&c, "buf", &r, KRES_OPEN_READ), 0);
	memset(buf, 0xCC, sizeof(buf));
	CHECK_EQ(kres_seek(&r, 0u), 0);
	CHECK_EQ(kres_read(&r, buf, 700), 700);
	CHECK_EQ(kres_read(&r, buf + 700, 900), 800);
	CHECK(memcmp(buf, pattern, 1500) == 0);
	/* Read past the logical EOF returns what remains. */
	CHECK_EQ(kres_read(&r, buf, 16), 0);
	CHECK_EQ(kres_seek(&r, 1490u), 0);
	CHECK_EQ(kres_read(&r, buf, 32), 10);
	/* A write past the logical EOF is refused. */
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_get(&c, "buf", &r, KRES_OPEN_WRITE), 0);
	CHECK_EQ(kres_seek(&r, 1501u), 0);
	CHECK_EQ(kres_write(&r, pattern, 1), -KOS_EINVAL);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_close(&c), 0);

	/* Record operations with a partial final record. */
	CHECK_EQ(kres_open(&c, "base.kpr", KRES_OPEN_READ | KRES_OPEN_WRITE),
		 0);
	CHECK_EQ(kres_get(&c, "buf", &r, KRES_OPEN_READ), 0);
	memset(buf, 0xDD, sizeof(buf));
	CHECK_EQ(kres_read_records(&r, 0u, 2u, buf), 1024);
	CHECK(memcmp(buf, pattern, 1024) == 0);
	CHECK_EQ(kres_read_records(&r, 2u, 2u, buf), 476);
	CHECK(memcmp(buf, pattern + 1024, 476) == 0);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_close(&c), 0);

	TEST_DONE();
}
