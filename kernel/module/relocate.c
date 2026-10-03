/* ZephyrOS M4/M5 module relocation and container validation.
 *
 * The ZMOD v1 container is validated completely before the loader touches
 * the driver arena: magic/version/type/ABI, reserved fields, all sizes
 * and offsets, granule arithmetic (in 16-bit intermediates that cannot
 * wrap), and every relocation record (type, reserved byte, patch range).
 *
 * M5 adds a bounded read-source container (struct zmod_source): the same
 * shared header/record validation and the same copy/relocate code paths
 * consume either a resident blob or streamed bytes (ROMFS), so there is
 * only one loader.  Every source read is bounded against the container
 * size before it is narrowed.
 *
 * The loader afterwards copies the payload, zeroes BSS, and applies each
 * ZREL_ABS16 record as `word += load_base`; a patch may never touch a
 * byte outside the loaded image and the addition may not overflow the
 * 16-bit address space.  All arithmetic is done in 16-bit intermediates
 * with explicit wrap checks to avoid hosted 32-bit runtime helpers.
 */

#include "zephyros/module.h"
#include "zephyros/driver.h"

static uint16_t zmod_read_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint8_t zmod_magic_ok(const uint8_t *p)
{
	return (uint8_t)(p[0] == ZMOD_MAGIC_0 && p[1] == ZMOD_MAGIC_1 &&
			 p[2] == ZMOD_MAGIC_2 && p[3] == ZMOD_MAGIC_3);
}

/* Shared header validation: everything a container must satisfy before the
 * arena may be touched.  Only scalar fields are copied out; the caller's
 * header bytes (memory blob or stack staging copy) stay caller-owned. */
static int zmod_validate_header(const struct zmod_header *h, uint16_t size,
				struct zmod_container *out)
{
	uint16_t payload_end;
	uint16_t reloc_end;
	uint16_t total;
	uint16_t need;

	if (!zmod_magic_ok(h->magic)) {
		return ZMOD_EINVAL;
	}
	if (h->format_version != ZMOD_FORMAT_VERSION) {
		return ZMOD_ENOTSUP;
	}
	if (h->module_type != ZMOD_TYPE_DRIVER) {
		return ZMOD_ENOTSUP;
	}
	if (h->driver_abi != ZDRIVER_ABI_VERSION) {
		return ZMOD_ENOTSUP;
	}
	if (h->flags != ZMOD_FLAG_NONE || h->reserved != 0) {
		return ZMOD_EINVAL;
	}
	if (h->image_size == 0) {
		return ZMOD_EFORMAT;
	}
	if (h->total_size != size) {
		return ZMOD_EFORMAT;
	}

	/* Payload must fit and end exactly at the declared container end. */
	if (h->payload_offset < ZMOD_HEADER_SIZE) {
		return ZMOD_EFORMAT;
	}
	payload_end = (uint16_t)(h->payload_offset + h->image_size);
	if (payload_end < h->payload_offset) {
		return ZMOD_EFORMAT; /* 16-bit wrap */
	}
	if (payload_end != h->total_size) {
		return ZMOD_EFORMAT;
	}

	/* Relocation table must fit before the payload without overlap. */
	if (h->reloc_offset < ZMOD_HEADER_SIZE) {
		return ZMOD_EFORMAT;
	}
	if (h->reloc_count > (uint16_t)((uint16_t)(0xFFFFu - h->reloc_offset) >> 2)) {
		return ZMOD_EFORMAT;
	}
	reloc_end = (uint16_t)(h->reloc_offset + (uint16_t)(h->reloc_count << 2));
	if (reloc_end > h->payload_offset) {
		return ZMOD_EFORMAT; /* overlap or wrapped table */
	}

	/* Entry points are image-relative and must be inside the image. */
	if (h->init_offset >= h->image_size) {
		return ZMOD_EFORMAT;
	}
	if (h->fini_offset != ZMOD_NO_ENTRY && h->fini_offset >= h->image_size) {
		return ZMOD_EFORMAT;
	}

	/* Granule arithmetic in wider-than-16-bit-safe steps. */
	if (h->image_size > (uint16_t)(ZMOD_MAX_TOTAL_SIZE - h->bss_size)) {
		return ZMOD_EFORMAT; /* image + bss would wrap 16 bits */
	}
	total = (uint16_t)(h->image_size + h->bss_size);
	need = (uint16_t)((total >> 9) + ((total & (ZMOD_GRANULE_SIZE - 1u)) != 0));
	if (need == 0 || need != h->required_granules) {
		return ZMOD_EFORMAT;
	}
	if (need > ZMOD_MAX_GRANULES) {
		return ZMOD_ENOSPC;
	}

	out->total_size = h->total_size;
	out->image_size = h->image_size;
	out->bss_size = h->bss_size;
	out->required_granules = h->required_granules;
	out->init_offset = h->init_offset;
	out->fini_offset = h->fini_offset;
	out->reloc_offset = h->reloc_offset;
	out->reloc_count = h->reloc_count;
	out->payload_offset = h->payload_offset;
	return ZMOD_OK;
}

/* Shared relocation-record validation. */
static int zmod_check_reloc(uint8_t type, uint8_t reserved,
			    uint16_t patch_offset, uint16_t image_size)
{
	if (type != ZREL_ABS16) {
		return ZMOD_ENOTSUP;
	}
	if (reserved != 0) {
		return ZMOD_EINVAL;
	}
	if (patch_offset >= image_size ||
	    (uint16_t)(image_size - patch_offset) < 2u) {
		return ZMOD_EFORMAT;
	}
	return ZMOD_OK;
}

/* Bounded read through the container's byte source; every offset/length is
 * checked against the validated container size before narrowing. */
static int zmod_source_read_checked(const struct zmod_source *s,
				    uint16_t offset, uint8_t *dst,
				    uint16_t length)
{
	if ((uint32_t)offset + length > s->size) {
		return ZMOD_EFORMAT;
	}
	if (length == 0) {
		return ZMOD_OK;
	}
	if (s->read(s->ctx, offset, dst, length) < 0) {
		return ZMOD_EFORMAT;
	}
	return ZMOD_OK;
}

#define ZMOD_STREAM_CHUNK 128u

static int zmod_container_read(const struct zmod_container *c,
			       uint16_t offset, uint8_t *dst, uint16_t length)
{
	uint16_t i;

	if ((uint32_t)offset + length > c->total_size) {
		return ZMOD_EFORMAT;
	}
	if (c->blob != (const uint8_t *)0) {
		for (i = 0; i < length; ++i) {
			dst[i] = c->blob[offset + i];
		}
		return ZMOD_OK;
	}
	if (c->source == (const struct zmod_source *)0) {
		return ZMOD_EFORMAT;
	}
	return zmod_source_read_checked(c->source, offset, dst, length);
}

int zmod_validate_container(const uint8_t *blob, uint16_t size,
			    struct zmod_container *out)
{
	const struct zmod_header *h;
	int result;
	uint16_t i;

	if (blob == (const uint8_t *)0 || out == (struct zmod_container *)0) {
		return ZMOD_EINVAL;
	}
	if (size < ZMOD_HEADER_SIZE) {
		return ZMOD_EFORMAT;
	}

	h = (const struct zmod_header *)blob;
	result = zmod_validate_header(h, size, out);
	if (result != ZMOD_OK) {
		return result;
	}
	out->blob = blob;
	out->source = (const struct zmod_source *)0;

	/* Every relocation record must be well-formed and patch in range. */
	for (i = 0; i < h->reloc_count; ++i) {
		const uint8_t *rec = blob + h->reloc_offset +
				     (uint16_t)(i << 2);

		result = zmod_check_reloc(rec[0], rec[1],
					  zmod_read_u16(rec + 2),
					  h->image_size);
		if (result != ZMOD_OK) {
			return result;
		}
	}
	return ZMOD_OK;
}

int zmod_validate_source(const struct zmod_source *source,
			 struct zmod_container *out)
{
	uint8_t header[ZMOD_HEADER_SIZE];
	uint8_t rec[ZMOD_RELOC_SIZE];
	const struct zmod_header *h =
		(const struct zmod_header *)header;
	uint16_t reloc_offset;
	uint16_t reloc_count;
	uint16_t image_size;
	int result;
	uint16_t i;

	if (source == (const struct zmod_source *)0 ||
	    source->read == (zmod_source_read_fn)0 ||
	    out == (struct zmod_container *)0) {
		return ZMOD_EINVAL;
	}
	if (source->size < ZMOD_HEADER_SIZE) {
		return ZMOD_EFORMAT;
	}
	result = zmod_source_read_checked(source, 0, header, ZMOD_HEADER_SIZE);
	if (result != ZMOD_OK) {
		return ZMOD_EFORMAT;
	}
	result = zmod_validate_header(h, source->size, out);
	if (result != ZMOD_OK) {
		return result;
	}
	out->blob = (const uint8_t *)0;
	out->source = source;

	reloc_offset = h->reloc_offset;
	reloc_count = h->reloc_count;
	image_size = h->image_size;
	for (i = 0; i < reloc_count; ++i) {
		uint16_t at = (uint16_t)(reloc_offset +
					 (uint16_t)(i << 2));

		result = zmod_source_read_checked(source, at, rec,
						  ZMOD_RELOC_SIZE);
		if (result != ZMOD_OK) {
			return ZMOD_EFORMAT;
		}
		result = zmod_check_reloc(rec[0], rec[1],
					  zmod_read_u16(rec + 2), image_size);
		if (result != ZMOD_OK) {
			return result;
		}
	}
	return ZMOD_OK;
}

int zmod_copy_image(uint8_t *load_base, const struct zmod_container *c)
{
	uint16_t done = 0;

	if (load_base == (uint8_t *)0 || c == (const struct zmod_container *)0) {
		return ZMOD_EINVAL;
	}
	while (done < c->image_size) {
		uint16_t chunk = (uint16_t)(c->image_size - done);
		int result;

		if (chunk > ZMOD_STREAM_CHUNK) {
			chunk = ZMOD_STREAM_CHUNK;
		}
		result = zmod_container_read(
			c, (uint16_t)(c->payload_offset + done),
			load_base + done, chunk);
		if (result != ZMOD_OK) {
			return result;
		}
		done = (uint16_t)(done + chunk);
	}
	return ZMOD_OK;
}

void zmod_zero_bss(uint8_t *load_base, const struct zmod_container *c)
{
	uint16_t i;

	for (i = 0; i < c->bss_size; ++i) {
		load_base[c->image_size + i] = 0;
	}
}

int zmod_apply_relocations(uint8_t *load_base, const struct zmod_container *c)
{
	uint16_t base = (uint16_t)load_base;
	uint16_t i;

	if (load_base == (uint8_t *)0 || c == (const struct zmod_container *)0) {
		return ZMOD_EINVAL;
	}
	for (i = 0; i < c->reloc_count; ++i) {
		uint8_t rec[ZMOD_RELOC_SIZE];
		uint16_t offset;
		uint16_t value;
		int result = zmod_container_read(
			c, (uint16_t)(c->reloc_offset + (uint16_t)(i << 2)),
			rec, ZMOD_RELOC_SIZE);

		if (result != ZMOD_OK) {
			return ZMOD_EFORMAT; /* re-validated defensively */
		}
		result = zmod_check_reloc(rec[0], rec[1],
					  zmod_read_u16(rec + 2),
					  c->image_size);
		if (result != ZMOD_OK) {
			return result;
		}
		offset = zmod_read_u16(rec + 2);
		value = zmod_read_u16(load_base + offset);
		if (value > (uint16_t)(0xFFFFu - base)) {
			return ZMOD_EFORMAT; /* relocation overflow */
		}
		value = (uint16_t)(value + base);
		load_base[offset] = (uint8_t)value;
		load_base[offset + 1] = (uint8_t)(value >> 8);
	}
	return ZMOD_OK;
}

void zmod_zero_allocation(uint8_t *load_base, uint16_t bytes)
{
	uint16_t i;

	for (i = 0; i < bytes; ++i) {
		load_base[i] = 0;
	}
}
