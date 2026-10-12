/* KonaOS M10 EXE v2 native executable container.
 *
 * EXE v2 is the only executable container (architecture 36.18): a primary
 * image loaded at logical 0100h plus zero or more auxiliary segments.
 * It is packed by tools/exepack.py and loaded from the namespace by
 * kernel/process/exec.c; see docs/exe-format.md.
 *
 * Packed little-endian 36-byte header, then the primary payload, the
 * segment table and the segment payloads:
 *
 *   00 magic[4]                "KEX2"
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
 *   01 kind           EXE_SEG_KIND_*
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
 * mirrored by tools/exepack.py (the host parity test cross-checks them).
 */

#ifndef KONAOS_EXE_H
#define KONAOS_EXE_H

#include "kos/types.h"
#include "kos/errno.h"

/* --- format constants ------------------------------------------------- */

#define EXE_MAGIC_0 'K' /* 0x4B */
#define EXE_MAGIC_1 'E' /* 0x45 */
#define EXE_MAGIC_2 'X' /* 0x58 */
#define EXE_MAGIC_3 '2' /* 0x32 */

#define EXE_FORMAT_VERSION 2u
#define EXE_ABI_VERSION    1u
#define EXE_FLAG_NONE      0u

#define EXE_HEADER_SIZE        36u
#define EXE_SEGMENT_ENTRY_SIZE 16u

#define EXE_LOAD_BASE   0x0100u /* logical load address           */
#define EXE_STACK_TOP   0xE000u /* initial SP; stack grows down   */
#define EXE_MIN_STACK_SIZE 0x0002u

/* Segment kinds.  `DATA` segments become extended-memory objects owned by
 * the process (architecture 36.16/36.18); `FAR_CODE` segments are linked
 * at 8000h and loaded into the aperture slice of an auxiliary bank, one
 * per bank, and executed in place through the mode-10 aperture (36.17):
 * the largest far-code segment is the 16 KiB aperture. */
#define EXE_SEG_KIND_FAR_CODE 1u
#define EXE_SEG_KIND_DATA     2u

/* Container limits.  The feasibility check uses the slot total: an
 * auxiliary object is contiguous in one bank, a data segment occupies at
 * most the four non-aperture slots of its bank, a far-code segment at
 * most the two aperture slots of its bank, and the child's primary bank
 * can never hold auxiliary slots, so a container is bounded by five
 * banks' worth; the packer enforces the same ceiling on the host. */
#define EXE_MAX_SEGMENTS    8u
#define EXE_MAX_TOTAL_SLOTS 20u
#define EXE_SEG_SLOT_SIZE   0x2000u /* one auxiliary slot          */
#define EXE_SEG_MAX_SIZE    0x8000u /* image + BSS, the largest object */
#define EXE_SEG_MAX_FAR_SIZE 0x4000u /* image + BSS, the aperture slice */

/* On-disk header, little-endian, packed. */
struct exe_header {
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
struct exe_segment {
	uint8_t  id;
	uint8_t  kind;
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t reserved0;
	uint32_t payload_offset;
	uint32_t reserved1;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct exe_header) == EXE_HEADER_SIZE,
	       "EXE header must be 36 bytes");
_Static_assert(sizeof(struct exe_segment) == EXE_SEGMENT_ENTRY_SIZE,
	       "EXE segment row must be 16 bytes");
_Static_assert(__builtin_offsetof(struct exe_header, magic) == 0x00,
	       "EXE magic offset");
_Static_assert(__builtin_offsetof(struct exe_header, format_version) == 0x04,
	       "EXE format_version offset");
_Static_assert(__builtin_offsetof(struct exe_header, abi_version) == 0x05,
	       "EXE abi_version offset");
_Static_assert(__builtin_offsetof(struct exe_header, flags) == 0x06,
	       "EXE flags offset");
_Static_assert(__builtin_offsetof(struct exe_header, reserved0) == 0x07,
	       "EXE reserved0 offset");
_Static_assert(__builtin_offsetof(struct exe_header, header_size) == 0x08,
	       "EXE header_size offset");
_Static_assert(__builtin_offsetof(struct exe_header, segment_count) == 0x0A,
	       "EXE segment_count offset");
_Static_assert(__builtin_offsetof(struct exe_header,
				   total_segment_slots) == 0x0B,
	       "EXE total_segment_slots offset");
_Static_assert(__builtin_offsetof(struct exe_header, image_size) == 0x0C,
	       "EXE image_size offset");
_Static_assert(__builtin_offsetof(struct exe_header, bss_size) == 0x0E,
	       "EXE bss_size offset");
_Static_assert(__builtin_offsetof(struct exe_header, entry_offset) == 0x10,
	       "EXE entry_offset offset");
_Static_assert(__builtin_offsetof(struct exe_header, stack_size) == 0x12,
	       "EXE stack_size offset");
_Static_assert(__builtin_offsetof(struct exe_header,
				   segment_table_offset) == 0x14,
	       "EXE segment_table_offset offset");
_Static_assert(__builtin_offsetof(struct exe_header,
				   primary_payload_offset) == 0x18,
	       "EXE primary_payload_offset offset");
_Static_assert(__builtin_offsetof(struct exe_header, file_size) == 0x1C,
	       "EXE file_size offset");
_Static_assert(__builtin_offsetof(struct exe_header, reserved1) == 0x20,
	       "EXE reserved1 offset");
_Static_assert(__builtin_offsetof(struct exe_segment, id) == 0x00,
	       "EXE segment id offset");
_Static_assert(__builtin_offsetof(struct exe_segment, kind) == 0x01,
	       "EXE segment kind offset");
_Static_assert(__builtin_offsetof(struct exe_segment, image_size) == 0x02,
	       "EXE segment image_size offset");
_Static_assert(__builtin_offsetof(struct exe_segment, bss_size) == 0x04,
	       "EXE segment bss_size offset");
_Static_assert(__builtin_offsetof(struct exe_segment, payload_offset) == 0x08,
	       "EXE segment payload_offset offset");
#endif /* __SDCC */

/* --- load result codes ------------------------------------------------ */

/* process_exec() returns EXE_EXEC_OK after the child ran and exited (the
 * child's status is delivered through the out parameter), or one of the
 * negative codes below.  Every format boundary has a distinct code so
 * target acceptance can assert the exact rejection without inspecting
 * console text; the numeric values are internal and mirrored by
 * tools/exepack.py. */
#define EXE_EXEC_OK            0
#define EXE_EXEC_ERR_ARG       (-0x21) /* invalid API argument         */
#define EXE_EXEC_ERR_ROMFS     (-0x22) /* lookup/read failed           */
#define EXE_EXEC_ERR_TYPE      (-0x23) /* entry is not a EXE           */
#define EXE_EXEC_ERR_NOSPACE   (-0x24) /* no free slot/bank/aux space  */
#define EXE_EXEC_ERR_READ      (-0x25) /* payload read/copy failed     */
#define EXE_EXEC_ERR_MAGIC     (-0x31)
#define EXE_EXEC_ERR_FORMAT    (-0x32) /* format_version field         */
#define EXE_EXEC_ERR_ABI       (-0x33) /* abi_version field            */
#define EXE_EXEC_ERR_FLAGS     (-0x34) /* flags field                  */
#define EXE_EXEC_ERR_RESERVED0 (-0x35)
#define EXE_EXEC_ERR_HEADER    (-0x36) /* header_size field            */
#define EXE_EXEC_ERR_RESERVED1 (-0x37)
#define EXE_EXEC_ERR_IMAGE     (-0x38) /* zero image                   */
#define EXE_EXEC_ERR_FILESIZE  (-0x39) /* file_size field mismatch     */
#define EXE_EXEC_ERR_ENTRY     (-0x3A) /* entry outside the image      */
#define EXE_EXEC_ERR_STACK     (-0x3B) /* stack_size < 2               */
#define EXE_EXEC_ERR_IMAGE_END (-0x3C) /* image reaches E000h/overflows */
#define EXE_EXEC_ERR_BSS_END   (-0x3D) /* BSS reaches E000h/overflows  */
#define EXE_EXEC_ERR_COLLISION (-0x3E) /* BSS/image overlaps the stack */
#define EXE_EXEC_ERR_SEG_SLOTS (-0x3F) /* segment count/slot total     */
#define EXE_EXEC_ERR_SEG_TABLE (-0x40) /* table outside the file       */
#define EXE_EXEC_ERR_SEGMENT   (-0x41) /* invalid segment row          */
#define EXE_EXEC_ERR_SEG_KIND  (-0x42) /* unknown/unsupported kind     */

/* Validated primary geometry of one EXE file (absolute logical
 * addresses). */
struct exe_geometry {
	uint16_t image_size; /* bytes copied to EXE_LOAD_BASE      */
	uint16_t bss_size;   /* zero bytes after the image         */
	uint16_t entry;      /* EXE_LOAD_BASE + entry_offset       */
	uint16_t stack_size;
	uint16_t stack_base; /* EXE_STACK_TOP - stack_size         */
};

/* The one up-front feasibility check: validate the header against the
 * complete file size.  Returns 0 or one of the EXE_EXEC_ERR_* codes
 * above.  `file_size` is the source FILE's size; the header must describe
 * the file exactly, and the segment table and primary payload must lie
 * inside it.  No global state is touched on failure. */
int exe_validate_header(const struct exe_header *header, uint32_t file_size,
			struct exe_geometry *out);

/* Protective bounds for one segment-table row, checked before it is
 * allocated or copied (the packer proves the same geometry on the host).
 * Returns 0 or EXE_EXEC_ERR_SEGMENT / EXE_EXEC_ERR_SEG_KIND. */
int exe_validate_segment(const struct exe_segment *segment,
			 uint32_t file_size);

#endif /* KONAOS_EXE_H */
