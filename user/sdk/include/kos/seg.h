/* KonaOS SDK: EXE v2 segments (packages 44a/44b).
 *
 * The native calls are abi/kos/seg.h (SYS_SEG_INFO, SYS_SEG_MAP).  A EXE
 * v2 `DATA` segment is a loader-created extended-memory object owned by
 * the program: `seginfo` returns its logical size, kind, load state
 * and the handle that abi/kos/xmem.h's read/write calls use.  A loader
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
 *     extern int far_main(int a, int b);   (built with FAR1=, linked
 *                                           at 8000h as segment 1)
 *     result = farcall(1u, far_main, a, b);
 *
 * `__banked` is the same mechanism as plain C (package 49b).  A program
 * built with `FAR1=`..`FAR4=` compiles each far source with bank n and
 * the `_FAR<n>` area, so every `__banked` function it defines emits
 * `b_<fn> = <n>`; SDCC then turns a call to it into
 *
 *     ld  e, #b_<fn>
 *     ld  hl, #_<fn>
 *     call __sdcc_bcall_ehl
 *
 * with every argument on the stack.  The SDK's trampoline (bcall.asm) is
 * the other half: it maps segment E, opens the aperture onto it, transfers
 * to HL and restores the caller's latch, so the call needs no `farcall`:
 *
 *     extern int far_main(int a, int b) __banked;   (built with FAR1=)
 *     result = far_main(a, b);
 *
 * The trampoline caches one token per segment id, so only the first call
 * of a segment spends a SYS_SEG_MAP.  A banked call that names no far
 * segment - an id the container does not have, or a `__banked` definition
 * in the primary bank (`b_` = 0) - reports KOS_RT_EFARMAP through
 * `exe_runtime_error` and returns without running the function; a banked
 * function in a far segment may itself make banked calls, including into
 * its own or another segment.
 *
 * The far call is a plain `CALL` to the entry address the linker resolved
 * - one program links the primary and the far area together - and the far
 * code may call primary code outside the aperture directly and issue its
 * own syscalls: the gate restores the aperture latch exactly, and a
 * syscall buffer in `8000h-BFFFh` names the far bank (the buffer-identity
 * rule).  A token exists only for the calling process's own FAR_CODE
 * segments.
 */

#ifndef KOS_SDK_SEG_H
#define KOS_SDK_SEG_H

#include "kos/types.h"
#include "kos/seg.h"

/* Describe container segment `segment` (1..count) into `out`.  Returns 0
 * or -errno (KOS_EINVAL for an id outside the caller's own container or
 * a bad record pointer). */
int seginfo(unsigned int segment, struct kos_seg_info *out);

/* The aperture token of `segment`, a FAR_CODE segment of the caller's own
 * container (>= 1); the segment is always at `8000h`.  Returns -errno
 * (KOS_EINVAL for an unknown id, a DATA segment's id or a bad id). */
int segmap(unsigned int segment);

/* The runtime code a banked call reports when the segment it names
 * cannot be mapped: an id that is not one of the program's `FAR_CODE`
 * segments (ids 1-4) or a refused SYS_SEG_MAP.  It is an SDK-level code
 * (the syscall-visible KOS_E* range is abi/kos/errno.h) and travels the
 * shared carrying-error path, so a banked function reads it as
 * -KOS_RT_EFARMAP (8-bit callers see the low byte). */
#define KOS_RT_EFARMAP 0x30

/* A far function: the entry the linker placed at an address >= 8000h. */
typedef int (*kos_far_fn)(int, int);

/* Map `segment` and call `entry(a, b)` through the aperture, then restore
 * the previous latch exactly.  Returns the far call's result, or -errno
 * when the segment cannot be mapped. */
int farcall(unsigned int segment, kos_far_fn entry, int a, int b);

#endif /* KOS_SDK_SEG_H */
