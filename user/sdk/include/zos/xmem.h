/* ZephyrOS SDK: extended-memory objects and the far aperture (package 43).
 *
 * The native family is abi/zos/xmem.h (SYS_XMEM_ALLOC / _FREE / _READ /
 * _WRITE / _INFO / _MAP).  These wrappers return a non-negative result or
 * the negated native error code, exactly like the other libzos clients.
 *
 * A mappable object can be opened through the mode-10 aperture:
 * `zos_xmem_map` returns the token and the object's address, and
 * `zos_aperture_open` writes the token to the memory-control port
 * (saving the previous latch), `zos_aperture_close` restores it.  While
 * the aperture is open, plain pointer accesses in 8000h-BFFFh reach the
 * object - and every syscall buffer in that range belongs to the far bank
 * too (the buffer-identity rule), so a path or read/write buffer inside
 * the aperture names the object, not the program's own memory.
 *
 * Ordinary programs that never map anything keep their whole 56 KiB and
 * need none of this.
 */

#ifndef ZOS_SDK_XMEM_H
#define ZOS_SDK_XMEM_H

#include "zos/types.h"
#include "zos/xmem.h"

/* Reserve `size` bytes (ZOS_XMEM_FLAG_*).  Returns the object handle
 * (>= 1) or -errno (ZOS_EINVAL for a bad size or flag, ZOS_ENOSPC when no
 * bank has room).  Contents are undefined. */
int zos_xmem_alloc(unsigned int size, unsigned int flags);

/* Release an object.  Returns 0 or -ZOS_EINVAL (stale, foreign or
 * unknown handle). */
int zos_xmem_free(unsigned int handle);

/* Copy `length` bytes between the object at `offset` and the program's
 * `buffer` (its bank follows the buffer-identity rule).  Returns the
 * bytes moved or -errno. */
int zos_xmem_read(unsigned int handle, unsigned int offset, void *buffer,
		  unsigned int length);
int zos_xmem_write(unsigned int handle, unsigned int offset,
		   const void *buffer, unsigned int length);

/* The object's logical description into `out`.  Returns 0 or -errno. */
int zos_xmem_info(unsigned int handle, struct zos_xmem_info *out);

/* Map a mappable object: returns the aperture token or -errno, and
 * `*address` receives the object's address in the aperture. */
int zos_xmem_map(unsigned int handle, unsigned int *address);

/* Open the aperture onto a mapped object: writes `token` and returns the
 * previous latch.  Close restores that latch exactly. */
unsigned char zos_aperture_open(unsigned char token);
void zos_aperture_close(unsigned char previous);

#endif /* ZOS_SDK_XMEM_H */
