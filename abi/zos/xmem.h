/* ZephyrOS native extended-memory and far-call ABI (M10, package 33c/33d).
 *
 * This header owns the argument blocks of the M10 extended-memory family
 * (architecture section 36.7): SYS_XMEM_*, SYS_ZEX_SEGMENT_INFO and
 * SYS_FAR_CALL.  The syscall numbers live in zos/syscall.h with the rest
 * of the native ABI.
 *
 * The 8-KiB slot, the physical bank and the auxiliary low-window layout
 * are Zephyr-80 implementation details (architecture section 36.17): a
 * program sees opaque handles, logical sizes and segment ids only.
 */

#ifndef ZOS_XMEM_H
#define ZOS_XMEM_H

#include "zos/types.h"
#include "zos/errno.h"

/* No allocation flags are defined yet; the field keeps the ABI stable. */
#define ZOS_XMEM_FLAG_NONE 0x00u

/* SYS_XMEM_ALLOC {size:u16, flags:u16} -> HL = opaque handle.  A size of
 * zero is ZOS_EINVAL.  A runtime allocation is process-owned,
 * zero-initialized, read/write and non-executable. */
struct zos_xmem_alloc_args {
	uint16_t size;
	uint16_t flags;
};

/* SYS_XMEM_FREE {handle:u16}. */
struct zos_xmem_free_args {
	uint16_t handle;
};

/* SYS_XMEM_READ / SYS_XMEM_WRITE
 * {handle:u16, offset:u16, buffer:u16, length:u16}.  The transfer is
 * between the caller's current execution bank and the named far object;
 * every range is checked before any copy. */
struct zos_xmem_io_args {
	uint16_t handle;
	uint16_t offset;
	uint16_t buffer;
	uint16_t length;
};

/* SYS_XMEM_INFO {handle:u16, buffer:u16} -> the record below, written to
 * `buffer` (a caller-window address).  A normal program is told the
 * logical description only; physical placement belongs to diagnostics. */
struct zos_xmem_info {
	uint16_t requested; /* bytes requested at allocation */
	uint16_t allocated; /* bytes actually reserved     */
	uint16_t flags;
	uint8_t  type;      /* ZOS_XMEM_TYPE_*             */
	uint8_t  reserved;
};

#define ZOS_XMEM_TYPE_RUNTIME 1u /* SYS_XMEM_ALLOC object */
#define ZOS_XMEM_TYPE_SEGMENT 2u /* loader-created ZEX segment */

struct zos_xmem_info_args {
	uint16_t handle;
	uint16_t buffer;
};

/* SYS_ZEX_SEGMENT_INFO {segment_id:u8, reserved:u8, buffer:u16} -> the
 * record below at `buffer`.  Identified by the ZEX-local segment id. */
struct zos_zex_seg_info {
	uint16_t image_size;
	uint16_t bss_size;
	uint8_t  flags;  /* ZEX2_SEG_FLAG_* */
	uint8_t  state;  /* ZOS_XMEM_SEG_PRESENT when loaded */
	uint16_t reserved;
};

#define ZOS_XMEM_SEG_ABSENT  0u
#define ZOS_XMEM_SEG_PRESENT 1u

struct zos_zex_seg_info_args {
	uint8_t  segment_id;
	uint8_t  reserved;
	uint16_t buffer;
};

/* SYS_FAR_CALL {entry:u8, reserved:u8, in:u16, in_len:u16, out:u16,
 * out_len:u16}.  The kernel copies the bounded in-frame out to the far
 * entry and the out-frame back into the caller's `out` buffer; the call
 * frame maximum is an architectural constant (architecture 36.13). */
#define ZOS_FAR_FRAME_MAX 32u
#define ZOS_FAR_ENTRY_MAX 16u

struct zos_far_call_args {
	uint8_t  entry;
	uint8_t  reserved;
	uint16_t in;
	uint16_t in_len;
	uint16_t out;
	uint16_t out_len;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct zos_xmem_alloc_args) == 4,
	       "xmem alloc block must be 4 packed bytes");
_Static_assert(sizeof(struct zos_xmem_free_args) == 2,
	       "xmem free block must be 2 packed bytes");
_Static_assert(sizeof(struct zos_xmem_io_args) == 8,
	       "xmem io block must be 8 packed bytes");
_Static_assert(sizeof(struct zos_xmem_info) == 8,
	       "xmem info record must be 8 packed bytes");
_Static_assert(sizeof(struct zos_xmem_info_args) == 4,
	       "xmem info block must be 4 packed bytes");
_Static_assert(sizeof(struct zos_zex_seg_info) == 8,
	       "segment info record must be 8 packed bytes");
_Static_assert(sizeof(struct zos_zex_seg_info_args) == 4,
	       "segment info block must be 4 packed bytes");
_Static_assert(sizeof(struct zos_far_call_args) == 10,
	       "far-call block must be 10 packed bytes");
#endif

#endif /* ZOS_XMEM_H */
