/* KRES KonaOS edition outcome program (card program, run from the
 * production shell by tests/test_kres_native.py).
 *
 *   krestest build     a sealed container /fs/BUILT.KPR: INFO (64
 *                      bytes) and DATA (1,500 bytes), then verify
 *   krestest edit      /fs/EDITED.KPR: create, write past the
 *                      capacity (growth), rename, delete, flush,
 *                      close; and the two-slot rule: a direct file
 *                      open beside the container works, a second
 *                      container is refused
 *   krestest edit2     reopen /fs/EDITED.KPR, rename B and flush
 *                      (the mid-flush cutoff replay's mode)
 *   krestest read      list /fs/BUILT.KPR and print each resource's
 *                      length and checksum
 *   krestest compact   rebuild /fs/EDITED.KPR and verify it
 *   krestest extract   B out of /fs/EDITED.KPR into /fs/EXTRACT.BIN
 *   krestest readonly  open the sealed /fs/BUILT.KPR read-only, read
 *                      DATA, attempt a write and a create (both
 *                      refused)
 *
 * The tests compute the two patterns the resources hold:
 *
 *   INFO: byte i = (i*3+1) & 0xFF, 64 bytes
 *   DATA: byte i = (i*7+5) & 0xFF, 1,500 bytes; edit writes the
 *   first 1,000 bytes of the same pattern into B
 */

#include "kos.h"

#include "kos/kres.h"
#include "kos/kres_konaos.h"
#include "kos/errno.h"

#define BUILT     "/fs/BUILT.KPR"
#define EDITED    "/fs/EDITED.KPR"
#define EXTRA     "/fs/EXTRA.BIN"
#define EXTRACTED "/fs/EXTRACT.BIN"

#define INFO_BYTES 64u
#define DATA_BYTES 1500u
#define EDIT_BYTES 1000u
#define CHUNK      512u

/* The install context (the adapter keys its two slots by container,
 * not by this). */
static unsigned char krestest_ctx;

/* The transfer buffer (static: 512 bytes on the stack would exceed
 * the reservation). */
static unsigned char krestest_buffer[CHUNK];

static unsigned char info_byte(unsigned int i)
{
	return (unsigned char)((i * 3u + 1u) & 0xFFu);
}

static unsigned char data_byte(unsigned int i)
{
	return (unsigned char)((i * 7u + 5u) & 0xFFu);
}

/* The data pattern into the buffer: `bytes` from `base`. */
static void fill_data(unsigned int base, unsigned int bytes)
{
	unsigned int i;

	for (i = 0u; i < bytes; ++i) {
		krestest_buffer[i] = data_byte(base + i);
	}
}

/* `bytes` of the data pattern from `base` through `r`, chunked. */
static int write_data(kres_resource *r, unsigned int bytes,
		      unsigned int base)
{
	unsigned int at = 0u;

	while (at < bytes) {
		unsigned int chunk = bytes - at;
		int result;

		if (chunk > CHUNK) {
			chunk = CHUNK;
		}
		fill_data(base + at, chunk);
		result = kres_write(r, krestest_buffer, (uint16_t)chunk);
		if (result < 0) {
			return result;
		}
		if (result != (int)chunk) {
			return -KOS_EIO;
		}
		at = (unsigned int)(at + chunk);
	}
	return 0;
}

/* The same through the sealed builder. */
static int build_data(unsigned int bytes, unsigned int base)
{
	unsigned int at = 0u;

	while (at < bytes) {
		unsigned int chunk = bytes - at;
		int result;

		if (chunk > CHUNK) {
			chunk = CHUNK;
		}
		fill_data(base + at, chunk);
		result = kres_build_write(krestest_buffer, (uint16_t)chunk);
		if (result < 0) {
			return result;
		}
		at = (unsigned int)(at + chunk);
	}
	return 0;
}

/* The whole resource through the buffer, summed mod 2^16. */
static int read_sum(kres_resource *r, unsigned int *sum_out)
{
	unsigned int sum = 0u;
	uint32_t at = 0u;

	while (at < r->used_bytes) {
		unsigned int chunk = (unsigned int)(r->used_bytes - at);
		int result;

		if (chunk > CHUNK) {
			chunk = CHUNK;
		}
		result = kres_read(r, krestest_buffer, (uint16_t)chunk);
		if (result != (int)chunk) {
			return result < 0 ? result : -KOS_EIO;
		}
		{
			unsigned int i;

			for (i = 0u; i < chunk; ++i) {
				sum = (sum + krestest_buffer[i]) & 0xFFFFu;
			}
		}
		at = (uint32_t)(at + chunk);
	}
	*sum_out = sum;
	return 0;
}

static int krestest_build(void)
{
	unsigned int i;
	int result;

	result = kres_build_begin(BUILT, 2);
	printf("krestest: build-begin %d\r\n", result);
	if (result < 0) {
		return 1;
	}
	result = kres_build_add("INFO");
	printf("krestest: build-add %d\r\n", result);
	for (i = 0u; i < INFO_BYTES; ++i) {
		krestest_buffer[i] = info_byte(i);
	}
	result = kres_build_write(krestest_buffer, (uint16_t)INFO_BYTES);
	printf("krestest: build-write %d\r\n", result);
	result = kres_build_add("DATA");
	printf("krestest: build-add2 %d\r\n", result);
	result = build_data(DATA_BYTES, 0u);
	printf("krestest: build-write2 %d\r\n", result);
	result = kres_build_end();
	printf("krestest: build-end %d\r\n", result);
	result = kres_verify(BUILT);
	printf("krestest: verify %d\r\n", result);
	return 0;
}

static int krestest_edit(void)
{
	kres_container c1;
	kres_container c2;
	kres_resource r;
	kres_resource r2;
	int extra;
	int result;

	extra = open(EXTRA, (unsigned char)(KFS_O_WRITE | KFS_O_CREATE));
	printf("krestest: edit-extra-open %d\r\n", extra);
	result = kres_open(&c1, EDITED,
			   KRES_OPEN_READ | KRES_OPEN_WRITE |
				   KRES_OPEN_CREATE);
	printf("krestest: edit-open %d\r\n", result);
	if (result < 0) {
		return 1;
	}
	result = kres_create(&c1, "A", 1u, &r);
	printf("krestest: edit-create %d\r\n", result);
	if (result == 0) {
		result = write_data(&r, EDIT_BYTES, 0u);
		(void)kres_rclose(&r);
	}
	printf("krestest: edit-write %d\r\n", result);
	result = kres_rename(&c1, "A", "B");
	printf("krestest: edit-rename %d\r\n", result);
	result = kres_create(&c1, "C", 2u, &r2);
	if (result == 0) {
		(void)write_data(&r2, 400u, EDIT_BYTES);
		(void)kres_rclose(&r2);
	}
	printf("krestest: edit-create2 %d\r\n", result);
	result = kres_delete(&c1, "C");
	printf("krestest: edit-delete %d\r\n", result);
	/* The two-slot rule: EXTRA and the container hold both card
	 * slots, so a second container is refused. */
	result = kres_open(&c2, EDITED, KRES_OPEN_READ);
	if (result == 0) {
		(void)kres_close(&c2);
	}
	printf("krestest: edit-second-open %d\r\n", result);
	result = kres_flush(&c1);
	printf("krestest: edit-flush %d\r\n", result);
	result = kres_close(&c1);
	printf("krestest: edit-close %d\r\n", result);
	(void)close((unsigned int)extra);
	return 0;
}

/* The mid-flush cutoff replay's mode: reopen the committed
 * container, rename B and flush - the tests stop the machine after
 * the second catalog-area write and prove the previous catalog still
 * verifies. */
static int krestest_edit2(void)
{
	kres_container c;
	int result;

	result = kres_open(&c, EDITED, KRES_OPEN_READ | KRES_OPEN_WRITE);
	printf("krestest: edit2-open %d\r\n", result);
	if (result < 0) {
		return 1;
	}
	result = kres_rename(&c, "B", "B2");
	printf("krestest: edit2-rename %d\r\n", result);
	result = kres_flush(&c);
	printf("krestest: edit2-flush %d\r\n", result);
	result = kres_close(&c);
	printf("krestest: edit2-close %d\r\n", result);
	return 0;
}

static int krestest_read(void)
{
	struct kres_iter it;
	kres_container c;
	int result;

	result = kres_open(&c, BUILT, KRES_OPEN_READ);
	printf("krestest: read-open %d\r\n", result);
	if (result < 0) {
		return 1;
	}
	result = kres_iter_begin(&c, &it);
	if (result < 0) {
		(void)kres_close(&c);
		return 1;
	}
	for (;;) {
		struct kres_info info;
		kres_resource r;
		unsigned int sum = 0u;
		int got = kres_iter_next(&it, &info);

		if (got <= 0) {
			break;
		}
		got = kres_get(&c, (const char *)info.name, &r,
			       KRES_OPEN_READ);
		if (got == 0) {
			got = read_sum(&r, &sum);
			(void)kres_rclose(&r);
		}
		printf("krestest: entry %s %u %u %d\r\n",
		       (const char *)info.name, (unsigned int)info.used_bytes,
		       sum, got);
	}
	result = kres_close(&c);
	printf("krestest: read-close %d\r\n", result);
	return 0;
}

static int krestest_compact(void)
{
	int result;

	result = kres_compact(EDITED, 0u);
	printf("krestest: compact %d\r\n", result);
	result = kres_verify(EDITED);
	printf("krestest: verify %d\r\n", result);
	return 0;
}

static int krestest_extract(void)
{
	kres_container c;
	long size;
	int handle;
	int result;

	result = kres_open(&c, EDITED, KRES_OPEN_READ);
	printf("krestest: extract-open %d\r\n", result);
	if (result < 0) {
		return 1;
	}
	result = kres_extract(&c, "B", EXTRACTED);
	printf("krestest: extract %d\r\n", result);
	result = kres_close(&c);
	printf("krestest: extract-close %d\r\n", result);
	handle = open(EXTRACTED, KFS_O_READ);
	if (handle < 0) {
		printf("krestest: extract-size %d\r\n", handle);
		return 1;
	}
	size = seek((unsigned int)handle, 0L, KFS_SEEK_END);
	(void)close((unsigned int)handle);
	printf("krestest: extract-size %d\r\n", (int)size);
	return 0;
}


static int krestest_readonly(void)
{
	kres_container c;
	kres_resource r;
	unsigned int sum = 0u;
	int result;

	result = kres_open(&c, BUILT, KRES_OPEN_READ);
	printf("krestest: readonly-open %d\r\n", result);
	if (result < 0) {
		return 1;
	}
	result = kres_get(&c, "DATA", &r, KRES_OPEN_READ);
	printf("krestest: readonly-get %d\r\n", result);
	if (result == 0) {
		result = read_sum(&r, &sum);
	}
	printf("krestest: readonly-sum %u\r\n", sum);
	result = kres_write(&r, krestest_buffer, 1u);
	printf("krestest: readonly-write %d\r\n", result);
	result = kres_create(&c, "NOPE", 1u, &r);
	printf("krestest: readonly-create %d\r\n", result);
	(void)kres_rclose(&r);
	result = kres_close(&c);
	printf("krestest: readonly-close %d\r\n", result);
	return 0;
}

int main(int argc, char **argv)
{
	int result;

	/* argv holds the command-tail words only, so argv[0] is the
	 * mode. */
	result = kres_install_mut(&kres_io_konaos, &kres_cache_xmem,
				  &krestest_ctx);
	printf("krestest: install %d\r\n", result);
	if (argc < 1) {
		printf("krestest: build|edit|read|compact|extract|readonly"
		       "\r\n");
		return 1;
	}
	if (strcmp(argv[0], "build") == 0) {
		return krestest_build();
	}
	if (strcmp(argv[0], "edit") == 0) {
		return krestest_edit();
	}
	if (strcmp(argv[0], "edit2") == 0) {
		return krestest_edit2();
	}
	if (strcmp(argv[0], "read") == 0) {
		return krestest_read();
	}
	if (strcmp(argv[0], "compact") == 0) {
		return krestest_compact();
	}
	if (strcmp(argv[0], "extract") == 0) {
		return krestest_extract();
	}
	if (strcmp(argv[0], "readonly") == 0) {
		return krestest_readonly();
	}
	printf("krestest: unknown mode\r\n");
	return 1;
}
