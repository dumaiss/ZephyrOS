/* ZephyrOS ROMFS kernel reader.
 *
 * Allocation-free reader over the platform recovery ROM pages.  The full
 * header and directory are validated by `romfs_init()` before the
 * filesystem is published; a malformed image is rejected as a whole and
 * no read can then be driven by unvalidated metadata.
 *
 * M8 adds the narrow module-facing resource imports
 * (`romfs_resource_size` / `romfs_resource_read`): name-resolved,
 * DATA-flag-only, 16-bit-bounded access that never exposes
 * `romfs_file_t` or the mutable filesystem state.  These are the only
 * filesystem-like callbacks in the driver ABI.
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

#include "kernel/lib/name.h"

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

int romfs_lookup_ci(const char *name, romfs_file_t *out)
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
		/* The one and only ROMFS name comparison: ASCII case-folded
		 * and bounded to the 24-byte entry name field (architecture
		 * sections 32.8 and 32.13).  VFS, the provider and the module
		 * resource imports all arrive here. */
		if (zos_ascii_equal((const char *)rom_access_staging, name,
				   ROMFS_NAME_SIZE, 1u)) {
			out->offset = romfs_le32(rom_access_staging + 24);
			out->size = romfs_le32(rom_access_staging + 28);
			out->flags = romfs_le16(rom_access_staging + 32);
			return 0;
		}
	}
	return -ZOS_ENOENT;
}

/* Directory enumeration, needed only by the VFS ROMFS provider.  Copy entry
 * `index` metadata and its NUL-terminated name (ASCII, at most
 * ROMFS_NAME_SIZE bytes).  Returns 0 or -ZOS_ENOENT past the end of the
 * validated directory; the provider turns that into end-of-stream. */
int romfs_entry_at(uint16_t index, romfs_file_t *out, char *name,
		   uint16_t name_size)
{
	uint32_t at;
	int result;

	if (romfs_ready == 0 || out == (romfs_file_t *)0 ||
	    index >= romfs_file_count) {
		return -ZOS_ENOENT;
	}
	at = ROMFS_HEADER_SIZE + ((uint32_t)index << 5) +
	     ((uint32_t)index << 2);
	result = romfs_copy_logical(at, rom_access_staging, ROMFS_ENTRY_SIZE);
	if (result != 0) {
		return result;
	}
	out->offset = romfs_le32(rom_access_staging + 24);
	out->size = romfs_le32(rom_access_staging + 28);
	out->flags = romfs_le16(rom_access_staging + 32);
	if (name != (char *)0 && name_size > 0) {
		uint16_t i = 0;

		while (i + 1 < name_size &&
		       rom_access_staging[i] != 0 &&
		       i < ROMFS_NAME_SIZE) {
			name[i] = (char)rom_access_staging[i];
			++i;
		}
		name[i] = '\0';
	}
	return 0;
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

/* --- bootstrap resource reads -------------------------------------------
 * Narrow, kernel-owned wrappers: resolve by name, require exactly
 * ROMFS_FLAG_DATA, bound every access, and never expose romfs_file_t or
 * the mutable filesystem state. */

static int romfs_resource_lookup(const char *name, romfs_file_t *file)
{
	int result;

	if (name == (const char *)0) {
		return -ZOS_EINVAL;
	}
	result = romfs_lookup_ci(name, file);
	if (result != 0) {
		return result;
	}
	if (file->flags != ROMFS_FLAG_DATA) {
		return -ZOS_EINVAL; /* only data resources are importable */
	}
	return 0;
}

int romfs_resource_size(const char *name, uint16_t *out_size)
{
	romfs_file_t file;
	int result;

	if (out_size == (uint16_t *)0) {
		return -ZOS_EINVAL;
	}
	result = romfs_resource_lookup(name, &file);
	if (result != 0) {
		return result;
	}
	if (file.size > 0xFFFFu) {
		return -ZOS_ENOSPC; /* import sizes are 16-bit bounded */
	}
	*out_size = (uint16_t)file.size;
	return 0;
}

int romfs_resource_read(const char *name, uint16_t offset, void *dst,
			uint16_t length)
{
	romfs_file_t file;
	int result;

	if (dst == (void *)0 && length != 0) {
		return -ZOS_EINVAL;
	}
	result = romfs_resource_lookup(name, &file);
	if (result != 0) {
		return result;
	}
	if ((uint32_t)offset > file.size) {
		return -ZOS_EINVAL;
	}
	if ((uint32_t)length > (file.size - (uint32_t)offset)) {
		return -ZOS_EINVAL; /* bounded: never read past the resource */
	}
	if (length == 0) {
		return 0;
	}
	return romfs_read(&file, (uint32_t)offset, dst, length);
}
