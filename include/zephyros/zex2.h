/* ZephyrOS M10 ZEX v2 native-executable container format (design spike).
 *
 * ZEX v2 is the M10 container: a primary image (the v1 load model,
 * unchanged logical 0100h placement) plus zero or more relocatable
 * auxiliary segments that carry far code/data, plus a far-entry table.
 * It is packed by tools/zexpack.py (format "zex-v2") and loaded by the
 * ZOS_M10_SPIKE resident loader in kernel/process/exec.c.
 *
 * The container is deliberately host-precomputed: the packer resolves
 * every address, encodes every relocation and proves every geometry
 * bound, so the resident loader only reads descriptors and applies
 * patches (work/M10-REFERENCE.md, "Host-side work").
 *
 * All multi-byte fields are little-endian.  All file geometry is 32-bit
 * (architecture section 36.10): a ZEX v2 file may exceed 64 KiB.
 *
 * Fixed 40-byte header:
 *
 *   00 magic[4]               "ZEX2"
 *   04 format_version         2
 *   05 abi_version            1
 *   06 flags                  0
 *   07 reserved0              0
 *   08 header_size            40
 *   0A segment_count          auxiliary segment count (0..MAX)
 *   0B far_entry_count        far-entry count (0..MAX)
 *   0C primary_image_size     bytes copied to 0100h
 *   0E primary_bss_size       zero bytes after the primary image
 *   10 primary_entry_offset   entry = 0100h + entry_offset
 *   12 stack_size             primary stack reservation
 *   14 total_slots            total 8-KiB slots the loader must allocate
 *   16 file_size              32-bit total container size
 *   1A primary_payload_offset 32-bit file offset of the primary payload
 *   1E segment_table_offset   32-bit file offset of the segment table
 *   22 far_entry_table_offset 32-bit file offset of the far-entry table
 *   26 reserved1              0
 *
 * Then, in file order: the primary payload, the segment table
 * (segment_count * 20 bytes), the far-entry table (far_entry_count * 4
 * bytes), the auxiliary segment payloads, and the relocation streams.
 *
 * A segment is linked at logical 0000h and loaded on an 8-KiB slot
 * boundary inside one auxiliary bank (architecture section 36.3/36.11).
 * Every ABS16 relocation is emitted as a patch offset; because a slot
 * base is a multiple of 256, adding it to a 16-bit value changes only
 * the value's high byte, so the loader patches exactly one byte per
 * relocation (slot_base_high added at patch_offset + 1).
 */

#ifndef ZEPHYROS_ZEX2_H
#define ZEPHYROS_ZEX2_H

#include "zos/types.h"

/* --- format constants ------------------------------------------------- */

#define ZEX2_MAGIC_0 'Z' /* 0x5A */
#define ZEX2_MAGIC_1 'E' /* 0x45 */
#define ZEX2_MAGIC_2 'X' /* 0x58 */
#define ZEX2_MAGIC_3 '2' /* 0x32 */

#define ZEX2_FORMAT_VERSION 2u
#define ZEX2_ABI_VERSION    1u
#define ZEX2_FLAG_NONE      0u

#define ZEX2_HEADER_SIZE          40u
#define ZEX2_SEGMENT_ENTRY_SIZE   20u
#define ZEX2_FAR_ENTRY_SIZE       4u

#define ZEX2_LOAD_BASE       0x0100u /* primary image load address    */
#define ZEX2_STACK_TOP       0xE000u /* primary stack top             */
#define ZEX2_MIN_STACK_SIZE  0x0002u

/* Auxiliary-bank geometry (architecture section 36.3).  The low 8 KiB of
 * an auxiliary bank is the execution/control window; allocation slots
 * start at 2000h and there are six of them. */
#define ZEX2_AUX_SLOT_SIZE   0x2000u
#define ZEX2_AUX_SLOT_BASE   0x2000u /* first allocatable slot base   */
#define ZEX2_AUX_SLOT_COUNT  6u
#define ZEX2_AUX_SLOT_LIMIT  0xE000u /* exclusive: common begins here */

/* Container limits, deliberately small for the spike.  The loader's
 * up-front feasibility check uses total_slots, so these are only the
 * table-size bounds that protect the kernel. */
#define ZEX2_MAX_SEGMENTS     6u
#define ZEX2_MAX_FAR_ENTRIES  16u

/* Segment flags: intended use, not hardware protection. */
#define ZEX2_SEG_FLAG_EXEC  0x01u
#define ZEX2_SEG_FLAG_WRITE 0x02u
#define ZEX2_SEG_FLAG_MASK  (ZEX2_SEG_FLAG_EXEC | ZEX2_SEG_FLAG_WRITE)

/* --- on-disk structures ----------------------------------------------- */

struct zex2_header {
	uint8_t  magic[4];
	uint8_t  format_version;
	uint8_t  abi_version;
	uint8_t  flags;
	uint8_t  reserved0;
	uint16_t header_size;
	uint8_t  segment_count;
	uint8_t  far_entry_count;
	uint16_t primary_image_size;
	uint16_t primary_bss_size;
	uint16_t primary_entry_offset;
	uint16_t stack_size;
	uint16_t total_slots;
	uint32_t file_size;
	uint32_t primary_payload_offset;
	uint32_t segment_table_offset;
	uint32_t far_entry_table_offset;
	uint16_t reserved1;
};

struct zex2_segment {
	uint8_t  id;
	uint8_t  flags;
	uint16_t image_size;
	uint16_t bss_size;
	uint16_t alloc_size;   /* bytes reserved: slot-rounded, >= 1 slot */
	uint32_t payload_offset;
	uint32_t reloc_offset;
	uint16_t reloc_count;  /* u16 patch offsets */
	uint16_t reserved;
};

struct zex2_far_entry {
	uint8_t  id;
	uint8_t  segment_id;
	uint16_t offset;
};

/* --- load result codes ------------------------------------------------ */

/* The v2 loader reuses the v1 ZEX_EXEC_ERR_* taxonomy from
 * zephyros/zex.h where the boundary is the same, and adds the
 * v2-specific boundaries below.  Values stay disjoint from v1. */
#define ZEX2_EXEC_ERR_SEG_COUNT   (-0x41) /* segment_count > MAX        */
#define ZEX2_EXEC_ERR_FAR_COUNT   (-0x42) /* far_entry_count > MAX      */
#define ZEX2_EXEC_ERR_SLOTS       (-0x43) /* total_slots > available    */
#define ZEX2_EXEC_ERR_SEGMENT     (-0x44) /* segment geometry invalid   */
#define ZEX2_EXEC_ERR_RELOC       (-0x45) /* patch offset out of range  */
#define ZEX2_EXEC_ERR_FAR         (-0x46) /* far entry out of range     */

#if defined(__SDCC)
_Static_assert(sizeof(struct zex2_header) == ZEX2_HEADER_SIZE,
	       "ZEX v2 header must be 40 bytes");
_Static_assert(__builtin_offsetof(struct zex2_header, header_size) == 0x08,
	       "ZEX v2 header_size offset");
_Static_assert(__builtin_offsetof(struct zex2_header, segment_count) == 0x0A,
	       "ZEX v2 segment_count offset");
_Static_assert(__builtin_offsetof(struct zex2_header, total_slots) == 0x14,
	       "ZEX v2 total_slots offset");
_Static_assert(__builtin_offsetof(struct zex2_header, file_size) == 0x16,
	       "ZEX v2 file_size offset");
_Static_assert(__builtin_offsetof(struct zex2_header, primary_payload_offset)
	       == 0x1A, "ZEX v2 primary_payload_offset offset");
_Static_assert(__builtin_offsetof(struct zex2_header, segment_table_offset)
	       == 0x1E, "ZEX v2 segment_table_offset offset");
_Static_assert(__builtin_offsetof(struct zex2_header, far_entry_table_offset)
	       == 0x22, "ZEX v2 far_entry_table_offset offset");
_Static_assert(sizeof(struct zex2_segment) == ZEX2_SEGMENT_ENTRY_SIZE,
	       "ZEX v2 segment entry must be 20 bytes");
_Static_assert(sizeof(struct zex2_far_entry) == ZEX2_FAR_ENTRY_SIZE,
	       "ZEX v2 far entry must be 4 bytes");
#endif /* __SDCC */

#endif /* ZEPHYROS_ZEX2_H */
