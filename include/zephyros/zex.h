/* ZephyrOS M6 ZEX v1 single-bank native executable format.
 *
 * ZEX v1 is the first native executable container: one application bank,
 * one code/data image, one BSS region, one entry point, one stack.  It
 * is packed by tools/zexpack.py and loaded from the ROMFS recovery store
 * by kernel/process/exec.c; see docs/zex-format.md.
 *
 * Packed little-endian 20-byte header, immediately followed by the image:
 *
 *   00 magic[4]        "ZEX1"
 *   04 format_version  1
 *   05 abi_version     1
 *   06 flags           0
 *   07 reserved0       0
 *   08 header_size     20
 *   0A image_size      bytes copied to 0100h
 *   0C bss_size        bytes zeroed after the image
 *   0E entry_offset    entry = 0100h + entry_offset (inside the image)
 *   10 stack_size      reserved stack [E000h-stack_size, E000h)
 *   12 reserved1       0
 *
 * The file size must equal header_size + image_size.  The loader rejects
 * overlap, a zero image, stack sizes below two bytes, an entry outside
 * the image, arithmetic wrap, and any image/BSS/stack collision before
 * any bank content changes.
 *
 * The header struct may be overlaid on container bytes; SDCC z80 does
 * not insert padding and the compile-time assertions pin every field
 * offset.  The result codes below are produced by this reader and
 * mirrored by tools/zexpack.py (the host parity test cross-checks them).
 */

#ifndef ZEPHYROS_ZEX_H
#define ZEPHYROS_ZEX_H

#include "zos/types.h"
#include "zephyros/errno.h"

/* --- format constants ------------------------------------------------- */

#define ZEX_MAGIC_0 'Z' /* 0x5A */
#define ZEX_MAGIC_1 'E' /* 0x45 */
#define ZEX_MAGIC_2 'X' /* 0x58 */
#define ZEX_MAGIC_3 '1' /* 0x31 */

#define ZEX_FORMAT_VERSION 1u
#define ZEX_ABI_VERSION    1u
#define ZEX_FLAG_NONE      0u

#define ZEX_HEADER_SIZE 20u
#define ZEX_LOAD_BASE   0x0100u /* logical load address           */
#define ZEX_STACK_TOP   0xE000u /* initial SP; stack grows down   */
#define ZEX_MIN_STACK_SIZE 0x0002u

/* On-disk header, little-endian, packed. */
struct zex_header {
	uint8_t  magic[4];
	uint8_t  format_version;
	uint8_t  abi_version;
	uint8_t  flags;
	uint8_t  reserved0;
	uint16_t header_size;
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t entry_offset;
	uint16_t stack_size;
	uint16_t reserved1;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct zex_header) == ZEX_HEADER_SIZE,
	       "ZEX header must be 20 bytes");
_Static_assert(__builtin_offsetof(struct zex_header, magic) == 0x00,
	       "ZEX magic offset");
_Static_assert(__builtin_offsetof(struct zex_header, format_version) == 0x04,
	       "ZEX format_version offset");
_Static_assert(__builtin_offsetof(struct zex_header, abi_version) == 0x05,
	       "ZEX abi_version offset");
_Static_assert(__builtin_offsetof(struct zex_header, flags) == 0x06,
	       "ZEX flags offset");
_Static_assert(__builtin_offsetof(struct zex_header, reserved0) == 0x07,
	       "ZEX reserved0 offset");
_Static_assert(__builtin_offsetof(struct zex_header, header_size) == 0x08,
	       "ZEX header_size offset");
_Static_assert(__builtin_offsetof(struct zex_header, image_size) == 0x0A,
	       "ZEX image_size offset");
_Static_assert(__builtin_offsetof(struct zex_header, bss_size) == 0x0C,
	       "ZEX bss_size offset");
_Static_assert(__builtin_offsetof(struct zex_header, entry_offset) == 0x0E,
	       "ZEX entry_offset offset");
_Static_assert(__builtin_offsetof(struct zex_header, stack_size) == 0x10,
	       "ZEX stack_size offset");
_Static_assert(__builtin_offsetof(struct zex_header, reserved1) == 0x12,
	       "ZEX reserved1 offset");
#endif /* __SDCC */

/* --- load result codes ------------------------------------------------ */

/* process_exec() returns ZEX_EXEC_OK after the child ran and exited (the
 * child's status is delivered through the out parameter), or one of the
 * negative codes below.  Every format boundary has a distinct code so
 * target acceptance can assert the exact rejection without inspecting
 * console text; the numeric values are internal and mirrored by
 * tools/zexpack.py. */
#define ZEX_EXEC_OK            0
#define ZEX_EXEC_ERR_ARG       (-0x21) /* invalid API argument         */
#define ZEX_EXEC_ERR_ROMFS     (-0x22) /* ROMFS lookup/read failed     */
#define ZEX_EXEC_ERR_TYPE      (-0x23) /* ROMFS entry is not ZEX       */
#define ZEX_EXEC_ERR_NOSPACE   (-0x24) /* no free process slot/bank    */
#define ZEX_EXEC_ERR_READ      (-0x25) /* payload read failed          */
#define ZEX_EXEC_ERR_MAGIC     (-0x31)
#define ZEX_EXEC_ERR_FORMAT    (-0x32) /* format_version field         */
#define ZEX_EXEC_ERR_ABI       (-0x33) /* abi_version field            */
#define ZEX_EXEC_ERR_FLAGS     (-0x34) /* flags field                  */
#define ZEX_EXEC_ERR_RESERVED0 (-0x35)
#define ZEX_EXEC_ERR_HEADER    (-0x36) /* header_size field            */
#define ZEX_EXEC_ERR_RESERVED1 (-0x37)
#define ZEX_EXEC_ERR_IMAGE     (-0x38) /* zero image                   */
#define ZEX_EXEC_ERR_FILESIZE  (-0x39) /* file != header_size + image  */
#define ZEX_EXEC_ERR_ENTRY     (-0x3A) /* entry outside the image      */
#define ZEX_EXEC_ERR_STACK     (-0x3B) /* stack_size < 2               */
#define ZEX_EXEC_ERR_IMAGE_END (-0x3C) /* image reaches E000h/overflows */
#define ZEX_EXEC_ERR_BSS_END   (-0x3D) /* BSS reaches E000h/overflows  */
#define ZEX_EXEC_ERR_COLLISION (-0x3E) /* BSS/image overlaps the stack */

/* Validated geometry of one ZEX file (absolute logical addresses). */
struct zex_geometry {
	uint16_t image_size; /* bytes copied to ZEX_LOAD_BASE      */
	uint16_t bss_size;   /* zero bytes after the image         */
	uint16_t entry;      /* ZEX_LOAD_BASE + entry_offset       */
	uint16_t stack_size;
	uint16_t stack_base; /* ZEX_STACK_TOP - stack_size         */
};

/* Validate a header plus the complete file size.  Returns 0 or one of
 * the ZEX_EXEC_ERR_* codes above.  `file_size` is the ROMFS payload
 * size; the header must describe the file exactly.  No global state is
 * touched on failure. */
int zex_validate_header(const struct zex_header *header, uint32_t file_size,
			struct zex_geometry *out);

#endif /* ZEPHYROS_ZEX_H */
