/* ZephyrOS M5 ROMFS read-only recovery filesystem.
 *
 * ROMFS v1 is the tiny, allocation-free boot/recovery store placed in the
 * platform's recovery ROM pages (platform/zephyr80/private/memory.h).  The image
 * is built by tools/romfspack.py from recovery/manifest.json; the kernel
 * reader here only looks up and reads complete, validated files.
 *
 * On-disk format (little-endian, see docs/romfs-format.md):
 *   16-byte header   magic[4]="ZRFS", version:u8=1, entry_size:u8=36,
 *                    file_count:u16, directory_size:u16, reserved:u16=0,
 *                    image_size:u32
 *   36-byte entries  name[24] (NUL-terminated, zero-padded),
 *                    logical_offset:u32, size:u32, flags:u16, reserved:u16=0
 *   payloads         packed in manifest order without alignment gaps
 *
 * `logical_offset` is measured across the concatenated 0000h-BFFFh windows
 * of the configured recovery pages.  Reads stage bounded chunks through
 * the common ROM-access primitive (rom_access_copy); no ROM byte is ever
 * mapped into the bank-7 address space.
 *
 * The API is deliberately small and is not the future native filesystem
 * ABI (M9 defines that separately).  M9 keeps exactly one lookup
 * (`romfs_lookup_ci`, ASCII case-folded) and one bounded read; the VFS
 * ROMFS provider in fs/romfs/ is the only caller that enumerates.
 */

#ifndef ZEPHYROS_ROMFS_H
#define ZEPHYROS_ROMFS_H

#include "platform.h"
#include "zos/errno.h"

/* ROMFS v1 format constants. */
#define ROMFS_MAGIC_0 'Z'
#define ROMFS_MAGIC_1 'R'
#define ROMFS_MAGIC_2 'F'
#define ROMFS_MAGIC_3 'S'
#define ROMFS_VERSION    1u
#define ROMFS_HEADER_SIZE 16u
#define ROMFS_ENTRY_SIZE  36u
#define ROMFS_NAME_SIZE   24u

/* Entry flags / media types. */
#define ROMFS_FLAG_ZMOD 1u
#define ROMFS_FLAG_ZEX  2u
#define ROMFS_FLAG_DATA 3u

/* Bounded common staging buffer used by the ROM-access primitive.  The
 * size is a platform memory reservation (memory.h/.inc) shared with
 * kernel/romfs/rom_access.asm; the host tests and mapcheck enforce the
 * pairing. */
#define ROMFS_STAGING_SIZE ZOS_ROMFS_STAGING_SIZE

/* Immutable metadata identifying one ROMFS entry. */
typedef struct romfs_file {
	uint32_t offset; /* logical offset of the payload        */
	uint32_t size;   /* payload bytes (32-bit bounded)       */
	uint16_t flags;  /* ROMFS_FLAG_*                         */
} romfs_file_t;

/* Validate the complete ROMFS header and directory, then publish the
 * filesystem.  Returns 0 or a negative error (the image is rejected as a
 * whole; a partially valid image is never published). */
int romfs_init(void);

/* The one ROMFS lookup: resolve a native name with ASCII case-folded
 * comparison (architecture sections 32.8 and 32.13).  It serves the VFS
 * ROMFS provider, the ZEX launcher and the module resource imports; there
 * is no case-sensitive variant beside it.  Returns 0 on success, or
 * -ZOS_ENOENT / a negative error. */
int romfs_lookup_ci(const char *name, romfs_file_t *out);

/* Bounded read from `offset` (file-relative) into `dst`.  Returns the
 * number of bytes copied (0 at EOF when offset == size and length == 0),
 * or a negative error.  Requests that leave the file are rejected; reads
 * across the 48-KiB recovery-page windows are staged internally. */
int romfs_read(const romfs_file_t *file, uint32_t offset, void *dst,
	       uint16_t length);

/* --- provider support ---------------------------------------------------
 * Indexed access to the validated directory.  It exists for the VFS ROMFS
 * provider's `readdir`, which is the only consumer that needs to enumerate;
 * ROMFS-boot builds only.  Returns 0, or -ZOS_ENOENT past the end of the
 * directory (which the provider reports as end-of-stream). */
int romfs_entry_at(uint16_t index, romfs_file_t *out, char *name,
		   uint16_t name_size);

/* --- module-facing resource imports (driver ABI v3) --------------------
 * The only filesystem-like drivers a loaded module receives.  Both
 * accept a name and operate on entries carrying exactly ROMFS_FLAG_DATA;
 * ZMOD/ZEX entries are rejected with -ZOS_EINVAL.  `romfs_file_t` and the
 * mutable filesystem state stay kernel-private; a resource larger than
 * 16 bits is rejected with -ZOS_ENOSPC.  The functions validate the
 * complete ROMFS image at init, so a lookup can never be driven by
 * unvalidated metadata. */

/* Size lookup by name.  Returns 0 and stores the 16-bit size, or a
 * negative error (-ZOS_EINVAL invalid/not DATA, -ZOS_ENOENT missing,
 * -ZOS_ENOSPC size > 0xFFFF, -ZOS_EINVAL before romfs_init()). */
int romfs_resource_size(const char *name, uint16_t *out_size);

/* Bounded read by name: `offset + length` may not leave the resource.
 * Returns the number of bytes copied, or a negative error (-ZOS_EINVAL
 * invalid/not DATA/out of bounds, -ZOS_ENOENT missing). */
int romfs_resource_read(const char *name, uint16_t offset, void *dst,
			uint16_t length);

/* --- internal ROM-access ABI -------------------------------------------
 * kernel/romfs/rom_access.asm owns the mapping transition: it validates
 * the request, locks interrupts, saves the exact latch/IFF state, enters
 * the mode-00 ROM window through the fixed mirrored stub, copies through
 * the common staging buffer (SRAM bank 0), and restores everything
 * exactly.  It is common-resident, stackless inside the ROM window, and
 * non-reentrant.
 */

struct rom_access_req {
	uint8_t page;    /* recovery ROM page (ZOS_ROMFS_FIRST_PAGE..) */
	uint16_t offset; /* byte offset within the 0000h-BFFFh window  */
	uint16_t length; /* bytes to stage, 1..ROMFS_STAGING_SIZE      */
};

#if defined(__SDCC)
/* The assembly primitive consumes this struct field by field; pin the
 * packed layout (z80 SDCC does not insert padding for these fields). */
_Static_assert(sizeof(struct rom_access_req) == 5,
	       "rom_access_req must be 5 packed bytes");
_Static_assert(__builtin_offsetof(struct rom_access_req, offset) == 1,
	       "rom_access_req offset field");
_Static_assert(__builtin_offsetof(struct rom_access_req, length) == 3,
	       "rom_access_req length field");
#endif

/* Returns 0 or a positive ZOS_E* error code (0 = staged OK). */
uint8_t rom_access_copy(const struct rom_access_req *req);

/* Bounded common staging buffer filled by rom_access_copy(). */
extern uint8_t rom_access_staging[];

#endif /* ZEPHYROS_ROMFS_H */
