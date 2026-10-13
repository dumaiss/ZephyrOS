/* KonaOS native segment ABI (public native ABI, packages 44a/44b).
 *
 * A EXE v2 container declares zero or more auxiliary segments in its
 * segment table (architecture 36.18).  A `DATA` segment becomes an
 * extended-memory object owned by the process (36.16): the loader places
 * it before the program runs, `SYS_SEG_INFO` names it and returns the
 * object handle, and the contents are read and written with the
 * ABI's kos/xmem.h calls.  A `FAR_CODE` segment (36.17) is linked at
 * `8000h`, loaded into the aperture slice of an auxiliary bank and
 * executed in place through the mode-10 aperture: the loader places one
 * far-code segment per auxiliary bank, the code is `8000h` when the
 * aperture is opened onto it, and a far call is an ordinary `CALL` while
 * the aperture is open.  It has no handle; `SYS_SEG_MAP` returns the
 * aperture token.
 *
 *   SYS_SEG_INFO  a segment's logical size, kind and load state
 *   SYS_SEG_MAP   the aperture token of a FAR_CODE segment
 *
 * Segment ids are the container's table ids: 1..segment_count, in table
 * order.  An id outside the container, or a segment of another process,
 * is refused: the table is the calling process's own.
 */

#ifndef KOS_SEG_H
#define KOS_SEG_H

#include "kos/types.h"
#include "kos/errno.h"

/* --- native syscall numbers (docs/abi.md section 15) -------------------- */

#define KOS_SYSCALL_SEG_INFO 0x26u
#define KOS_SYSCALL_SEG_MAP  0x27u

/* --- segment kinds ------------------------------------------------------ */

/* A container's kinds: far code is linked at 8000h and executed in place
 * through the aperture; data becomes a process-owned extended-memory
 * object reached by handle. */
#define KOS_SEG_KIND_FAR_CODE 1u
#define KOS_SEG_KIND_DATA     2u

/* --- load states -------------------------------------------------------- */

/* Every segment of the running process's container is placed by the
 * loader or the whole exec fails, so a live segment reports LOADED. */
#define KOS_SEG_STATE_LOADED 1u

/* SYS_SEG_INFO 26h {segment:u8, reserved:u8, buffer:u16}
 *   -> the 8-byte record below at `buffer`, HL = 0.
 *
 * `segment` is the container id (1..count).  `handle` is the
 * extended-memory handle of a `DATA` segment, for SYS_XMEM_READ /
 * SYS_XMEM_WRITE; a `FAR_CODE` segment has none and reports 0.  `flags`
 * is the segment's KOS_XMEM_FLAG_* set: a `DATA` segment that asked for
 * the aperture (the container row's `EXE_SEG_FLAG_MAPPABLE`, work package
 * 49d) reports `KOS_XMEM_FLAG_MAPPABLE` and its handle can be mapped with
 * SYS_XMEM_MAP.  An unknown id, a bad block or a buffer outside the
 * caller window is KOS_EINVAL, and `SYS_XMEM_FREE` refuses the handle (a
 * loader segment is released only at exit). */
struct kos_seg_info {
	uint16_t size;   /* image + BSS, the segment's logical size */
	uint8_t  kind;   /* KOS_SEG_KIND_*                          */
	uint8_t  state;  /* KOS_SEG_STATE_*                         */
	uint16_t handle; /* DATA: the xmem object handle; else 0    */
	uint16_t flags;  /* KOS_XMEM_FLAG_* of the segment, 0 = none */
};

struct kos_seg_info_args {
	uint8_t  segment;
	uint8_t  reserved;
	uint16_t buffer;
};

/* SYS_SEG_MAP 27h {segment:u8, reserved:u8}
 *   -> HL = the aperture token (>= 1) that opens the mode-10 aperture
 *      onto this FAR_CODE segment.
 *
 * Only the calling process's own `FAR_CODE` segments have a token: an
 * unknown id, a `DATA` segment's id (it is reached by handle, not mapped)
 * or a bad block is KOS_EINVAL.  The segment is always at `8000h`, so no
 * address is returned; the token keeps the caller's own bank field and
 * selects P = 7 - bank with the segment's application bank 0-5, so a
 * token never names bank 6 or bank 7.  The SDK opens the aperture with
 * it (`farcall`) and restores the previous latch. */
struct kos_seg_map_args {
	uint8_t segment;
	uint8_t reserved;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct kos_seg_info) == 8,
	       "seg info record must be 8 packed bytes");
_Static_assert(sizeof(struct kos_seg_info_args) == 4,
	       "seg info block must be 4 packed bytes");
_Static_assert(sizeof(struct kos_seg_map_args) == 2,
	       "seg map block must be 2 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_seg_info, size) == 0,
	       "seg info size is the first word");
_Static_assert(__builtin_offsetof(struct kos_seg_info, kind) == 2,
	       "seg info kind follows the size");
_Static_assert(__builtin_offsetof(struct kos_seg_info, state) == 3,
	       "seg info state follows the kind");
_Static_assert(__builtin_offsetof(struct kos_seg_info, handle) == 4,
	       "seg info handle follows the state");
_Static_assert(__builtin_offsetof(struct kos_seg_info_args, buffer) == 2,
	       "seg info buffer is the second word");
#endif

#endif /* KOS_SEG_H */
