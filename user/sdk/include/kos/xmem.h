/* KonaOS SDK: extended-memory objects and the far aperture.
 *
 * The native family is the ABI's kos/xmem.h (SYS_XMEM_ALLOC / _FREE / _READ /
 * _WRITE / _INFO / _MAP).  These wrappers return a non-negative result or
 * the negated native error code, exactly like the other libkos clients.
 *
 * A mappable object can be opened through the far aperture (8000h-BFFFh):
 * `xmap` returns the token and the object's address, and
 * `aperture_open` opens the aperture onto the token and returns the
 * token in force before, which `aperture_close` restores.  The token is
 * the board's - the latch value on Zephyr-80, a one-byte mapping handle
 * on Zephyr-80 Mk2 - and the SDK the program is built with opens it the
 * board's way (docs/sdk.md).  While the aperture is open, plain pointer
 * accesses in 8000h-BFFFh reach the object - and every syscall buffer in
 * that range belongs to the object too (the buffer-identity rule), so a
 * path or read/write buffer inside the aperture names the object, not
 * the program's own memory.
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

/* Open the aperture onto a mapped object: returns the token in force
 * before.  Close restores that mapping exactly.  Both keep DE and HL. */
unsigned char aperture_open(unsigned char token);
void aperture_close(unsigned char previous);

#endif /* KOS_SDK_XMEM_H */
