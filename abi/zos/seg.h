/* ZephyrOS native segment ABI (public native ABI, package 44a).
 *
 * A ZEX v2 container declares zero or more auxiliary segments in its
 * segment table (architecture 36.18).  A `DATA` segment becomes an
 * extended-memory object owned by the process (36.16): the loader places
 * it before the program runs, `SYS_SEG_INFO` names it and returns the
 * object handle, and the contents are read and written with the
 * abi/zos/xmem.h calls.  A `FAR_CODE` segment is the package-44b kind
 * (executed in place through the aperture); it has no handle and this
 * package's loader refuses it.
 *
 *   SYS_SEG_INFO  a segment's logical size, kind and load state
 *
 * Segment ids are the container's table ids: 1..segment_count, in table
 * order.  An id outside the container, or a segment of another process,
 * is refused: the table is the calling process's own.
 */

#ifndef ZOS_SEG_H
#define ZOS_SEG_H

#include "zos/types.h"
#include "zos/errno.h"

/* --- native syscall numbers (docs/abi.md section 15) -------------------- */

#define ZOS_SYSCALL_SEG_INFO 0x26u

/* --- segment kinds ------------------------------------------------------ */

/* The loader refuses a `FAR_CODE` segment until package 44b; `SYS_SEG_INFO`
 * therefore reports only `DATA` in M10 step 10a. */
#define ZOS_SEG_KIND_FAR_CODE 1u
#define ZOS_SEG_KIND_DATA     2u

/* --- load states -------------------------------------------------------- */

/* Every segment of the running process's container is placed by the
 * loader or the whole exec fails, so a live segment reports LOADED. */
#define ZOS_SEG_STATE_LOADED 1u

/* SYS_SEG_INFO 26h {segment:u8, reserved:u8, buffer:u16}
 *   -> the 8-byte record below at `buffer`, HL = 0.
 *
 * `segment` is the container id (1..count).  `handle` is the
 * extended-memory handle of a `DATA` segment, for SYS_XMEM_READ /
 * SYS_XMEM_WRITE; a `FAR_CODE` segment has none and reports 0.  An
 * unknown id, a bad block or a buffer outside the caller window is
 * ZOS_EINVAL, and `SYS_XMEM_FREE` refuses the handle (a loader segment is
 * released only at exit). */
struct zos_seg_info {
	uint16_t size;   /* image + BSS, the segment's logical size */
	uint8_t  kind;   /* ZOS_SEG_KIND_*                          */
	uint8_t  state;  /* ZOS_SEG_STATE_*                         */
	uint16_t handle; /* DATA: the xmem object handle; else 0    */
	uint16_t reserved;
};

struct zos_seg_info_args {
	uint8_t  segment;
	uint8_t  reserved;
	uint16_t buffer;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct zos_seg_info) == 8,
	       "seg info record must be 8 packed bytes");
_Static_assert(sizeof(struct zos_seg_info_args) == 4,
	       "seg info block must be 4 packed bytes");
_Static_assert(__builtin_offsetof(struct zos_seg_info, size) == 0,
	       "seg info size is the first word");
_Static_assert(__builtin_offsetof(struct zos_seg_info, kind) == 2,
	       "seg info kind follows the size");
_Static_assert(__builtin_offsetof(struct zos_seg_info, state) == 3,
	       "seg info state follows the kind");
_Static_assert(__builtin_offsetof(struct zos_seg_info, handle) == 4,
	       "seg info handle follows the state");
_Static_assert(__builtin_offsetof(struct zos_seg_info_args, buffer) == 2,
	       "seg info buffer is the second word");
#endif

#endif /* ZOS_SEG_H */
