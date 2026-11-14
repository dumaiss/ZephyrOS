/* KRES host tests: compaction policies and sealed containers (§7,
 * §8).  Preserve reservations, tight pack, seal, read-back byte for
 * byte, mutations refused, an interrupted build refused at open. */

#include "kres_test.h"

#include "kos/kres.h"

static void install(void)
{
	static uint8_t ctx;

	mem_io_reset();
	mem_truncate_refuses_growth(1);
	kres_install_mut(mem_io(), &kres_cache_mem, &ctx);
}

static uint8_t data_a[1200];
static uint8_t data_b[700];

static void fill(void)
{
	unsigned int i;

	for (i = 0u; i < sizeof(data_a); ++i) {
		data_a[i] = (uint8_t)(i * 13u + 1u);
	}
	for (i = 0u; i < sizeof(data_b); ++i) {
		data_b[i] = (uint8_t)(i * 17u + 3u);
	}
}

static void make_source(const char *path)
{
	kres_container c;
	kres_resource r;

	CHECK_EQ(kres_create_container(path), 0);
	CHECK_EQ(kres_open(&c, path, KRES_OPEN_READ | KRES_OPEN_WRITE), 0);
	CHECK_EQ(kres_create(&c, "A", 2u, &r), 0);
	CHECK_EQ(kres_write(&r, data_a, 1200), 1200);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_create(&c, "B", 2u, &r), 0);
	CHECK_EQ(kres_write(&r, data_b, 700), 700);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_close(&c), 0);
}

static void expect_bytes(const char *path)
{
	kres_container c;
	kres_resource r;
	uint8_t buf[1300];
	int result;

	CHECK_EQ(kres_open(&c, path, KRES_OPEN_READ), 0);
	CHECK_EQ(kres_get(&c, "A", &r, KRES_OPEN_READ), 0);
	CHECK_EQ(kres_read(&r, buf, 1200), 1200);
	CHECK(memcmp(buf, data_a, 1200) == 0);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_get(&c, "B", &r, KRES_OPEN_READ), 0);
	CHECK_EQ(kres_read(&r, buf, 700), 700);
	CHECK(memcmp(buf, data_b, 700) == 0);
	CHECK_EQ(kres_rclose(&r), 0);
	result = kres_close(&c);
	CHECK_EQ(result, 0);
}

int main(void)
{
	kres_container c;
	kres_resource r;
	uint8_t buf[1300];
	int result;

	install();
	fill();
	make_source("src.kpr");

	/* Default compaction preserves reservations. */
	CHECK_EQ(kres_compact("src.kpr", 0u), 0);
	expect_bytes("src.kpr");
	CHECK_EQ(kres_open(&c, "src.kpr", KRES_OPEN_READ), 0);
	{
		struct kres_info info;

		CHECK_EQ(kres_stat(&c, "A", &info), 0);
		CHECK_EQ(info.allocated_records, 4u); /* grown capacity */
		CHECK_EQ(kres_stat(&c, "B", &info), 0);
		CHECK_EQ(info.allocated_records, 2u); /* its reservation */
	}
	CHECK_EQ(kres_close(&c), 0);

	/* Tight compaction releases the slack. */
	CHECK_EQ(kres_compact("src.kpr", KRES_TIGHT), 0);
	expect_bytes("src.kpr");
	CHECK_EQ(kres_open(&c, "src.kpr", KRES_OPEN_READ), 0);
	{
		struct kres_info info;

		CHECK_EQ(kres_stat(&c, "A", &info), 0);
		CHECK_EQ(info.allocated_records, 3u); /* ceil(1200/512) */
		CHECK_EQ(kres_stat(&c, "B", &info), 0);
		CHECK_EQ(info.allocated_records, 2u); /* ceil(700/512) */
	}
	CHECK_EQ(kres_close(&c), 0);

	/* Seal: a read-only container with the same bytes. */
	CHECK_EQ(kres_compact("src.kpr", KRES_SEAL), 0);
	expect_bytes("src.kpr");
	CHECK_EQ(kres_open(&c, "src.kpr", KRES_OPEN_READ), 0);
	CHECK(c.sealed != 0u);
	CHECK_EQ(kres_create(&c, "C", 1u, &r), -KOS_EROFS);
	CHECK_EQ(kres_get(&c, "A", &r, KRES_OPEN_WRITE), -KOS_EROFS);
	CHECK_EQ(kres_get(&c, "A", &r, KRES_OPEN_READ), 0);
	CHECK_EQ(kres_write(&r, data_a, 1), -KOS_EINVAL); /* not writable */
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_close(&c), 0);

	/* The sealed builder: byte-for-byte content, mutations refused. */
	CHECK_EQ(kres_build_begin("built.kpr"), 0);
	CHECK_EQ(kres_build_add("A"), 0);
	CHECK_EQ(kres_build_write(data_a, 1200), 1200);
	CHECK_EQ(kres_build_add("B"), 0);
	CHECK_EQ(kres_build_write(data_b, 700), 700);
	CHECK_EQ(kres_build_end(), 0);
	expect_bytes("built.kpr");
	CHECK_EQ(kres_open(&c, "built.kpr", KRES_OPEN_READ), 0);
	CHECK(c.sealed != 0u);
	CHECK_EQ(kres_create(&c, "C", 1u, &r), -KOS_EROFS);
	CHECK_EQ(kres_get(&c, "A", &r, KRES_OPEN_WRITE), -KOS_EROFS);
	CHECK_EQ(kres_close(&c), 0);

	/* An interrupted build is refused at open. */
	CHECK_EQ(kres_build_begin("half.kpr"), 0);
	CHECK_EQ(kres_build_add("A"), 0);
	CHECK_EQ(kres_build_write(data_a, 100), 100);
	CHECK_EQ(kres_build_abort(), 0);
	CHECK_EQ(kres_open(&c, "half.kpr", KRES_OPEN_READ), -KRES_E_BAD);

	/* The builder has no count limit below the format's: 40
	 * resources, added out of order, come back sorted; a duplicate
	 * name is refused; an empty build is a valid empty container. */
	{
		struct kres_iter it;
		struct kres_info info;
		char previous[KRES_NAME_MAX + 1u];
		unsigned int i;

		CHECK_EQ(kres_build_begin("many.kpr"), 0);
		for (i = 0u; i < 40u; ++i) {
			char name[16];

			sprintf(name, "r%02u", (i * 17u) % 40u);
			result = kres_build_add(name);
			CHECK_EQ(result, 0);
			if (result != 0) {
				break;
			}
			if (i % 3u != 0u) { /* some stay empty */
				CHECK_EQ(kres_build_write(name, 3), 3);
			}
		}
		CHECK_EQ(kres_build_add("r07"), -KOS_EEXIST);
		CHECK_EQ(kres_build_end(), 0);
		CHECK_EQ(kres_verify("many.kpr", (struct kres_usage *)0), 0);
		CHECK_EQ(kres_open(&c, "many.kpr", KRES_OPEN_READ), 0);
		CHECK_EQ(c.resource_count, 40u);
		CHECK_EQ(kres_iter_begin(&c, &it), 0);
		previous[0] = '\0';
		i = 0u;
		while (kres_iter_next(&it, &info) > 0) {
			CHECK(strcmp(previous, (const char *)info.name) < 0);
			strcpy(previous, (const char *)info.name);
			++i;
		}
		CHECK_EQ(i, 40u);
		CHECK_EQ(kres_close(&c), 0);

		CHECK_EQ(kres_build_begin("empty.kpr"), 0);
		CHECK_EQ(kres_build_end(), 0);
		CHECK_EQ(kres_verify("empty.kpr", (struct kres_usage *)0), 0);
		CHECK_EQ(kres_open(&c, "empty.kpr", KRES_OPEN_READ), 0);
		CHECK_EQ(c.resource_count, 0u);
		CHECK_EQ(kres_get(&c, "x", &r, KRES_OPEN_READ), -KOS_ENOENT);
		CHECK_EQ(kres_close(&c), 0);

		CHECK_EQ(kres_build_begin("dup.kpr"), 0);
		CHECK_EQ(kres_build_add("same"), 0);
		CHECK_EQ(kres_build_write("y", 1), 1);
		CHECK_EQ(kres_build_add("same"), -KOS_EEXIST);
		CHECK_EQ(kres_build_abort(), 0);
	}

	/* verify passes on every completed form. */
	CHECK_EQ(kres_verify("src.kpr", (struct kres_usage *)0), 0);
	CHECK_EQ(kres_verify("built.kpr", (struct kres_usage *)0), 0);
	CHECK_EQ(kres_verify("dup.kpr", (struct kres_usage *)0), -KRES_E_BAD); /* aborted */
	(void)buf;
	TEST_DONE();
}
