/* ZephyrOS M10 ZEX v2 native executable container.
 *
 * ZEX v2 is the only executable container (architecture 36.18): a primary
 * image loaded at logical 0100h plus zero or more auxiliary segments.
 * It is packed by tools/zexpack.py and loaded from the namespace by
 * kernel/process/exec.c; see docs/zex-format.md.
 *
 * Packed little-endian 36-byte header, then the primary payload, the
 * segment table and the segment payloads:
 *
 *   00 magic[4]                "ZEX2"
 *   04 format_version          2
 *   05 abi_version             1
 *   06 flags                   0
 *   07 reserved0               0
 *   08 header_size             36
 *   0A segment_count           rows in the segment table (0..8)
 *   0B total_segment_slots     whole 8-KiB slots all segments need
 *   0C primary_image_size      bytes copied to 0100h
 *   0E primary_bss_size        bytes zeroed after the primary image
 *   10 entry_offset            entry = 0100h + entry_offset
 *   12 stack_size              reserved stack [E000h-stack_size, E000h)
 *   14 segment_table_offset    u32 file offset of the segment table
 *   18 primary_payload_offset  u32 file offset of the primary payload
 *   1C file_size               u32 total container size
 *   20 reserved1               u32, 0
 *
 * Segment table row (16 bytes):
 *
 *   00 id             1..segment_count, ascending
 *   01 kind           ZEX_SEG_KIND_*
 *   02 image_size     bytes copied into the segment's memory
 *   04 bss_size       bytes zeroed after the segment image
 *   06 reserved0      0
 *   08 payload_offset u32 file offset of the segment payload
 *   0C reserved1      0
 *
 * Every file offset and the total size are 32-bit, so a container may
 * exceed 64 KiB.  The loader's up-front feasibility check reads the whole
 * header (image/BSS/stack geometry, segment_count, total_segment_slots,
 * table and payload placement) before it allocates or writes anything;
 * the host packer proves the same boundaries so a bad link cannot reach
 * the store.
 *
 * The header and row structs may be overlaid on container bytes; SDCC
 * z80 does not insert padding and the compile-time assertions pin every
 * field offset.  The result codes below are produced by this reader and
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
#define ZEX_MAGIC_3 '2' /* 0x32 */

#define ZEX_FORMAT_VERSION 2u
#define ZEX_ABI_VERSION    1u
#define ZEX_FLAG_NONE      0u

#define ZEX_HEADER_SIZE        36u
#define ZEX_SEGMENT_ENTRY_SIZE 16u

#define ZEX_LOAD_BASE   0x0100u /* logical load address           */
#define ZEX_STACK_TOP   0xE000u /* initial SP; stack grows down   */
#define ZEX_MIN_STACK_SIZE 0x0002u

/* Segment kinds.  `DATA` segments become extended-memory objects owned by
 * the process (architecture 36.16/36.18); `FAR_CODE` segments are linked
 * at 8000h and loaded into the aperture slice of an auxiliary bank, one
 * per bank, and executed in place through the mode-10 aperture (36.17):
 * the largest far-code segment is the 16 KiB aperture. */
#define ZEX_SEG_KIND_FAR_CODE 1u
#define ZEX_SEG_KIND_DATA     2u

/* Container limits.  The feasibility check uses the slot total: an
 * auxiliary object is contiguous in one bank, a data segment occupies at
 * most the four non-aperture slots of its bank, a far-code segment at
 * most the two aperture slots of its bank, and the child's primary bank
 * can never hold auxiliary slots, so a container is bounded by five
 * banks' worth; the packer enforces the same ceiling on the host. */
#define ZEX_MAX_SEGMENTS    8u
#define ZEX_MAX_TOTAL_SLOTS 20u
#define ZEX_SEG_SLOT_SIZE   0x2000u /* one auxiliary slot          */
#define ZEX_SEG_MAX_SIZE    0x8000u /* image + BSS, the largest object */
#define ZEX_SEG_MAX_FAR_SIZE 0x4000u /* image + BSS, the aperture slice */

/* On-disk header, little-endian, packed. */
struct zex_header {
	uint8_t  magic[4];
	uint8_t  format_version;
	uint8_t  abi_version;
	uint8_t  flags;
	uint8_t  reserved0;
	uint16_t header_size;
	uint8_t  segment_count;
	uint8_t  total_segment_slots;
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t entry_offset;
	uint16_t stack_size;
	uint32_t segment_table_offset;
	uint32_t primary_payload_offset;
	uint32_t file_size;
	uint32_t reserved1;
};

/* One segment-table row, little-endian, packed. */
struct zex_segment {
	uint8_t  id;
	uint8_t  kind;
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t reserved0;
	uint32_t payload_offset;
	uint32_t reserved1;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct zex_header) == ZEX_HEADER_SIZE,
	       "ZEX header must be 36 bytes");
_Static_assert(sizeof(struct zex_segment) == ZEX_SEGMENT_ENTRY_SIZE,
	       "ZEX segment row must be 16 bytes");
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
_Static_assert(__builtin_offsetof(struct zex_header, segment_count) == 0x0A,
	       "ZEX segment_count offset");
_Static_assert(__builtin_offsetof(struct zex_header,
				   total_segment_slots) == 0x0B,
	       "ZEX total_segment_slots offset");
_Static_assert(__builtin_offsetof(struct zex_header, image_size) == 0x0C,
	       "ZEX image_size offset");
_Static_assert(__builtin_offsetof(struct zex_header, bss_size) == 0x0E,
	       "ZEX bss_size offset");
_Static_assert(__builtin_offsetof(struct zex_header, entry_offset) == 0x10,
	       "ZEX entry_offset offset");
_Static_assert(__builtin_offsetof(struct zex_header, stack_size) == 0x12,
	       "ZEX stack_size offset");
_Static_assert(__builtin_offsetof(struct zex_header,
				   segment_table_offset) == 0x14,
	       "ZEX segment_table_offset offset");
_Static_assert(__builtin_offsetof(struct zex_header,
				   primary_payload_offset) == 0x18,
	       "ZEX primary_payload_offset offset");
_Static_assert(__builtin_offsetof(struct zex_header, file_size) == 0x1C,
	       "ZEX file_size offset");
_Static_assert(__builtin_offsetof(struct zex_header, reserved1) == 0x20,
	       "ZEX reserved1 offset");
_Static_assert(__builtin_offsetof(struct zex_segment, id) == 0x00,
	       "ZEX segment id offset");
_Static_assert(__builtin_offsetof(struct zex_segment, kind) == 0x01,
	       "ZEX segment kind offset");
_Static_assert(__builtin_offsetof(struct zex_segment, image_size) == 0x02,
	       "ZEX segment image_size offset");
_Static_assert(__builtin_offsetof(struct zex_segment, bss_size) == 0x04,
	       "ZEX segment bss_size offset");
_Static_assert(__builtin_offsetof(struct zex_segment, payload_offset) == 0x08,
	       "ZEX segment payload_offset offset");
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
#define ZEX_EXEC_ERR_ROMFS     (-0x22) /* lookup/read failed           */
#define ZEX_EXEC_ERR_TYPE      (-0x23) /* entry is not a ZEX           */
#define ZEX_EXEC_ERR_NOSPACE   (-0x24) /* no free slot/bank/aux space  */
#define ZEX_EXEC_ERR_READ      (-0x25) /* payload read/copy failed     */
#define ZEX_EXEC_ERR_MAGIC     (-0x31)
#define ZEX_EXEC_ERR_FORMAT    (-0x32) /* format_version field         */
#define ZEX_EXEC_ERR_ABI       (-0x33) /* abi_version field            */
#define ZEX_EXEC_ERR_FLAGS     (-0x34) /* flags field                  */
#define ZEX_EXEC_ERR_RESERVED0 (-0x35)
#define ZEX_EXEC_ERR_HEADER    (-0x36) /* header_size field            */
#define ZEX_EXEC_ERR_RESERVED1 (-0x37)
#define ZEX_EXEC_ERR_IMAGE     (-0x38) /* zero image                   */
#define ZEX_EXEC_ERR_FILESIZE  (-0x39) /* file_size field mismatch     */
#define ZEX_EXEC_ERR_ENTRY     (-0x3A) /* entry outside the image      */
#define ZEX_EXEC_ERR_STACK     (-0x3B) /* stack_size < 2               */
#define ZEX_EXEC_ERR_IMAGE_END (-0x3C) /* image reaches E000h/overflows */
#define ZEX_EXEC_ERR_BSS_END   (-0x3D) /* BSS reaches E000h/overflows  */
#define ZEX_EXEC_ERR_COLLISION (-0x3E) /* BSS/image overlaps the stack */
#define ZEX_EXEC_ERR_SEG_SLOTS (-0x3F) /* segment count/slot total     */
#define ZEX_EXEC_ERR_SEG_TABLE (-0x40) /* table outside the file       */
#define ZEX_EXEC_ERR_SEGMENT   (-0x41) /* invalid segment row          */
#define ZEX_EXEC_ERR_SEG_KIND  (-0x42) /* unknown/unsupported kind     */

/* Validated primary geometry of one ZEX file (absolute logical
 * addresses). */
struct zex_geometry {
	uint16_t image_size; /* bytes copied to ZEX_LOAD_BASE      */
	uint16_t bss_size;   /* zero bytes after the image         */
	uint16_t entry;      /* ZEX_LOAD_BASE + entry_offset       */
	uint16_t stack_size;
	uint16_t stack_base; /* ZEX_STACK_TOP - stack_size         */
};

/* The one up-front feasibility check: validate the header against the
 * complete file size.  Returns 0 or one of the ZEX_EXEC_ERR_* codes
 * above.  `file_size` is the source FILE's size; the header must describe
 * the file exactly, and the segment table and primary payload must lie
 * inside it.  No global state is touched on failure. */
int zex_validate_header(const struct zex_header *header, uint32_t file_size,
			struct zex_geometry *out);

/* Protective bounds for one segment-table row, checked before it is
 * allocated or copied (the packer proves the same geometry on the host).
 * Returns 0 or ZEX_EXEC_ERR_SEGMENT / ZEX_EXEC_ERR_SEG_KIND. */
int zex_validate_segment(const struct zex_segment *segment,
			 uint32_t file_size);

#endif /* ZEPHYROS_ZEX_H */
