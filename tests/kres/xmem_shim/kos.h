/* KRES host tests: the XMEM shim (stands in for the SDK's kos.h when
 * the KonaOS catalog store, kres_cache_xmem.c, is compiled for the
 * host).  Objects obey the native rules that matter to the store: at
 * most 32 KiB each, at most 32 live, contents undefined at allocation,
 * stale handles refused. */

#ifndef KRES_XMEM_SHIM_KOS_H
#define KRES_XMEM_SHIM_KOS_H

#include <stdint.h>

#include "kos/errno.h"

#define KOS_XMEM_FLAG_NONE 0u

int xalloc(unsigned int size, unsigned int flags);
int xfree(unsigned int handle);
int xread(unsigned int handle, unsigned int offset, void *buffer,
	  unsigned int length);
int xwrite(unsigned int handle, unsigned int offset, const void *buffer,
	   unsigned int length);

/* The shim's accounting: live objects, live bytes, the peak bytes, and
 * a cap on the total (0: none). */
unsigned int xmem_shim_objects(void);
unsigned long xmem_shim_bytes(void);
unsigned long xmem_shim_peak(void);
void xmem_shim_limit(unsigned long bytes);

#endif /* KRES_XMEM_SHIM_KOS_H */
