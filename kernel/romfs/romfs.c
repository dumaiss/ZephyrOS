/* ZephyrOS M5 ROMFS kernel reader.
 *
 * Allocation-free reader over the platform recovery ROM pages.  The full
 * header and directory are validated by `romfs_init()` before the
 * filesystem is published; a malformed image is rejected as a whole and
 * no read can then be driven by unvalidated metadata.
 *
 * Reads are staged in bounded chunks through the common ROM-access
 * primitive (`kernel/romfs/rom_access.asm`), which enters the mode-00
 * ROM window for one bounded copy and restores the exact caller state, so
 * reads may cross 48-KiB window boundaries without any bank-7 code or
 * stack ever seeing ROM.
 *
 * All offset arithmetic is checked in 32-bit intermediates before it is
 * narrowed; 32-bit division is avoided (window index by repeated
 * subtraction) so the freestanding build needs no runtime library.
 */

#include "zephyros/romfs.h"
#include "zephyros/module.h"

extern uint8_t rom_access_staging[];

/* Published filesystem state (bank-7 runtime data). */
static uint8_t romfs_ready;
static uint16_t romfs_file_count;
static uint32_t romfs_image_size;

static uint16_t romfs_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t romfs_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Split a 32-bit logical offset into (recovery page, window offset)
 * without a 32-bit division.  The capacity is at most capacity/window
 * iterations; anything outside the configured pages fails. */
static int romfs_window(uint32_t logical, uint8_t *page, uint16_t *offset)
{
	uint32_t window = 0;

	while (logical >= ZOS_ROMFS_WINDOW_SIZE) {
		logical -= ZOS_ROMFS_WINDOW_SIZE;
		++window;
	}
	if (window >= ZOS_ROMFS_PAGE_COUNT) {
		return -ZOS_EIO;
	}
	*page = (uint8_t)(ZOS_ROMFS_FIRST_PAGE + window);
	*offset = (uint16_t)logical;
	return 0;
}

/* Copy `length` (1..65535) staged bytes from a logical stream offset.
 * Splits at window boundaries and at the common staging size so the
 * ROM-access primitive is never asked for an out-of-window request. */
static int romfs_copy_logical(uint32_t logical, uint8_t *dst, uint16_t length)
{
	while (length > 0) {
		struct rom_access_req req;
		uint16_t chunk = length;
		uint32_t logical_end;
		uint8_t status;
		int result;

		/* Every stream read stays inside the physical capacity; the
		 * header/table/file validation above enforces the tighter
		 * per-structure bounds. */
		logical_end = logical + chunk;
		if (logical_end < logical || logical_end > ZOS_ROMFS_CAPACITY) {
			return -ZOS_EIO;
		}
		result = romfs_window(logical, &req.page, &req.offset);
		if (result != 0) {
			return result;
		}
		if (chunk > (uint16_t)(ZOS_ROMFS_WINDOW_SIZE - req.offset)) {
			chunk = (uint16_t)(ZOS_ROMFS_WINDOW_SIZE - req.offset);
		}
		if (chunk > ROMFS_STAGING_SIZE) {
			chunk = ROMFS_STAGING_SIZE;
		}
		req.length = chunk;
		status = rom_access_copy(&req);
		if (status != 0) {
			return -(int)status;
		}
		{
			uint16_t i;

			for (i = 0; i < chunk; ++i) {
				dst[i] = rom_access_staging[i];
			}
		}
		dst += chunk;
		logical += chunk;
		length -= chunk;
	}
	return 0;
}

/* Stored names are NUL-terminated and zero-padded inside 24 bytes. */
static uint8_t romfs_name_valid(const uint8_t *name)
{
	uint8_t i;

	for (i = 0; i < ROMFS_NAME_SIZE; ++i) {
		uint8_t c = name[i];

		if (c == 0) {
			break;
		}
		if (c < 0x20 || c > 0x7E) {
			return 0;
		}
	}
	if (i == 0 || i >= ROMFS_NAME_SIZE) {
		return 0; /* empty or unterminated */
	}
	for (; i < ROMFS_NAME_SIZE; ++i) {
		if (name[i] != 0) {
			return 0; /* padding must be zero */
		}
	}
	return 1;
}

static uint8_t romfs_name_match(const uint8_t *stored, const char *wanted)
{
	uint8_t i;

	for (i = 0; i < ROMFS_NAME_SIZE; ++i) {
		uint8_t c = stored[i];

		if (c == 0) {
			return (uint8_t)(wanted[i] == 0);
		}
		if (wanted[i] == 0 || (uint8_t)wanted[i] != c) {
			return 0;
		}
	}
	return 0; /* unterminated stored name (init rejects it) */
}

/* Validate one directory entry staged in rom_access_staging. */
static int romfs_check_entry(const uint8_t *entry, uint32_t payload_start)
{
	uint32_t offset;
	uint32_t size;

	if (!romfs_name_valid(entry)) {
		return -ZOS_EIO;
	}
	if (romfs_le16(entry + 32) != ROMFS_FLAG_ZMOD &&
	    romfs_le16(entry + 32) != ROMFS_FLAG_ZEX &&
	    romfs_le16(entry + 32) != ROMFS_FLAG_DATA) {
		return -ZOS_EIO;
	}
	if (romfs_le16(entry + 34) != 0) {
		return -ZOS_EIO;
	}
	offset = romfs_le32(entry + 24);
	size = romfs_le32(entry + 28);
	if (offset < payload_start) {
		return -ZOS_EIO; /* table/payload overlap */
	}
	if (offset > romfs_image_size) {
		return -ZOS_EIO;
	}
	if (size > (romfs_image_size - offset)) {
		return -ZOS_EIO; /* leaves the image or wraps 32 bits */
	}
	return 0;
}

int romfs_init(void)
{
	uint8_t header[ROMFS_HEADER_SIZE];
	uint32_t directory_size;
	uint32_t expected_directory;
	uint32_t payload_start;
	uint16_t count;
	uint16_t i;
	int result;

	romfs_ready = 0;
	romfs_file_count = 0;
	romfs_image_size = 0;

	result = romfs_copy_logical(0, header, ROMFS_HEADER_SIZE);
	if (result != 0) {
		return result; /* raw -ZOS_EBUSY/-ZOS_EINVAL/-ZOS_EIO */
	}
	if (header[0] != ROMFS_MAGIC_0 || header[1] != ROMFS_MAGIC_1 ||
	    header[2] != ROMFS_MAGIC_2 || header[3] != ROMFS_MAGIC_3) {
		return -ZOS_EIO;
	}
	if (header[4] != ROMFS_VERSION) {
		return -ZOS_EIO;
	}
	if (header[5] != ROMFS_ENTRY_SIZE) {
		return -ZOS_EIO;
	}
	if (romfs_le16(header + 10) != 0) {
		return -ZOS_EIO; /* reserved field */
	}
	count = romfs_le16(header + 6);
	directory_size = romfs_le16(header + 8);
	romfs_image_size = romfs_le32(header + 12);

	/* count * 36 computed with shifts (no 32-bit multiply runtime). */
	expected_directory = ((uint32_t)count << 5) + ((uint32_t)count << 2);
	if (directory_size != expected_directory) {
		return -ZOS_EIO;
	}
	if (romfs_image_size < ROMFS_HEADER_SIZE ||
	    romfs_image_size > ZOS_ROMFS_CAPACITY) {
		return -ZOS_ENOSPC;
	}
	payload_start = ROMFS_HEADER_SIZE + directory_size;
	if (payload_start > romfs_image_size) {
		return -ZOS_EIO;
	}

	/* Validate the entire directory before publishing the filesystem:
	 * every entry must be well-formed and inside the declared payload
	 * area.  A corrupt image is never partially accepted. */
	for (i = 0; i < count; ++i) {
		uint32_t at = ROMFS_HEADER_SIZE +
			      ((uint32_t)i << 5) + ((uint32_t)i << 2);

		if (at > romfs_image_size ||
		    (uint32_t)ROMFS_ENTRY_SIZE > (romfs_image_size - at)) {
			return -ZOS_EIO;
		}
		result = romfs_copy_logical(at, rom_access_staging,
					    ROMFS_ENTRY_SIZE);
		if (result != 0) {
			return result;
		}
		result = romfs_check_entry(rom_access_staging, payload_start);
		if (result != 0) {
			return result;
		}
	}

	romfs_file_count = count;
	romfs_ready = 1;
	return 0;
}

int romfs_lookup(const char *name, romfs_file_t *out)
{
	uint16_t i;

	if (romfs_ready == 0 || name == (const char *)0 ||
	    out == (romfs_file_t *)0) {
		return -ZOS_EINVAL;
	}
	for (i = 0; i < romfs_file_count; ++i) {
		uint32_t at = ROMFS_HEADER_SIZE +
			      ((uint32_t)i << 5) + ((uint32_t)i << 2);
		int result = romfs_copy_logical(at, rom_access_staging,
						ROMFS_ENTRY_SIZE);

		if (result != 0) {
			return result;
		}
		if (romfs_name_match(rom_access_staging, name)) {
			out->offset = romfs_le32(rom_access_staging + 24);
			out->size = romfs_le32(rom_access_staging + 28);
			out->flags = romfs_le16(rom_access_staging + 32);
			return 0;
		}
	}
	return -ZOS_ENOENT;
}

int romfs_read(const romfs_file_t *file, uint32_t offset, void *dst,
	       uint16_t length)
{
	int result;

	if (romfs_ready == 0 || file == (const romfs_file_t *)0) {
		return -ZOS_EINVAL;
	}
	if (dst == (void *)0 && length != 0) {
		return -ZOS_EINVAL;
	}
	if (offset > file->size) {
		return -ZOS_EINVAL;
	}
	if ((uint32_t)length > (file->size - offset)) {
		return -ZOS_EINVAL; /* bounded: never read past EOF */
	}
	if (length == 0) {
		return 0; /* clean EOF read */
	}
	result = romfs_copy_logical(file->offset + offset, (uint8_t *)dst,
				    length);
	if (result != 0) {
		return result;
	}
	return (int)length;
}

/* --- module-loader source adapter ------------------------------------- */

static int romfs_source_read(const void *ctx, uint16_t offset, uint8_t *dst,
			     uint16_t length)
{
	const romfs_file_t *file = (const romfs_file_t *)ctx;

	return romfs_read(file, (uint32_t)offset, dst, length);
}

int romfs_load_zmod(const char *name, const char *romfs_name)
{
	static romfs_file_t file;
	struct zmod_source source;
	int result;

	if (romfs_ready == 0) {
		return -ZOS_EINVAL;
	}
	if (name == (const char *)0 || romfs_name == (const char *)0) {
		return -ZOS_EINVAL;
	}
	result = romfs_lookup(romfs_name, &file);
	if (result != 0) {
		return result;
	}
	if (file.flags != ROMFS_FLAG_ZMOD) {
		return -ZOS_EINVAL;
	}
	if (file.size < ZMOD_HEADER_SIZE || file.size > 0xFFFFu) {
		return ZMOD_EFORMAT;
	}
	source.read = romfs_source_read;
	source.ctx = &file;
	source.size = (uint16_t)file.size;
	return module_load_source(name, &source);
}
