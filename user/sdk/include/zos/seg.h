/* ZephyrOS SDK: ZEX v2 segments (packages 44a/44b).
 *
 * The native calls are abi/zos/seg.h (SYS_SEG_INFO, SYS_SEG_MAP).  A ZEX
 * v2 `DATA` segment is a loader-created extended-memory object owned by
 * the program: `seginfo` returns its logical size, kind, load state
 * and the handle that abi/zos/xmem.h's read/write calls use.  A loader
 * segment is released only at exit, so `xfree` refuses it.
 *
 * A `FAR_CODE` segment is linked at `8000h`, placed in the aperture slice
 * of its own auxiliary bank (one far-code segment per bank) and executed
 * in place: the program's primary image, its BSS and its stack stay
 * outside `8000h-BFFFh` (the build enforces the link rule), so when the
 * mode-10 aperture is open onto the segment, an address in `8000h-BFFFh`
 * names the far bank.  `segmap(segment)` returns the aperture token,
 * and `farcall` maps the segment, makes the far call, and restores
 * the previous latch:
 *
 *     extern int far_main(int a, int b);   (built with --codeseg
 *                                           FAR_CODE, linked at 8000h)
 *     result = farcall(1u, far_main, a, b);
 *
 * The far call is a plain `CALL` to the entry address the linker resolved
 * - one program links the primary and the far area together - and the far
 * code may call primary code outside the aperture directly and issue its
 * own syscalls: the gate restores the aperture latch exactly, and a
 * syscall buffer in `8000h-BFFFh` names the far bank (the buffer-identity
 * rule).  A token exists only for the calling process's own FAR_CODE
 * segments.
 */

#ifndef ZOS_SDK_SEG_H
#define ZOS_SDK_SEG_H

#include "zos/types.h"
#include "zos/seg.h"

/* Describe container segment `segment` (1..count) into `out`.  Returns 0
 * or -errno (ZOS_EINVAL for an id outside the caller's own container or
 * a bad record pointer). */
int seginfo(unsigned int segment, struct zos_seg_info *out);

/* The aperture token of `segment`, a FAR_CODE segment of the caller's own
 * container (>= 1); the segment is always at `8000h`.  Returns -errno
 * (ZOS_EINVAL for an unknown id, a DATA segment's id or a bad id). */
int segmap(unsigned int segment);

/* A far function: the entry the linker placed at an address >= 8000h. */
typedef int (*zos_far_fn)(int, int);

/* Map `segment` and call `entry(a, b)` through the aperture, then restore
 * the previous latch exactly.  Returns the far call's result, or -errno
 * when the segment cannot be mapped. */
int farcall(unsigned int segment, zos_far_fn entry, int a, int b);

#endif /* ZOS_SDK_SEG_H */
