/* KRES format v1 on-disk layout (private; docs/kres-sdk-design.md §4).
 *
 * The format is parsed and emitted byte by byte; no structure is ever
 * overlaid on file bytes.  Every offset below is a byte offset into one
 * 512-byte record, pinned by _Static_assert.  All multibyte integers on
 * disk are little-endian.
 */

#ifndef KRES_LAYOUT_H
#define KRES_LAYOUT_H

#include "kos/types.h"

#define KRES_RECORD_SIZE      512u
#define KRES_CATALOG_RECORDS  4u
#define KRES_ENTRY_SIZE       64u
#define KRES_NAME_MAX         48u
#define KRES_RESOURCE_MAX     32u
#define KRES_RECORD_LIMIT     65535u

/* The header record (record 0). */
#define KRES_HDR_MAGIC        0x00u /* 4 bytes, ASCII "KRES" */
#define KRES_HDR_MAJOR        0x04u /* 2 bytes, 1 */
#define KRES_HDR_MINOR        0x06u /* 2 bytes, 0 */
#define KRES_HDR_RECORD_SIZE  0x08u /* 2 bytes, 512 */
#define KRES_HDR_CATALOG_RECS 0x0Au /* 2 bytes, 4 */
#define KRES_HDR_GENERATION   0x0Cu /* 4 bytes */
#define KRES_HDR_ACTIVE_AREA  0x10u /* 1 byte, 0 = A, 1 = B */
#define KRES_HDR_RESERVED     0x11u /* 1 byte, 0 */
#define KRES_HDR_RES_COUNT    0x12u /* 2 bytes */
#define KRES_HDR_NEXT_RECORD  0x14u /* 4 bytes */
#define KRES_HDR_FLAGS        0x18u /* 4 bytes; bit 0 SEALED */

#define KRES_HDR_FLAG_SEALED  0x01u

#define KRES_HDR_MAJOR_V1     1u
#define KRES_HDR_MINOR_V1     0u

/* Catalog entries (eight per record, never split across records). */
#define KRES_ENT_NAME         0x00u /* 48 bytes, zero-padded */
#define KRES_ENT_START        0x30u /* 4 bytes */
#define KRES_ENT_ALLOCATED    0x34u /* 4 bytes */
#define KRES_ENT_USED         0x38u /* 4 bytes */
#define KRES_ENT_NAME_LENGTH  0x3Cu /* 1 byte, 1..48 */
#define KRES_ENT_FLAGS        0x3Du /* 1 byte, 0 in v1 */
#define KRES_ENT_RESERVED     0x3Eu /* 2 bytes, 0 */

/* Catalog areas (records 1..4 and 5..8; sealed containers use A only
 * and start resources at record 5). */
#define KRES_AREA_A_BASE      1u
#define KRES_AREA_B_BASE      (1u + KRES_CATALOG_RECORDS)
#define KRES_DATA_BASE_MUT     (1u + 2u * KRES_CATALOG_RECORDS)
#define KRES_DATA_BASE_SEALED  (1u + KRES_CATALOG_RECORDS)

#if defined(__SDCC)
_Static_assert(KRES_HDR_MAGIC == 0u, "magic offset");
_Static_assert(KRES_HDR_MAJOR == 4u, "major offset");
_Static_assert(KRES_HDR_GENERATION == 12u, "generation offset");
_Static_assert(KRES_HDR_ACTIVE_AREA == 16u, "active area offset");
_Static_assert(KRES_HDR_RES_COUNT == 18u, "resource count offset");
_Static_assert(KRES_HDR_NEXT_RECORD == 20u, "next_record offset");
_Static_assert(KRES_HDR_FLAGS == 24u, "flags offset");
_Static_assert(KRES_ENT_NAME == 0u, "entry name offset");
_Static_assert(KRES_ENT_START == 48u, "entry start offset");
_Static_assert(KRES_ENT_ALLOCATED == 52u, "entry allocated offset");
_Static_assert(KRES_ENT_USED == 56u, "entry used offset");
_Static_assert(KRES_ENT_NAME_LENGTH == 60u, "entry name_length offset");
_Static_assert(KRES_ENT_FLAGS == 61u, "entry flags offset");
_Static_assert(KRES_ENT_RESERVED == 62u, "entry reserved offset");
_Static_assert(KRES_ENTRY_SIZE == 64u, "entry size");
#endif /* __SDCC */

/* Little-endian accessors; the host edition shares these byte-for-byte
 * helpers so both sides agree on the format by construction. */
void kres_put16(uint8_t *at, uint16_t value);
void kres_put32(uint8_t *at, uint32_t value);
uint16_t kres_get16(const uint8_t *at);
uint32_t kres_get32(const uint8_t *at);

/* Zero one catalog entry before it is filled. */
void kres_ent_clear(uint8_t *entry);

#endif /* KRES_LAYOUT_H */
