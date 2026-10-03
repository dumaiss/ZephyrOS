/* ZephyrOS M4 module relocation and container validation.
 *
 * The ZMOD v1 container is validated completely before the loader touches
 * the driver arena: magic/version/type/ABI, reserved fields, all sizes
 * and offsets, granule arithmetic (in 16-bit intermediates that cannot
 * wrap), and every relocation record (type, reserved byte, patch range).
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

int zmod_validate_container(const uint8_t *blob, uint16_t size,
			    struct zmod_container *out)
{
	const struct zmod_header *h;
	uint16_t payload_end;
	uint16_t reloc_end;
	uint16_t total;
	uint16_t need;
	uint16_t i;

	if (blob == (const uint8_t *)0 || out == (struct zmod_container *)0) {
		return ZMOD_EINVAL;
	}
	if (size < ZMOD_HEADER_SIZE) {
		return ZMOD_EFORMAT;
	}

	h = (const struct zmod_header *)blob;
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
	if (need > ZMOD_GRANULE_COUNT) {
		return ZMOD_ENOSPC;
	}

	/* Every relocation record must be well-formed and patch in range. */
	for (i = 0; i < h->reloc_count; ++i) {
		const uint8_t *rec = blob + h->reloc_offset +
				     (uint16_t)(i << 2);
		uint16_t offset = zmod_read_u16(rec + 2);

		if (rec[0] != ZREL_ABS16) {
			return ZMOD_ENOTSUP;
		}
		if (rec[1] != 0) {
			return ZMOD_EINVAL;
		}
		if (offset >= h->image_size ||
		    (uint16_t)(h->image_size - offset) < 2u) {
			return ZMOD_EFORMAT;
		}
	}

	out->blob = blob;
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

void zmod_copy_image(uint8_t *load_base, const struct zmod_container *c)
{
	uint16_t i;

	for (i = 0; i < c->image_size; ++i) {
		load_base[i] = c->blob[c->payload_offset + i];
	}
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

	for (i = 0; i < c->reloc_count; ++i) {
		const uint8_t *rec = c->blob + c->reloc_offset +
				     (uint16_t)(i << 2);
		uint16_t offset = zmod_read_u16(rec + 2);
		uint16_t value;

		if (rec[0] != ZREL_ABS16 || rec[1] != 0) {
			return ZMOD_EFORMAT; /* validate again, defensively */
		}
		if (offset >= c->image_size ||
		    (uint16_t)(c->image_size - offset) < 2u) {
			return ZMOD_EFORMAT;
		}
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
