/* KonaOS native extended-memory ABI (public native ABI).
 *
 * Architecture 4.8-7.2: a process allocates process-owned
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
 * A EXE v2 `DATA` segment is one of these objects with type
 * `KOS_XMEM_TYPE_SEGMENT` (SYS_SEG_INFO names it); it is read and written
 * with this family's calls and released only at exit.  A DATA segment
 * whose container row carried `EXE_SEG_FLAG_MAPPABLE`
 * is mappable: its image was linked at `8000h` and the loader places it
 * in the aperture slice, so a program opens the aperture onto it with
 * `SYS_XMEM_MAP` and uses the C symbols directly.
 *
 * **Contents are undefined.**  Nothing pre-zeroes an object (architecture
 * 4.8): a program that needs zeroes writes them.  Every range
 * (`offset + length` against the allocated size, the caller buffer against
 * its window) is checked before any byte moves; invalid, stale or foreign
 * handles are refused.
 *
 * The aperture (7.2): in mode 10 with a nonzero page field P, the
 * caller's `8000h-BFFFh` names bank 7 - P.  `SYS_XMEM_MAP` returns the
 * **token** - the execution-latch value that opens the aperture onto the
 * object - and the object's address in the aperture (`8000h` for an object
 * in the lower aperture slot, `A000h` in the upper).  The SDK opens the
 * aperture by writing the token to the memory-control port and closes it
 * by restoring the previous latch (the SDK's aperture wrapper).  Tokens
 * exist only for the
 * calling process's own objects, and every object bank maps to a page
 * field of 2..7, so no token ever exposes bank 6 or bank 7.
 *
 * Buffer identity (4.9): a caller address in `8000h-BFFFh` while the
 * caller's saved latch has a nonzero page field names the far bank, not
 * the primary bank - in `read`/`write` copies and in every other syscall
 * that moves caller bytes.  A buffer that would cross the aperture
 * boundary is refused.
 *
 * Argument blocks live in the caller window `0100h-1FFFh`; `read`/`write`
 * buffers and the info record must lie in the caller buffer window
 * `0100h-DFFFh` and are moved only through the checked copy gates.
 *
 * The text above is the Zephyr-80 Mk1 board's.  On the Mk2 board
 * (`KOS_BOARD_MK2`, docs/abi.md section 8.3) the family keeps its
 * numbers, blocks and errors with these differences:
 *
 * - An object is any whole number of 8 KiB units up to
 *   `KOS_XMEM_MAX_OBJECT` (56 KiB); a `MAPPABLE` object is at most the
 *   16 KiB far window.  There are no slots and no placement classes; a
 *   loader segment is an object of its own pages.
 * - `SYS_XMEM_MAP` (and `SYS_SEG_MAP`) returns a **mapping handle**, not
 *   a latch value, and `address` is always `KOS_XMEM_APERTURE_BASE`.  The
 *   handle is opaque, nonzero and one byte; the far-window vector every
 *   program page zero carries (`RST 28h`, docs/abi.md section 8.3) opens
 *   the window onto it and returns the handle in force before, and
 *   handle 0 is the program's own memory, so passing the returned handle
 *   back closes the window.  A freed object's handle is refused there.
 * - Buffer identity: a caller address is resolved through the caller's
 *   current mapping, far window included, so a buffer may also straddle
 *   the window's edges.
 */

#ifndef KOS_XMEM_H
#define KOS_XMEM_H

#include "kos/types.h"
#include "kos/errno.h"

/* --- native syscall numbers (docs/abi.md section 8.1) -------------------- */

#define KOS_SYSCALL_XMEM_ALLOC 0x20u
#define KOS_SYSCALL_XMEM_FREE  0x21u
#define KOS_SYSCALL_XMEM_READ  0x22u
#define KOS_SYSCALL_XMEM_WRITE 0x23u
#define KOS_SYSCALL_XMEM_INFO  0x24u
#define KOS_SYSCALL_XMEM_MAP   0x25u

/* --- allocation flags --------------------------------------------------- */

/* Place the object in the aperture slice so `SYS_XMEM_MAP` can open it
 * (at most 16 KiB).  Unknown flag bits are KOS_EINVAL. */
#define KOS_XMEM_FLAG_MAPPABLE 0x0001u
#define KOS_XMEM_FLAG_NONE     0x0000u

/* --- geometry (the slot size is documented; placement is not) ----------- */

/* One auxiliary slot; an allocation rounds up to whole slots. */
#define KOS_XMEM_SLOT_SIZE     0x2000u
/* The mode-10 far aperture: `KOS_XMEM_APERTURE_SIZE` bytes at
 * `KOS_XMEM_APERTURE_BASE` while P != 0. */
#define KOS_XMEM_APERTURE_BASE 0x8000u
#define KOS_XMEM_APERTURE_SIZE 0x4000u
/* The largest mappable object (the aperture slice) and the largest object
 * of any kind (Mk1: the four non-aperture data slots of one bank; Mk2:
 * seven 8 KiB units). */
#define KOS_XMEM_MAX_MAPPABLE  0x4000u
#if defined(KOS_BOARD_MK2)
#define KOS_XMEM_MAX_OBJECT    0xE000u
#else
#define KOS_XMEM_MAX_OBJECT    0x8000u
#endif

/* SYS_XMEM_ALLOC 20h {size:u16, flags:u16} -> HL = handle (>= 1).
 * A zero size, an unknown flag or a mappable size over 16 KiB is
 * KOS_EINVAL; a size over the largest object, an exhausted object table or
 * a free region without a fitting run is KOS_ENOSPC.  Contents are
 * undefined. */
struct kos_xmem_alloc_args {
	uint16_t size;
	uint16_t flags;
};

/* SYS_XMEM_FREE 21h {handle:u16} -> HL = 0.  A handle that is not live,
 * not this process's, or stale (a reused entry) is KOS_EINVAL, and so is
 * a loader-created `KOS_XMEM_TYPE_SEGMENT` object: a EXE data segment is
 * released only at exit (architecture 4.8). */
struct kos_xmem_free_args {
	uint16_t handle;
};

/* SYS_XMEM_READ 22h / SYS_XMEM_WRITE 23h
 * {handle:u16, offset:u16, buffer:u16, length:u16} -> HL = bytes moved.
 * The transfer is between the object and the caller's buffer at `buffer`
 * (its bank follows the buffer-identity rule above).  A bad block or
 * handle, an offset/length outside the object or a buffer outside the
 * caller window is KOS_EINVAL; a copy-gate failure is KOS_EFAULT. */
struct kos_xmem_io_args {
	uint16_t handle;
	uint16_t offset;
	uint16_t buffer;
	uint16_t length;
};

/* SYS_XMEM_INFO 24h {handle:u16, buffer:u16} -> the 8-byte record below at
 * `buffer`, HL = 0.  Never reports a bank or slot. */
struct kos_xmem_info {
	uint16_t requested; /* bytes asked for at allocation */
	uint16_t allocated; /* bytes reserved (whole slots)  */
	uint16_t flags;     /* KOS_XMEM_FLAG_*               */
	uint8_t  type;      /* KOS_XMEM_TYPE_*               */
	uint8_t  reserved;
};

#define KOS_XMEM_TYPE_RUNTIME 1u /* SYS_XMEM_ALLOC object           */
#define KOS_XMEM_TYPE_SEGMENT 2u /* loader-created EXE data segment */

struct kos_xmem_info_args {
	uint16_t handle;
	uint16_t buffer;
};

/* SYS_XMEM_MAP 25h {handle:u16, address:u16 out}
 *   -> HL = the map token (the latch value that opens the aperture onto
 *      this object; on Mk2 the mapping handle the far-window operation
 *      opens) and `address` = the object's aperture address.
 * Only the caller's own mappable objects can be mapped: any other handle,
 * a non-mappable object or a bad block is KOS_EINVAL. */
struct kos_xmem_map_args {
	uint16_t handle;
	uint16_t address; /* out */
};

/* SYS_PERSONALITY 0Fh {list:u16, count:u8, reserved:u8, entry:u16} - the
 * Mk2 board's one-way handoff to another machine personality (docs/abi.md
 * section 8.4); it does not return.  `list` addresses `count` page
 * records in the caller buffer window; record n names the object page
 * that becomes logical page n (n * 2000h) of the personality's 64 KiB space, so
 * `count` is KOS_PERSONALITY_PAGES.  Every record is checked before
 * anything changes: a bad block, a wrong count, a nonzero reserved byte,
 * a handle that is not one of the caller's live objects (freed, stale,
 * foreign), a page past the object's end, or an entry whose two bytes
 * below it hold the nucleus's switch code is KOS_EINVAL and KonaOS stays
 * intact.  Then the nucleus stops its interrupt and NMI sources, maps
 * the eight pages as the whole working mapping and jumps to `entry` with
 * interrupts masked and IM 1; leaving takes a reset.  No physical page
 * number crosses the ABI.  On the Mk1 board the number is unassigned
 * (KOS_ENOSYS). */
#define KOS_SYSCALL_PERSONALITY 0x0Fu
#define KOS_PERSONALITY_PAGES   8u

struct kos_personality_page {
	uint16_t handle;   /* the caller's object                  */
	uint8_t  page;     /* its 8 KiB page: object offset >> 13  */
	uint8_t  reserved; /* 0                                    */
};

struct kos_personality_args {
	uint16_t list;     /* count kos_personality_page records */
	uint8_t  count;    /* KOS_PERSONALITY_PAGES              */
	uint8_t  reserved; /* 0                                  */
	uint16_t entry;    /* the first instruction              */
};

#if defined(__SDCC)
_Static_assert(sizeof(struct kos_personality_page) == 4,
	       "personality page record must be 4 packed bytes");
_Static_assert(sizeof(struct kos_personality_args) == 6,
	       "personality block must be 6 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_personality_args, entry) == 4,
	       "personality entry is the last word");
_Static_assert(sizeof(struct kos_xmem_alloc_args) == 4,
	       "xmem alloc block must be 4 packed bytes");
_Static_assert(sizeof(struct kos_xmem_free_args) == 2,
	       "xmem free block must be 2 packed bytes");
_Static_assert(sizeof(struct kos_xmem_io_args) == 8,
	       "xmem io block must be 8 packed bytes");
_Static_assert(sizeof(struct kos_xmem_info) == 8,
	       "xmem info record must be 8 packed bytes");
_Static_assert(sizeof(struct kos_xmem_info_args) == 4,
	       "xmem info block must be 4 packed bytes");
_Static_assert(sizeof(struct kos_xmem_map_args) == 4,
	       "xmem map block must be 4 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_xmem_io_args, offset) == 2,
	       "xmem io offset follows the handle");
_Static_assert(__builtin_offsetof(struct kos_xmem_io_args, buffer) == 4,
	       "xmem io buffer follows the offset");
_Static_assert(__builtin_offsetof(struct kos_xmem_io_args, length) == 6,
	       "xmem io length is the last word");
_Static_assert(__builtin_offsetof(struct kos_xmem_map_args, address) == 2,
	       "xmem map address is the second word");
#endif

#endif /* KOS_XMEM_H */
