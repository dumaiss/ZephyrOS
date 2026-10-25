/* KRES host tests: the randomized oracle.  10,000 random operations
 * against the in-memory adapter, with an in-memory name->bytes map as
 * the oracle; every 100 operations the container is verified and
 * every resource is compared byte for byte. */

#include "kres_test.h"

#include "kos/kres.h"

#define MAX_RES 32u
#define MAX_BYTES 3000u

struct oracle_entry {
	char name[16];
	uint8_t bytes[MAX_BYTES];
	uint32_t used;
	uint8_t live;
};

static struct oracle_entry oracle[MAX_RES];
static unsigned int oracle_count;

static uint32_t rng_state = 0x12345678u;

static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void install(void)
{
	static uint8_t ctx;

	mem_io_reset();
	mem_truncate_refuses_growth(1);
	kres_install(mem_io(), &kres_cache_mem, &ctx);
}

static struct oracle_entry *find_entry(const char *name)
{
	unsigned int i;

	for (i = 0u; i < MAX_RES; ++i) {
		if (oracle[i].live != 0u &&
		    strcmp(oracle[i].name, name) == 0) {
			return &oracle[i];
		}
	}
	return (struct oracle_entry *)0;
}

static struct oracle_entry *add_entry(const char *name)
{
	unsigned int i;

	for (i = 0u; i < MAX_RES; ++i) {
		if (oracle[i].live == 0u) {
			strncpy(oracle[i].name, name, sizeof(oracle[i].name) - 1u);
			oracle[i].used = 0u;
			oracle[i].live = 1u;
			return &oracle[i];
		}
	}
	return (struct oracle_entry *)0;
}

/* Compare every live resource against the file. */
static void compare_all(const char *path)
{
	kres_container c;
	struct kres_iter it;
	struct kres_info info;
	unsigned int compared = 0u;

	CHECK_EQ(kres_verify(path), 0);
	CHECK_EQ(kres_open(&c, path, KRES_OPEN_READ), 0);
	CHECK_EQ(c.resource_count, oracle_count);
	CHECK_EQ(kres_iter_begin(&c, &it), 0);
	while (kres_iter_next(&it, &info) > 0) {
		struct oracle_entry *entry =
			find_entry((const char *)info.name);
		kres_resource r;
		uint8_t buf[MAX_BYTES];
		int got;

		CHECK(entry != (struct oracle_entry *)0);
		if (entry == (struct oracle_entry *)0) {
			(void)kres_close(&c);
			return;
		}
		CHECK_EQ(info.used_bytes, entry->used);
		CHECK_EQ(kres_get(&c, (const char *)info.name, &r,
				  KRES_OPEN_READ), 0);
		got = kres_read(&r, buf, MAX_BYTES);
		CHECK_EQ((uint32_t)got, entry->used);
		if (got == (int)entry->used) {
			CHECK(memcmp(buf, entry->bytes, entry->used) == 0);
		}
		CHECK_EQ(kres_rclose(&r), 0);
		++compared;
	}
	CHECK_EQ(compared, oracle_count);
	CHECK_EQ(kres_close(&c), 0);
}

int main(void)
{
	static const char *const name_pool[8] = {
		"alpha", "beta", "gamma", "delta",
		"eps", "zeta", "eta", "theta",
	};
	kres_container c;
	unsigned int op;
	unsigned int live = 0u;
	int opened = 0;

	install();
	memset(oracle, 0, sizeof(oracle));
	CHECK_EQ(kres_create_container("rnd.kpr"), 0);

	for (op = 0u; op < 10000u; ++op) {
		unsigned int choice = rnd() % 10u;
		unsigned int which = rnd() % 8u;
		const char *name = name_pool[which];
		struct oracle_entry *entry = find_entry(name);
		int result;

		if (opened == 0) {
			result = kres_open(&c, "rnd.kpr",
					   KRES_OPEN_READ | KRES_OPEN_WRITE);
			if (result != 0) {
				fprintf(stderr, "open at op %u: %d\n", op,
					result);
				break;
			}
			opened = 1;
		}
		switch (choice) {
		case 0u: /* create (or duplicate) */
			result = kres_create(&c, name, 1u, &(kres_resource){0});
			if (entry == (struct oracle_entry *)0) {
				CHECK_EQ(result, 0);
				if (result == 0) {
					CHECK(add_entry(name) !=
					      (struct oracle_entry *)0);
					++oracle_count;
					++live;
				}
			} else {
				CHECK_EQ(result, -KOS_EEXIST);
			}
			break;
		case 1u: /* delete */
			result = kres_delete(&c, name);
			if (entry != (struct oracle_entry *)0) {
				CHECK_EQ(result, 0);
				if (result == 0) {
					entry->live = 0u;
					--oracle_count;
					--live;
				}
			} else {
				CHECK_EQ(result, -KOS_ENOENT);
			}
			break;
		case 2u: { /* append a random run */
			uint8_t chunk[64];
			uint16_t len = (uint16_t)(rnd() % 64u);
			unsigned int i;

			if (entry == (struct oracle_entry *)0) {
				break;
			}
			for (i = 0u; i < len; ++i) {
				chunk[i] = (uint8_t)rnd();
			}
			{
				kres_resource r;

				result = kres_get(&c, name, &r,
						  KRES_OPEN_WRITE);
				CHECK_EQ(result, 0);
				if (result != 0) {
					break;
				}
				CHECK_EQ(kres_seek(&r, entry->used), 0);
				if (len != 0u) {
					result = kres_write(&r, chunk, len);
					CHECK_EQ(result, (int)len);
					if (result == (int)len) {
						memcpy(entry->bytes +
						       entry->used, chunk,
						       len);
						entry->used += len;
					}
				}
				CHECK_EQ(kres_rclose(&r), 0);
			}
			break;
		}
		case 3u: { /* overwrite a random prefix */
			kres_resource r;
			uint32_t at;
			uint16_t len;

			if (entry == (struct oracle_entry *)0 ||
			    entry->used == 0u) {
				break;
			}
			at = rnd() % entry->used;
			len = (uint16_t)(rnd() % 64u);
			if (len > entry->used - at) {
				len = (uint16_t)(entry->used - at);
			}
			result = kres_get(&c, name, &r, KRES_OPEN_WRITE);
			CHECK_EQ(result, 0);
			if (result == 0) {
				uint8_t chunk[64];
				unsigned int i;

				for (i = 0u; i < len; ++i) {
					chunk[i] = (uint8_t)rnd();
				}
				CHECK_EQ(kres_seek(&r, at), 0);
				if (len != 0u) {
					result = kres_write(&r, chunk, len);
					CHECK_EQ(result, (int)len);
					if (result == (int)len) {
						memcpy(entry->bytes + at,
						       chunk, len);
					}
				}
				CHECK_EQ(kres_rclose(&r), 0);
			}
			break;
		}
		case 4u: { /* rename to the next pool name */
			const char *target = name_pool[(which + 1u) % 8u];
			struct oracle_entry *there = find_entry(target);

			if (strcmp(name, target) == 0) {
				break;
			}
			result = kres_rename(&c, name, target);
			if (entry == (struct oracle_entry *)0) {
				CHECK_EQ(result, -KOS_ENOENT);
			} else if (there != (struct oracle_entry *)0) {
				CHECK_EQ(result, -KOS_EEXIST);
			} else {
				CHECK_EQ(result, 0);
				if (result == 0) {
					strncpy(entry->name, target,
						sizeof(entry->name) - 1u);
				}
			}
			break;
		}
		case 5u: /* flush + reopen */
			CHECK_EQ(kres_close(&c), 0);
			opened = 0;
			break;
		case 6u: /* resize (small) */
			if (entry != (struct oracle_entry *)0) {
				kres_resource r;
				uint32_t target = rnd() % 200u;

				result = kres_get(&c, name, &r,
						  KRES_OPEN_WRITE);
				CHECK_EQ(result, 0);
				if (result == 0) {
					CHECK_EQ(kres_resize(&r, target), 0);
					if (target > entry->used) {
						memset(entry->bytes +
						       entry->used, 0,
						       target -
						       entry->used);
					}
					entry->used = target;
					CHECK_EQ(kres_rclose(&r), 0);
				}
			}
			break;
		case 7u: /* read back a run and compare */
			if (entry != (struct oracle_entry *)0 &&
			    entry->used != 0u) {
				kres_resource r;
				uint32_t at = rnd() % entry->used;
				uint32_t want = entry->used - at;
				uint8_t buf[64];
				int got;

				if (want > 64u) {
					want = 64u;
				}
				result = kres_get(&c, name, &r,
						  KRES_OPEN_READ);
				CHECK_EQ(result, 0);
				if (result == 0) {
					CHECK_EQ(kres_seek(&r, at), 0);
					got = kres_read(&r, buf,
							(uint16_t)want);
					CHECK_EQ(got, (int)want);
					if (got == (int)want) {
						CHECK(memcmp(buf,
							     entry->bytes + at,
							     want) == 0);
					}
					CHECK_EQ(kres_rclose(&r), 0);
				}
			}
			break;
		case 8u: /* compact */
			if (opened != 0) {
				CHECK_EQ(kres_close(&c), 0);
				opened = 0;
			}
			result = kres_compact("rnd.kpr",
					      (rnd() & 1u) != 0u
						      ? KRES_TIGHT
						      : 0u);
			CHECK_EQ(result, 0);
			compare_all("rnd.kpr");
			break;
		default:
			break;
		}
		if (op % 100u == 99u) {
			if (opened != 0) {
				CHECK_EQ(kres_close(&c), 0);
				opened = 0;
			}
			compare_all("rnd.kpr");
		}
	}
	if (opened != 0) {
		CHECK_EQ(kres_close(&c), 0);
	}
	compare_all("rnd.kpr");
	(void)live;
	TEST_DONE();
}
