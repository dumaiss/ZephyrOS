/* KRES core: rebuild compaction (private).
 *
 * kres_compact rebuilds `path` in a temporary file and swaps names
 * (design §7.1), preserving live reservations by default, with
 * KRES_TIGHT dropping reserved slack and KRES_SEAL producing a sealed
 * container.  An interruption at any step leaves at least one complete
 * valid container, so there is no in-place variant.
 */

#include "kres_internal.h"

/* The temporary and recovery spellings: the path with one suffix
 * character appended (the host edition has no 8.3 limit; the KonaOS
 * adapter maps these to on-card names). */
#define KRES_TEMP_SUFFIX '~'
#define KRES_RECOVERY_SUFFIX '#'

/* An appended name: the path plus one suffix character, in `out`
 * (bounded by `out_size`). */
static int make_swap_name(char *out, unsigned int out_size,
			  const char *path, char suffix)
{
	uint16_t length = 0u;

	while (path[length] != '\0') {
		if ((unsigned int)length + 2u >= out_size) {
			return -KOS_EINVAL;
		}
		out[length] = path[length];
		++length;
	}
	out[length] = suffix;
	out[length + 1u] = '\0';
	return 0;
}

/* Stream one resource's live bytes into `out` (an open descriptor). */
static int copy_bytes(kres_resource *in, kres_resource *out)
{
	uint32_t at = 0u;

	while (at < in->used_bytes) {
		uint16_t chunk = (uint16_t)(in->used_bytes - at);
		int result;

		if (chunk > KRES_RECORD_SIZE) {
			chunk = KRES_RECORD_SIZE;
		}
		if (kres_seek(in, at) < 0) {
			return -KOS_EIO;
		}
		result = kres_read(in, kres_shared, chunk);
		if (result < 0) {
			return result;
		}
		if (result != (int)chunk) {
			return -KOS_EIO;
		}
		if (kres_seek(out, at) < 0) {
			return -KOS_EIO;
		}
		result = kres_write(out, kres_shared, chunk);
		if (result < 0) {
			return result;
		}
		at = (uint32_t)(at + chunk);
	}
	return 0;
}

/* Copy every live resource under the chosen policy. */
static int copy_resources(kres_container *src, kres_container *dst,
			  unsigned int flags)
{
	struct kres_iter it;
	struct kres_info info;
	int result;

	result = kres_iter_begin(src, &it);
	if (result < 0) {
		return result;
	}
	while (kres_iter_next(&it, &info) != 0) {
		uint16_t capacity = (flags & KRES_TIGHT) != 0u
					    ? (uint16_t)((info.used_bytes +
							  KRES_RECORD_SIZE - 1u) /
							 KRES_RECORD_SIZE)
					    : (uint16_t)info.allocated_records;
		kres_resource in;
		kres_resource out;

		if (capacity == 0u) {
			capacity = 1u;
		}
		result = kres_get(src, (const char *)info.name, &in,
				  KRES_OPEN_READ);
		if (result < 0) {
			return result;
		}
		result = kres_create(dst, (const char *)info.name, capacity,
				     &out);
		if (result < 0) {
			(void)kres_rclose(&in);
			return result;
		}
		result = copy_bytes(&in, &out);
		if (result == 0) {
			(void)kres_rclose(&out);
		}
		(void)kres_rclose(&in);
		if (result < 0) {
			return result;
		}
	}
	return 0;
}

/* Build the sealed form into `temp` straight from the source catalog. */
static int seal_copy(kres_container *src, const char *temp)
{
	struct kres_iter it;
	struct kres_info info;
	int result;

	result = kres_build_begin(temp, (uint8_t)src->resource_count);
	if (result < 0) {
		return result;
	}
	result = kres_iter_begin(src, &it);
	if (result < 0) {
		goto fail;
	}
	while (kres_iter_next(&it, &info) != 0) {
		kres_resource in;
		uint32_t at = 0u;

		result = kres_get(src, (const char *)info.name, &in,
				  KRES_OPEN_READ);
		if (result < 0) {
			goto fail;
		}
		result = kres_build_add((const char *)info.name);
		if (result < 0) {
			(void)kres_rclose(&in);
			goto fail;
		}
		while (at < in.used_bytes) {
			uint16_t chunk = (uint16_t)(in.used_bytes - at);

			if (chunk > KRES_RECORD_SIZE) {
				chunk = KRES_RECORD_SIZE;
			}
			result = kres_seek(&in, at);
			if (result < 0) {
				(void)kres_rclose(&in);
				goto fail;
			}
			result = kres_read(&in, kres_shared, chunk);
			if (result != (int)chunk) {
				(void)kres_rclose(&in);
				goto fail;
			}
			result = kres_build_write(kres_shared, chunk);
			if (result < 0) {
				(void)kres_rclose(&in);
				goto fail;
			}
			at = (uint32_t)(at + chunk);
		}
		(void)kres_rclose(&in);
	}
	return kres_build_end();
fail:
	(void)kres_build_abort();
	return result;
}

int kres_compact(const char *path, unsigned int flags)
{
	kres_container src;
	char swap_path[160];
	char recovery[160];
	int result;

	if (kres_io == (const struct kres_io *)0 || path == (const char *)0 ||
	    (flags & ~(KRES_TIGHT | KRES_SEAL)) != 0u) {
		return -KOS_EINVAL;
	}
	result = make_swap_name(swap_path, sizeof(swap_path), path,
				KRES_TEMP_SUFFIX);
	if (result < 0) {
		return result;
	}
	if ((flags & KRES_SEAL) != 0u) {
		result = kres_open(&src, path, KRES_OPEN_READ);
		if (result < 0) {
			return result;
		}
		result = seal_copy(&src, swap_path);
		if (result != 0) {
			(void)kres_close(&src);
			(void)kres_io->unlink(kres_ctx, swap_path);
			return result;
		}
		(void)kres_close(&src);
	} else {
		kres_container dst;

		result = kres_open(&src, path, KRES_OPEN_READ);
		if (result < 0) {
			return result;
		}
		result = kres_create_container(swap_path);
		if (result < 0) {
			(void)kres_close(&src);
			return result;
		}
		result = kres_open(&dst, swap_path, KRES_OPEN_READ |
						    KRES_OPEN_WRITE);
		if (result < 0) {
			(void)kres_close(&src);
			(void)kres_io->unlink(kres_ctx, swap_path);
			return result;
		}
		result = copy_resources(&src, &dst, flags);
		if (result == 0) {
			result = kres_close(&dst);
		} else {
			(void)kres_close(&dst);
		}
		(void)kres_close(&src);
		if (result < 0) {
			(void)kres_io->unlink(kres_ctx, swap_path);
			return result;
		}
	}
	/* The swap: original to the recovery name, the temp into place,
	 * then the recovery file away. */
	result = make_swap_name(recovery, sizeof(recovery), path,
				KRES_RECOVERY_SUFFIX);
	if (result < 0) {
		(void)kres_io->unlink(kres_ctx, swap_path);
		return result;
	}
	result = kres_io->rename(kres_ctx, path, recovery);
	if (result < 0) {
		(void)kres_io->unlink(kres_ctx, swap_path);
		return result;
	}
	result = kres_io->rename(kres_ctx, swap_path, path);
	if (result < 0) {
		/* The recovery copy still holds the original. */
		(void)kres_io->rename(kres_ctx, recovery, path);
		return result;
	}
	return kres_io->unlink(kres_ctx, recovery);
}
