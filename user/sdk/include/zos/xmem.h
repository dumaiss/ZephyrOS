/* ZephyrOS SDK: extended-memory and far-call wrappers (M10, package 33).
 *
 * C-facing wrappers over the pinned extended-memory family (20h-26h,
 * architecture section 36.7).  Return convention matches zos/console.h:
 * a non-negative result, or the negated native error code.  Argument
 * blocks are built in the SDK's shared low-window scratch.
 */

#ifndef ZOS_SDK_XMEM_H
#define ZOS_SDK_XMEM_H

#include "zos/types.h"
#include "zos/xmem.h"

/* One request for the shared gate stub (low-level callers only). */
struct zos_xmem_call_req {
	unsigned char number;
	void         *block;
};

int zos_xmem_call(struct zos_xmem_call_req *req);

/* Allocate a zero-initialized runtime object; returns an opaque handle
 * (> 0) or -errno.  A zero size or an oversized request is -ZOS_EINVAL /
 * -ZOS_ENOSPC. */
int zos_xmem_alloc(unsigned int size);
int zos_xmem_free(unsigned int handle);

/* Transfer between the calling execution bank and the object.  The whole
 * range is checked before any byte moves. */
int zos_xmem_read(unsigned int handle, unsigned int offset, void *buffer,
		  unsigned int length);
int zos_xmem_write(unsigned int handle, unsigned int offset,
		   const void *buffer, unsigned int length);

/* Fill the logical description; returns 8 or a negative error. */
int zos_xmem_info(unsigned int handle, struct zos_xmem_info *out);

/* Loader-created segment description by ZEX-local id; 8 or -errno. */
int zos_zex_segment_info(unsigned char segment_id,
			 struct zos_zex_seg_info *out);

/* Synchronous far call: copies at most ZOS_FAR_FRAME_MAX bytes in and
 * out and returns the far entry's result, or -errno (recursive calls
 * return -ZOS_EBUSY). */
int zos_far_call(unsigned char entry, const void *in, unsigned int in_len,
		 void *out, unsigned int out_len);

#endif /* ZOS_SDK_XMEM_H */
