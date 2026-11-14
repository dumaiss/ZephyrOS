/* KRES host tests: verify refusals (§10).  Deterministic corruption
 * of valid containers: the header fields (with the header check
 * recomputed, so the field rule itself is what refuses), the catalog
 * descriptors, the catalog order, names, entries and extents, overlaps
 * between catalog and resource extents, truncation; and the extent
 * accounting verify reports. */

#include "kres_test.h"

#include "kos/kres.h"

static void install(void)
{
	static uint8_t ctx;

	mem_io_reset();
	mem_truncate_refuses_growth(1);
	kres_install_mut(mem_io(), &kres_cache_mem, &ctx);
}

/* The library's own codecs, so the test re-signs edited headers. */
uint16_t kres_header_check(const uint8_t *header);

static void put16(uint8_t *at, uint16_t value)
{
	at[0] = (uint8_t)value;
	at[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *at, uint32_t value)
{
	at[0] = (uint8_t)value;
	at[1] = (uint8_t)(value >> 8);
	at[2] = (uint8_t)(value >> 16);
	at[3] = (uint8_t)(value >> 24);
}

static uint32_t get32(const uint8_t *at)
{
	return (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
	       ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static void resign(uint8_t *image)
{
	put16(image + 0x2C, kres_header_check(image));
}

/* A mutable container: alpha, beta, gamma, committed twice (the
 * second commit lands in area A again after B). */
static uint8_t *build_valid(const char *path, uint32_t *size)
{
	kres_container c;
	kres_resource r;
	const struct mem_file *f;
	uint8_t *copy;

	CHECK_EQ(kres_create_container(path), 0);
	CHECK_EQ(kres_open(&c, path, KRES_OPEN_READ | KRES_OPEN_WRITE), 0);
	CHECK_EQ(kres_create(&c, "beta", 1u, &r), 0);
	CHECK_EQ(kres_write(&r, "BB", 2), 2);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_create(&c, "alpha", 2u, &r), 0);
	CHECK_EQ(kres_write(&r, "AAAA", 4), 4);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_create(&c, "gamma", 1u, &r), 0);
	CHECK_EQ(kres_write(&r, "G", 1), 1);
	CHECK_EQ(kres_rclose(&r), 0);
	CHECK_EQ(kres_close(&c), 0);
	CHECK_EQ(kres_verify(path, (struct kres_usage *)0), 0);
	f = mem_file_by_name(path);
	*size = f->size;
	copy = (uint8_t *)malloc(f->size);
	memcpy(copy, f->data, f->size);
	return copy;
}

/* verify (and, unless `open_ok`, open) refuse the image. */
static void expect_refused_image(const uint8_t *image, uint32_t size,
				 int open_ok, int line)
{
	kres_container c;
	int verified;
	int opened;

	mem_file_new("bad.kpr", image, size);
	verified = kres_verify("bad.kpr", (struct kres_usage *)0);
	opened = kres_open(&c, "bad.kpr", KRES_OPEN_READ);
	if (opened == 0) {
		(void)kres_close(&c);
	}
	++checks;
	if (verified >= 0 || (!open_ok && opened >= 0)) {
		++failures;
		fprintf(stderr, "line %d: accepted (verify %d, open %d)\n",
			line, verified, opened);
	}
}

#define REFUSED(image, size) expect_refused_image(image, size, 0, __LINE__)
#define VERIFY_REFUSED(image, size) \
	expect_refused_image(image, size, 1, __LINE__)

int main(void)
{
	uint8_t *good;
	uint8_t *bad;
	uint32_t size;
	uint32_t catalog; /* byte offset of the active catalog */
	struct kres_usage usage;

	install();
	good = build_valid("good.kpr", &size);
	bad = (uint8_t *)malloc(size + 4096u);
	catalog = get32(good + (good[0x10] != 0u ? 0x24 : 0x1C)) * 512u;

	/* The accounting: header, two one-record areas, alpha's two
	 * records, beta's and gamma's one; nothing abandoned. */
	CHECK_EQ(kres_verify("good.kpr", &usage), 0);
	CHECK_EQ(usage.resources, 3u);
	CHECK_EQ(usage.catalog_records, 2u);
	CHECK_EQ(usage.live_records, 4u);
	CHECK_EQ(usage.abandoned_records, 0u);
	CHECK_EQ(usage.next_record, 7u);

	/* The header check: a torn or corrupt header. */
	memcpy(bad, good, size);
	bad[0x14] ^= 1u;                       /* next_record, unsigned */
	REFUSED(bad, size);
	memcpy(bad, good, size);
	bad[0x2C] ^= 0x40u;
	REFUSED(bad, size);

	/* Fixed fields (re-signed). */
	{
		static const struct {
			unsigned int at;
			uint8_t value;
		} fixed[] = {
			{ 0u, (uint8_t)'X' },   /* magic */
			{ 4u, 2u },             /* major */
			{ 6u, 1u },             /* minor */
			{ 9u, 0x01u },          /* record size */
			{ 10u, 4u },            /* reserved */
			{ 0x18u, 2u },          /* an unknown flag */
			{ 0x10u, 2u },          /* active area */
		};
		unsigned int i;

		for (i = 0u; i < sizeof(fixed) / sizeof(fixed[0]); ++i) {
			memcpy(bad, good, size);
			bad[fixed[i].at] = fixed[i].value;
			resign(bad);
			REFUSED(bad, size);
		}
	}
	/* Reserved header bytes: open ignores them, verify refuses. */
	memcpy(bad, good, size);
	bad[0x11] = 1u;
	resign(bad);
	VERIFY_REFUSED(bad, size);
	memcpy(bad, good, size);
	bad[0x1FF] = 1u;
	VERIFY_REFUSED(bad, size);

	/* Catalog descriptors. */
	memcpy(bad, good, size);
	put32(bad + 0x1C, 0u);                 /* A over the header */
	resign(bad);
	REFUSED(bad, size);
	memcpy(bad, good, size);
	put32(bad + 0x24, 6000u);              /* B past next_record */
	resign(bad);
	REFUSED(bad, size);
	memcpy(bad, good, size);
	put32(bad + 0x28, 0xFFFFFFF0u);        /* overflowing extent */
	resign(bad);
	REFUSED(bad, size);
	memcpy(bad, good, size);
	put32(bad + 0x24, 1u);                 /* B over A */
	resign(bad);
	REFUSED(bad, size);
	memcpy(bad, good, size);
	put16(bad + 0x12, 9u);                 /* 9 entries, one record */
	resign(bad);
	REFUSED(bad, size);
	memcpy(bad, good, size);
	put32(bad + 0x20, 0u);                 /* a mutable zero area */
	resign(bad);
	REFUSED(bad, size);
	memcpy(bad, good, size);
	put32(bad + 0x14, 70000u);             /* past the record limit */
	resign(bad);
	REFUSED(bad, size);

	/* Sealed inconsistencies on a mutable layout. */
	memcpy(bad, good, size);
	put32(bad + 0x18, 1u);                 /* SEALED with area B */
	resign(bad);
	REFUSED(bad, size);

	/* Catalog entries: alpha, beta, gamma at the active area. */
	memcpy(bad, good, size);
	bad[catalog + 0x3Cu] = 0u;             /* empty name */
	VERIFY_REFUSED(bad, size);
	memcpy(bad, good, size);
	bad[catalog + 0x3Cu] = 49u;            /* long name */
	VERIFY_REFUSED(bad, size);
	memcpy(bad, good, size);
	bad[catalog + 0x3Du] = 1u;             /* flags */
	VERIFY_REFUSED(bad, size);
	memcpy(bad, good, size);
	bad[catalog + 0x3Eu] = 1u;             /* reserved */
	VERIFY_REFUSED(bad, size);
	memcpy(bad, good, size);
	bad[catalog + 1u] = 0u;                /* NUL inside "alpha" */
	VERIFY_REFUSED(bad, size);
	memcpy(bad, good, size);
	bad[catalog + 20u] = 'x';              /* nonzero name padding */
	VERIFY_REFUSED(bad, size);
	memcpy(bad, good, size);
	put32(bad + catalog + 0x30u, 60000u);  /* start */
	VERIFY_REFUSED(bad, size);
	memcpy(bad, good, size);
	put32(bad + catalog + 0x34u, 5000u);   /* allocated */
	VERIFY_REFUSED(bad, size);
	memcpy(bad, good, size);
	put32(bad + catalog + 0x34u, 0u);      /* zero allocation */
	VERIFY_REFUSED(bad, size);
	memcpy(bad, good, size);
	put32(bad + catalog + 0x38u, 4096u);   /* used > allocated */
	VERIFY_REFUSED(bad, size);
	memcpy(bad, good, size);
	put32(bad + catalog + 0x30u, 0u);      /* over the header */
	VERIFY_REFUSED(bad, size);
	/* Out of order: swap alpha and beta. */
	memcpy(bad, good, size);
	memcpy(bad + catalog, good + catalog + 64u, 64u);
	memcpy(bad + catalog + 64u, good + catalog, 64u);
	VERIFY_REFUSED(bad, size);
	/* A duplicate: beta twice. */
	memcpy(bad, good, size);
	memcpy(bad + catalog, good + catalog + 64u, 64u);
	VERIFY_REFUSED(bad, size);
	/* Overlaps: beta inside alpha; gamma over a catalog area. */
	memcpy(bad, good, size);
	put32(bad + catalog + 64u + 0x30u,
	      get32(good + catalog + 0x30u) + 1u);
	VERIFY_REFUSED(bad, size);
	memcpy(bad, good, size);
	put32(bad + catalog + 128u + 0x30u, get32(good + 0x24));
	VERIFY_REFUSED(bad, size);
	/* A resource over the allocation mark. */
	memcpy(bad, good, size);
	put32(bad + 0x14, 6u);
	resign(bad);
	VERIFY_REFUSED(bad, size);

	/* Truncation: the header only, and a file cut inside the
	 * catalog. */
	VERIFY_REFUSED(good, 512u);
	REFUSED(good, 100u);
	REFUSED(good, catalog + 32u);

	free(bad);
	free(good);

	/* A sealed container: the catalog ends the file and fills its
	 * records exactly. */
	CHECK_EQ(kres_build_begin("sealed.kpr"), 0);
	CHECK_EQ(kres_build_add("x"), 0);
	CHECK_EQ(kres_build_write("xx", 2), 2);
	CHECK_EQ(kres_build_add("y"), 0);
	CHECK_EQ(kres_build_end(), 0);
	CHECK_EQ(kres_verify("sealed.kpr", &usage), 0);
	CHECK_EQ(usage.next_record, 4u);   /* header, x, y, catalog */
	CHECK_EQ(usage.catalog_records, 1u);
	CHECK_EQ(usage.abandoned_records, 0u);
	{
		const struct mem_file *f = mem_file_by_name("sealed.kpr");

		size = f->size;
		good = (uint8_t *)malloc(size);
		memcpy(good, f->data, size);
	}
	bad = (uint8_t *)malloc(size + 512u);
	memcpy(bad, good, size);
	put32(bad + 0x0C, 1u);                 /* a sealed generation */
	resign(bad);
	REFUSED(bad, size);
	memcpy(bad, good, size);
	put32(bad + 0x24, 1u);                 /* a sealed area B */
	resign(bad);
	REFUSED(bad, size);
	memcpy(bad, good, size);
	put32(bad + 0x14, 5u);                 /* slack after the catalog */
	memset(bad + size, 0, 512u);
	resign(bad);
	VERIFY_REFUSED(bad, size + 512u);
	memcpy(bad, good, size);
	put32(bad + 0x20, 2u);                 /* an oversized catalog */
	put32(bad + 0x14, 5u);
	memset(bad + size, 0, 512u);
	resign(bad);
	VERIFY_REFUSED(bad, size + 512u);
	free(bad);
	free(good);
	TEST_DONE();
}
