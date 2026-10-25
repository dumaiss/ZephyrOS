/* KRES host tests: the commit order (§5.6).  Every adapter write a
 * flush performs is failed in turn; after each failure the abandoned
 * file must still open on the previous committed catalog. */

#include "kres_test.h"

#include "kos/kres.h"

static void install(void)
{
	static uint8_t ctx;

	mem_io_reset();
	mem_truncate_refuses_growth(1);
	kres_install_mut(mem_io(), &kres_cache_mem, &ctx);
}

int main(void)
{
	kres_container c;
	kres_resource r;
	struct kres_iter it;
	struct kres_info info;
	unsigned int fail_at;
	unsigned int writes_seen;
	unsigned int names_seen;
	int result;

	install();

	for (fail_at = 0u; fail_at < 5u; ++fail_at) {
		/* A fresh container with one committed resource. */
		mem_io_reset();
		mem_truncate_refuses_growth(1);
		kres_install_mut(mem_io(), &kres_cache_mem, &(uint8_t){0});
		CHECK_EQ(kres_create_container("c.kpr"), 0);
		CHECK_EQ(kres_open(&c, "c.kpr",
				   KRES_OPEN_READ | KRES_OPEN_WRITE), 0);
		CHECK_EQ(kres_create(&c, "keep", 1u, &r), 0);
		CHECK_EQ(kres_write(&r, "KEEP", 4), 4);
		CHECK_EQ(kres_rclose(&r), 0);
		CHECK_EQ(kres_close(&c), 0);

		/* A mutation, then a flush whose `fail_at`-th write
		 * fails; the container is abandoned like a crash. */
		CHECK_EQ(kres_open(&c, "c.kpr",
				   KRES_OPEN_READ | KRES_OPEN_WRITE), 0);
		CHECK_EQ(kres_create(&c, "lose", 1u, &r), 0);
		CHECK_EQ(kres_write(&r, "LOSE", 4), 4);
		CHECK_EQ(kres_rclose(&r), 0);
		writes_seen = mem_count_writes();
		mem_fail_write(writes_seen + fail_at, -KOS_EIO);
		result = kres_flush(&c);
		CHECK(result < 0);
		mem_fail_write(0xFFFFFFFFu, 0);
		CHECK_EQ(mem_io()->close(&c), 0); /* the crash */

		/* The previous committed catalog survives. */
		CHECK_EQ(kres_open(&c, "c.kpr", KRES_OPEN_READ), 0);
		CHECK_EQ(c.resource_count, 1u);
		CHECK_EQ(kres_get(&c, "keep", &r, KRES_OPEN_READ), 0);
		CHECK_EQ(kres_read(&r, (uint8_t[8]){0}, 4), 4);
		CHECK_EQ(kres_rclose(&r), 0);
		CHECK_EQ(kres_get(&c, "lose", &r, KRES_OPEN_READ),
			 -KOS_ENOENT);
		CHECK_EQ(kres_verify("c.kpr"), 0);
		CHECK_EQ(kres_close(&c), 0);
		names_seen = 0u;
		CHECK_EQ(kres_open(&c, "c.kpr", KRES_OPEN_READ), 0);
		CHECK_EQ(kres_iter_begin(&c, &it), 0);
		while (kres_iter_next(&it, &info) > 0) {
			++names_seen;
		}
		CHECK_EQ(names_seen, 1u);
		CHECK_EQ(kres_close(&c), 0);
	}

	TEST_DONE();
}
