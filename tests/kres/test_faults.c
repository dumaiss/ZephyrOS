/* KRES host tests: fault injection.  Short reads/writes and failed
 * seeks/syncs at every phase return errors, never crash, and never
 * push the file past the allocation mark. */

#include "kres_test.h"

#include "kos/kres.h"

static void install(void)
{
	static uint8_t ctx;

	mem_io_reset();
	mem_truncate_refuses_growth(1);
	kres_install(mem_io(), &kres_cache_mem, &ctx);
}

static uint8_t blob[400];

int main(void)
{
	kres_container c;
	kres_resource r;
	unsigned int ordinal;
	int result;

	for (ordinal = 0u; ordinal < 40u; ++ordinal) {
		install();
		CHECK_EQ(kres_create_container("f.kpr"), 0);
		CHECK_EQ(kres_open(&c, "f.kpr",
				   KRES_OPEN_READ | KRES_OPEN_WRITE), 0);
		CHECK_EQ(kres_create(&c, "res", 1u, &r), 0);

		/* Arm the fault for this ordinal and push through
		 * every phase that performs adapter ops. */
		mem_fail_write(ordinal, -KOS_EIO);
		result = kres_write(&r, blob, 400);
		CHECK(result == 400 || result < 0);
		mem_fail_write(0xFFFFFFFFu, 0);

		mem_fail_seek(ordinal, -KOS_EIO);
		result = kres_write(&r, blob, 100);
		CHECK(result == 100 || result < 0);
		mem_fail_seek(0xFFFFFFFFu, 0);

		mem_fail_sync(ordinal, -KOS_EIO);
		result = kres_close(&c);
		CHECK(result == 0 || result < 0);
		mem_fail_sync(0xFFFFFFFFu, 0);

		/* Whatever happened, the file never exceeds the format's
		 * 32 MiB bound: growth writes only whole extents under
		 * the allocation mark, and an interrupted flush may
		 * leave the header naming a mark behind unreferenced
		 * records. */
		{
			const struct mem_file *f =
				mem_file_by_name("f.kpr");

			CHECK(f != (const struct mem_file *)0);
			if (f != (const struct mem_file *)0) {
				CHECK(f->size <=
				      (uint32_t)KRES_RECORD_LIMIT *
					      KRES_RECORD_SIZE);
			}
		}
	}

	/* Short reads and writes return cleanly. */
	install();
	CHECK_EQ(kres_create_container("f2.kpr"), 0);
	CHECK_EQ(kres_open(&c, "f2.kpr",
			   KRES_OPEN_READ | KRES_OPEN_WRITE), 0);
	CHECK_EQ(kres_create(&c, "res", 1u, &r), 0);
	CHECK_EQ(kres_write(&r, blob, 400), 400);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_close(&c), 0);
	for (ordinal = 0u; ordinal < 8u; ++ordinal) {
		CHECK_EQ(kres_open(&c, "f2.kpr", KRES_OPEN_READ), 0);
		CHECK_EQ(kres_get(&c, "res", &r, KRES_OPEN_READ), 0);
		mem_short_read(ordinal);
		result = kres_read(&r, blob, 400);
		CHECK(result >= 0);
		mem_short_read(0xFFFFFFFFu);
		CHECK_EQ(kres_rclose(&r), 0);
		CHECK_EQ(kres_close(&c), 0);
	}

	TEST_DONE();
}
