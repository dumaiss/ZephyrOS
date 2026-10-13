/* KonaOS SDK: EXE v2 segments.
 *
 * The native calls are the ABI's kos/seg.h (SYS_SEG_INFO, SYS_SEG_MAP).  A EXE
 * v2 `DATA` segment is a loader-created extended-memory object owned by
 * the program: `seginfo` returns its logical size, kind, load state
 * and the handle that the ABI's kos/xmem.h read/write calls use.  A loader
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
 * `__banked` is the same mechanism as plain C.  A program
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
 * (the syscall-visible KOS_E* range is the ABI's kos/errno.h) and travels the
 * shared carrying-error path, so a banked function reads it as
 * -KOS_RT_EFARMAP (8-bit callers see the low byte). */
#define KOS_RT_EFARMAP 0x30

/* A far function: the entry the linker placed at an address >= 8000h. */
typedef int (*kos_far_fn)(int, int);

/* Map `segment` and call `entry(a, b)` through the aperture, then restore
 * the previous latch exactly.  Returns the far call's result, or -errno
 * when the segment cannot be mapped. */
int farcall(unsigned int segment, kos_far_fn entry, int a, int b);

/* --- far data ----------------------------------------
 *
 * A program writes far data in C: the sources of one `DATAn=` option are
 * compiled into the areas `_DATA<n>` (its `const` objects) and `_DATA<n>B`
 * (its zeroed objects), the link bases the pair at `8000h` - image first,
 * then BSS - and the container carries it as one mappable `DATA` segment
 * (row flag `EXE_SEG_FLAG_MAPPABLE`).  The loader places such a segment at
 * `8000h` of an auxiliary bank, exactly where the link put it, and zeroes
 * its BSS; a program reaches it two ways:
 *
 *   - `farmap(n, &saved)` opens the aperture onto the n-th `DATAn`
 *     segment and returns `8000h`, so the C symbols of its sources are
 *     plain pointers for as long as it stays open; `farunmap(saved)`
 *     restores the caller's mapping.  One far window (7.2) serves both,
 *     so a mapped data segment and a `__banked` call cannot be open at the
 *     same time: copy out of the table first, then make the call;
 *   - `xread`/`xwrite` copy through the segment's handle like any other
 *     object.
 *
 * `blob(n)` is the handle of the n-th `BLOBS` file, a plain DATA segment
 * that carries a payload file rather than C objects: read it with `xread`
 * (there is no address to map, and no `const` in the program's image).
 *
 * Both ordinals are 1-based and count the container's DATA rows of their
 * kind in id order, which is what `DATAn=` (ascending from 1) and
 * `BLOBS=` (in the order given) generate.
 *
 * A file that only arrives at run time is not bundled: `xload` reads it
 * into a fresh object and hands back its handle.  It is limited to 32 KiB,
 * or 16 KiB when it asks for `KOS_XMEM_FLAG_MAPPABLE`; a larger file is
 * `-KOS_ENOSPC`, and a program that must have one streams it with
 * `open`/`read`/`xwrite` instead of loading it whole.  A mappable object
 * is filled by reading the file straight into the open window (the
 * buffer-identity rule, 4.9), a plain one through an SDK-internal
 * 512-byte buffer.
 */

/* Open the aperture onto the n-th `DATAn` segment (1-based) and return its
 * address, `8000h`, or NULL.  On success `*prev` holds the latch
 * `farunmap` restores; on failure it holds the negated error code as a
 * signed byte (`*(signed char *)prev`), the same 8-bit convention the
 * runtime codes use.  A `DATAn` source linked by the SDK's rule is at most
 * 16 KiB; the aperture shows the whole segment. */
void *farmap(unsigned int n, unsigned char *prev);

/* Restore the mapping `farmap` saved: pass what it wrote to `*prev`, and
 * only on the success path. */
void farunmap(unsigned char prev);

/* The handle of the n-th `BLOBS` file (1-based), for `xread`/`xwrite`
 * with offsets, or -errno (KOS_EINVAL for no such file). */
int blob(unsigned int n);

/* Load the whole file at `path` into a new object and return its handle,
 * or -errno: KOS_EINVAL for a bad flag or an empty/unreadable file,
 * KOS_ENOSPC when the file is over the limit above or no bank has room.
 * `flags` is KOS_XMEM_FLAG_MAPPABLE for an object that can be mapped, or
 * KOS_XMEM_FLAG_NONE.  Nothing is left behind on failure. */
int xload(const char *path, unsigned int flags);

#endif /* KOS_SDK_SEG_H */
