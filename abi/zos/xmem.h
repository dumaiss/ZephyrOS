/* ZephyrOS native extended-memory ABI (public native ABI, package 43).
 *
 * Architecture 36.15-36.17: a process allocates process-owned
 * extended-memory objects and reaches their contents three ways: through
 * `read`/`write` copies, through the mode-10 far aperture (mappable
 * objects only), or by never mapping anything and keeping its whole
 * primary bank.
 *
 *   SYS_XMEM_ALLOC  reserve an object, returns an opaque handle
 *   SYS_XMEM_FREE   release it
 *   SYS_XMEM_READ   copy object -> caller buffer
 *   SYS_XMEM_WRITE  copy caller buffer -> object
 *   SYS_XMEM_INFO   logical description (size, flags, type)
 *   SYS_XMEM_MAP    aperture token + address for a mappable object
 *
 * An object is read/write, non-executable and owned by exactly one
 * process; a child inherits none of its parent's objects and exit releases
 * every object after the process's handles are unbound.  The size is
 * rounded up to whole 8 KiB slots and an object is contiguous inside one
 * bank; **placement is never visible through the API** (the bank, the slot
 * and the physical layout are Zephyr-80 implementation details).  The
 * largest object is 32 KiB; a `MAPPABLE` object is at most 16 KiB and
 * lands in the aperture slice.
 *
 * A ZEX v2 `DATA` segment is one of these objects with type
 * `ZOS_XMEM_TYPE_SEGMENT` (SYS_SEG_INFO names it); it is read and written
 * with this family's calls and released only at exit.
 *
 * **Contents are undefined.**  Nothing pre-zeroes an object (architecture
 * 36.19): a program that needs zeroes writes them.  Every range
 * (`offset + length` against the allocated size, the caller buffer against
 * its window) is checked before any byte moves; invalid, stale or foreign
 * handles are refused.
 *
 * The aperture (36.17): in mode 10 with a nonzero page field P, the
 * caller's `8000h-BFFFh` names bank 7 - P.  `SYS_XMEM_MAP` returns the
 * **token** - the execution-latch value that opens the aperture onto the
 * object - and the object's address in the aperture (`8000h` for an object
 * in the lower aperture slot, `A000h` in the upper).  The SDK opens the
 * aperture by writing the token to the memory-control port and closes it
 * by restoring the previous latch (user/sdk).  Tokens exist only for the
 * calling process's own objects, and every object bank maps to a page
 * field of 2..7, so no token ever exposes bank 6 or bank 7.
 *
 * Buffer identity (36.8): a caller address in `8000h-BFFFh` while the
 * caller's saved latch has a nonzero page field names the far bank, not
 * the primary bank - in `read`/`write` copies and in every other syscall
 * that moves caller bytes.  A buffer that would cross the aperture
 * boundary is refused.
 *
 * Argument blocks live in the caller window `0100h-1FFFh`; `read`/`write`
 * buffers and the info record must lie in the caller buffer window
 * `0100h-DFFFh` and are moved only through the checked copy gates.
 */

#ifndef ZOS_XMEM_H
#define ZOS_XMEM_H

#include "zos/types.h"
#include "zos/errno.h"

/* --- native syscall numbers (docs/abi.md section 14) -------------------- */

#define ZOS_SYSCALL_XMEM_ALLOC 0x20u
#define ZOS_SYSCALL_XMEM_FREE  0x21u
#define ZOS_SYSCALL_XMEM_READ  0x22u
#define ZOS_SYSCALL_XMEM_WRITE 0x23u
#define ZOS_SYSCALL_XMEM_INFO  0x24u
#define ZOS_SYSCALL_XMEM_MAP   0x25u

/* --- allocation flags --------------------------------------------------- */

/* Place the object in the aperture slice so `SYS_XMEM_MAP` can open it
 * (at most 16 KiB).  Unknown flag bits are ZOS_EINVAL. */
#define ZOS_XMEM_FLAG_MAPPABLE 0x0001u
#define ZOS_XMEM_FLAG_NONE     0x0000u

/* --- geometry (the slot size is documented; placement is not) ----------- */

/* One auxiliary slot; an allocation rounds up to whole slots. */
#define ZOS_XMEM_SLOT_SIZE     0x2000u
/* The mode-10 far aperture: `ZOS_XMEM_APERTURE_SIZE` bytes at
 * `ZOS_XMEM_APERTURE_BASE` while P != 0. */
#define ZOS_XMEM_APERTURE_BASE 0x8000u
#define ZOS_XMEM_APERTURE_SIZE 0x4000u
/* The largest mappable object (the aperture slice) and the largest object
 * of any kind (the four non-aperture data slots of one bank). */
#define ZOS_XMEM_MAX_MAPPABLE  0x4000u
#define ZOS_XMEM_MAX_OBJECT    0x8000u

/* SYS_XMEM_ALLOC 20h {size:u16, flags:u16} -> HL = handle (>= 1).
 * A zero size, an unknown flag or a mappable size over 16 KiB is
 * ZOS_EINVAL; a size over the largest object, an exhausted object table or
 * a free region without a fitting run is ZOS_ENOSPC.  Contents are
 * undefined. */
struct zos_xmem_alloc_args {
	uint16_t size;
	uint16_t flags;
};

/* SYS_XMEM_FREE 21h {handle:u16} -> HL = 0.  A handle that is not live,
 * not this process's, or stale (a reused entry) is ZOS_EINVAL, and so is
 * a loader-created `ZOS_XMEM_TYPE_SEGMENT` object: a ZEX data segment is
 * released only at exit (architecture 36.16). */
struct zos_xmem_free_args {
	uint16_t handle;
};

/* SYS_XMEM_READ 22h / SYS_XMEM_WRITE 23h
 * {handle:u16, offset:u16, buffer:u16, length:u16} -> HL = bytes moved.
 * The transfer is between the object and the caller's buffer at `buffer`
 * (its bank follows the buffer-identity rule above).  A bad block or
 * handle, an offset/length outside the object or a buffer outside the
 * caller window is ZOS_EINVAL; a copy-gate failure is ZOS_EFAULT. */
struct zos_xmem_io_args {
	uint16_t handle;
	uint16_t offset;
	uint16_t buffer;
	uint16_t length;
};

/* SYS_XMEM_INFO 24h {handle:u16, buffer:u16} -> the 8-byte record below at
 * `buffer`, HL = 0.  Never reports a bank or slot. */
struct zos_xmem_info {
	uint16_t requested; /* bytes asked for at allocation */
	uint16_t allocated; /* bytes reserved (whole slots)  */
	uint16_t flags;     /* ZOS_XMEM_FLAG_*               */
	uint8_t  type;      /* ZOS_XMEM_TYPE_*               */
	uint8_t  reserved;
};

#define ZOS_XMEM_TYPE_RUNTIME 1u /* SYS_XMEM_ALLOC object           */
#define ZOS_XMEM_TYPE_SEGMENT 2u /* loader-created ZEX data segment */

struct zos_xmem_info_args {
	uint16_t handle;
	uint16_t buffer;
};

/* SYS_XMEM_MAP 25h {handle:u16, address:u16 out}
 *   -> HL = the map token (the latch value that opens the aperture onto
 *      this object) and `address` = the object's aperture address.
 * Only the caller's own mappable objects can be mapped: any other handle,
 * a non-mappable object or a bad block is ZOS_EINVAL. */
struct zos_xmem_map_args {
	uint16_t handle;
	uint16_t address; /* out */
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
_Static_assert(sizeof(struct zos_xmem_map_args) == 4,
	       "xmem map block must be 4 packed bytes");
_Static_assert(__builtin_offsetof(struct zos_xmem_io_args, offset) == 2,
	       "xmem io offset follows the handle");
_Static_assert(__builtin_offsetof(struct zos_xmem_io_args, buffer) == 4,
	       "xmem io buffer follows the offset");
_Static_assert(__builtin_offsetof(struct zos_xmem_io_args, length) == 6,
	       "xmem io length is the last word");
_Static_assert(__builtin_offsetof(struct zos_xmem_map_args, address) == 2,
	       "xmem map address is the second word");
#endif

#endif /* ZOS_XMEM_H */
