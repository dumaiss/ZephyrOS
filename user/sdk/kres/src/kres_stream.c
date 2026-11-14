/* KRES byte/record reads and resource cursors. Kept separate from container
 * opening/catalog lookup so a kos_reader callback makes only this streaming
 * path resident. Public API and on-media bytes are unchanged. */
#include "kres_internal.h"

int kres_rclose(kres_resource *r)
{
	if (r == (kres_resource *)0 || r->open == 0u) {
		return -KOS_EINVAL;
	}
	r->open = 0u;
	return 0;
}

int kres_seek(kres_resource *r, uint32_t offset)
{
	if (r == (kres_resource *)0 || r->open == 0u) {
		return -KOS_EINVAL;
	}
	r->cursor = offset;
	return 0;
}

int kres_read(kres_resource *r, void *dst, uint16_t len)
{
	uint32_t at;
	uint16_t got = 0u;

	if (r == (kres_resource *)0 || r->open == 0u || dst == (void *)0 ||
	    len == 0u) {
		return -KOS_EINVAL;
	}
	if (r->cursor >= r->used_bytes) {
		return 0; /* end of file */
	}
	if ((uint32_t)len > r->used_bytes - r->cursor) {
		len = (uint16_t)(r->used_bytes - r->cursor);
	}
	at = r->start_record * KRES_RECORD_SIZE + r->cursor;
	while (len != 0u) {
		uint16_t chunk = (uint16_t)(KRES_RECORD_SIZE -
			 (at & (uint32_t)(KRES_RECORD_SIZE - 1u)));
		int result;

		if (chunk > len) {
			chunk = len;
		}
		result = kres_io->seek(r->container->io_ctx, at);
		if (result < 0) {
			return result;
		}
		result = kres_io->read(r->container->io_ctx, kres_shared,
				       chunk);
		if (result < 0) {
			return result;
		}
		if (result == 0) {
			break;
		}
		memcpy(dst, kres_shared, (uint16_t)result);
		dst = (uint8_t *)dst + result;
		got = (uint16_t)(got + (uint16_t)result);
		len = (uint16_t)(len - (uint16_t)result);
		at = (uint32_t)(at + (uint16_t)result);
		r->cursor = (uint32_t)(r->cursor + (uint16_t)result);
		if (result != (int)chunk) {
			break; /* a short read ends the walk */
		}
	}
	return got;
}

int kres_read_records(kres_resource *r, uint16_t first_record,
		      uint16_t record_count, void *dst)
{
	uint16_t count = 0u;
	int total = 0;

	if (r == (kres_resource *)0 || r->open == 0u || dst == (void *)0) {
		return -KOS_EINVAL;
	}
	while (count < record_count &&
	       (uint32_t)first_record + count < r->allocated_records) {
		uint32_t record = (uint32_t)first_record + count;
		uint32_t at = (uint32_t)r->start_record + record;
		uint32_t record_begin = record * KRES_RECORD_SIZE;
		uint16_t want = KRES_RECORD_SIZE;
		int result;

		if (record_begin >= r->used_bytes) {
			break;
		}
		if (record_begin + KRES_RECORD_SIZE > r->used_bytes) {
			want = (uint16_t)(r->used_bytes - record_begin);
		}
		result = kres_io->seek(r->container->io_ctx,
				       at * KRES_RECORD_SIZE);
		if (result < 0) {
			return result;
		}
		result = kres_io->read(r->container->io_ctx, kres_shared,
				       want);
		if (result < 0) {
			return result;
		}
		memcpy(dst, kres_shared, (uint16_t)result);
		dst = (uint8_t *)dst + result;
		total += result;
		++count;
		if (result != (int)want) {
			break; /* a short read ends the walk */
		}
	}
	return total;
}

