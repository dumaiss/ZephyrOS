/* ZephyrOS libzos: extended-memory and far-call wrappers (M10 package 33).
 *
 * Owns the shared low-window argument block (zos_scratch) and builds
 * every 20h-26h block before calling the single gate stub
 * (xmemgate.asm).  The kernel owns all window, range and handle
 * validation.
 */

#include "zos.h"
#include "zos_internal.h"

static int zxm_call(unsigned char number, void *block)
{
	struct zos_xmem_call_req req;

	req.number = number;
	req.block = block;
	return zos_xmem_call(&req);
}

int zos_xmem_alloc(unsigned int size)
{
	zos_scratch.alloc.size = size;
	zos_scratch.alloc.flags = ZOS_XMEM_FLAG_NONE;
	return zxm_call(ZOS_SYSCALL_XMEM_ALLOC, &zos_scratch.alloc);
}

int zos_xmem_free(unsigned int handle)
{
	zos_scratch.free_args.handle = handle;
	return zxm_call(ZOS_SYSCALL_XMEM_FREE, &zos_scratch.free_args);
}

int zos_xmem_read(unsigned int handle, unsigned int offset, void *buffer,
		  unsigned int length)
{
	zos_scratch.xmem_io.handle = handle;
	zos_scratch.xmem_io.offset = offset;
	zos_scratch.xmem_io.buffer = (unsigned int)buffer;
	zos_scratch.xmem_io.length = length;
	return zxm_call(ZOS_SYSCALL_XMEM_READ, &zos_scratch.xmem_io);
}

int zos_xmem_write(unsigned int handle, unsigned int offset,
		   const void *buffer, unsigned int length)
{
	zos_scratch.xmem_io.handle = handle;
	zos_scratch.xmem_io.offset = offset;
	zos_scratch.xmem_io.buffer = (unsigned int)buffer;
	zos_scratch.xmem_io.length = length;
	return zxm_call(ZOS_SYSCALL_XMEM_WRITE, &zos_scratch.xmem_io);
}

int zos_xmem_info(unsigned int handle, struct zos_xmem_info *out)
{
	zos_scratch.info.handle = handle;
	zos_scratch.info.buffer = (unsigned int)out;
	return zxm_call(ZOS_SYSCALL_XMEM_INFO, &zos_scratch.info);
}

int zos_zex_segment_info(unsigned char segment_id,
			 struct zos_zex_seg_info *out)
{
	zos_scratch.seginfo.segment_id = segment_id;
	zos_scratch.seginfo.reserved = 0;
	zos_scratch.seginfo.buffer = (unsigned int)out;
	return zxm_call(ZOS_SYSCALL_ZEX_SEGMENT_INFO, &zos_scratch.seginfo);
}

int zos_far_call(unsigned char entry, const void *in, unsigned int in_len,
		 void *out, unsigned int out_len)
{
	zos_scratch.far.entry = entry;
	zos_scratch.far.reserved = 0;
	zos_scratch.far.in = (unsigned int)in;
	zos_scratch.far.in_len = in_len;
	zos_scratch.far.out = (unsigned int)out;
	zos_scratch.far.out_len = out_len;
	return zxm_call(ZOS_SYSCALL_FAR_CALL, &zos_scratch.far);
}
