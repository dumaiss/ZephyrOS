/* KonaOS SDK: extended-memory objects and the far aperture.
 *
 * The native family is the ABI's kos/xmem.h (SYS_XMEM_ALLOC / _FREE / _READ /
 * _WRITE / _INFO / _MAP).  These wrappers return a non-negative result or
 * the negated native error code, exactly like the other libkos clients.
 *
 * A mappable object can be opened through the mode-10 aperture:
 * `xmap` returns the token and the object's address, and
 * `aperture_open` writes the token to the memory-control port
 * (saving the previous latch), `aperture_close` restores it.  While
 * the aperture is open, plain pointer accesses in 8000h-BFFFh reach the
 * object - and every syscall buffer in that range belongs to the far bank
 * too (the buffer-identity rule), so a path or read/write buffer inside
 * the aperture names the object, not the program's own memory.
 *
 * Ordinary programs that never map anything keep their whole 56 KiB and
 * need none of this.
 */

#ifndef KOS_SDK_XMEM_H
#define KOS_SDK_XMEM_H

#include "kos/types.h"
#include "kos/xmem.h"

/* Reserve `size` bytes (KOS_XMEM_FLAG_*).  Returns the object handle
 * (>= 1) or -errno (KOS_EINVAL for a bad size or flag, KOS_ENOSPC when no
 * bank has room).  Contents are undefined. */
int xalloc(unsigned int size, unsigned int flags);

/* Release an object.  Returns 0 or -KOS_EINVAL (stale, foreign or
 * unknown handle). */
int xfree(unsigned int handle);

/* Copy `length` bytes between the object at `offset` and the program's
 * `buffer` (its bank follows the buffer-identity rule).  Returns the
 * bytes moved or -errno. */
int xread(unsigned int handle, unsigned int offset, void *buffer,
		  unsigned int length);
int xwrite(unsigned int handle, unsigned int offset,
		   const void *buffer, unsigned int length);

/* The object's logical description into `out`.  Returns 0 or -errno. */
int xinfo(unsigned int handle, struct kos_xmem_info *out);

/* Map a mappable object: returns the aperture token or -errno, and
 * `*address` receives the object's address in the aperture. */
int xmap(unsigned int handle, unsigned int *address);

/* Open the aperture onto a mapped object: writes `token` and returns the
 * previous latch.  Close restores that latch exactly. */
unsigned char aperture_open(unsigned char token);
void aperture_close(unsigned char previous);

#endif /* KOS_SDK_XMEM_H */
