/* KRES: the read-only link proof (card program).
 *
 * A sealed-container reader that installs the read side only -
 * `kres_install(&kres_io_konaos_read, &kres_cache_xmem, &ctx)` - so
 * the link map must carry none of the mutation, flush, builder,
 * compaction, extract or verify members.  tests/test_kres_native.py
 * runs it from the shell and checks its map.
 *
 *   kresread            list /fs/BUILT.KPR and print each resource's
 *                       length and checksum (the same pattern as
 *                       krestest read)
 */

#include "kos.h"

#include "kos/kres.h"
#include "kos/kres_konaos.h"
#include "kos/errno.h"

#define BUILT "/fs/BUILT.KPR"
#define CHUNK 512u

static unsigned char kresread_ctx;
static unsigned char kresread_buffer[CHUNK];

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
		result = kres_read(r, kresread_buffer, (uint16_t)chunk);
		if (result != (int)chunk) {
			return result < 0 ? result : -KOS_EIO;
		}
		{
			unsigned int i;

			for (i = 0u; i < chunk; ++i) {
				sum = (sum + kresread_buffer[i]) & 0xFFFFu;
			}
		}
		at = (uint32_t)(at + chunk);
	}
	*sum_out = sum;
	return 0;
}

int main(void)
{
	struct kres_iter it;
	kres_container c;
	int result;

	kres_install(&kres_io_konaos_read, &kres_cache_xmem, &kresread_ctx);
	result = kres_open(&c, BUILT, KRES_OPEN_READ);
	printf("kresread: open %d\r\n", result);
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
		printf("kresread: entry %s %u %u %d\r\n",
		       (const char *)info.name, (unsigned int)info.used_bytes,
		       sum, got);
	}
	result = kres_close(&c);
	printf("kresread: close %d\r\n", result);
	return 0;
}
