/* ZephyrOS libzos: extended-memory wrappers (package 43).
 *
 * Every argument block is marshalled through the shared low-window
 * scratch union (zos_internal.h, the 16 bytes zex.mk places at 0100h), so
 * the family needs no gate block of its own; the raw gate stub is the
 * shared one (user/sdk/src/ipcgate.asm).  The aperture helpers are
 * register-only (user/sdk/src/xmemgate.asm) - no syscall is involved in
 * opening or closing the aperture.
 */

#include "zos.h"
#include "zos_internal.h"

int zos_xmem_alloc(unsigned int size, unsigned int flags)
{
	zos_scratch.xmem_alloc.size = size;
	zos_scratch.xmem_alloc.flags = flags;
	return zos_ipc_raw(ZOS_SYSCALL_XMEM_ALLOC,
			   &zos_scratch.xmem_alloc);
}

int zos_xmem_free(unsigned int handle)
{
	zos_scratch.xmem_free.handle = handle;
	return zos_ipc_raw(ZOS_SYSCALL_XMEM_FREE, &zos_scratch.xmem_free);
}

static int zxmem_io(unsigned char number, unsigned int handle,
		    unsigned int offset, const void *buffer,
		    unsigned int length)
{
	zos_scratch.xmem_io.handle = handle;
	zos_scratch.xmem_io.offset = offset;
	zos_scratch.xmem_io.buffer = (unsigned int)buffer;
	zos_scratch.xmem_io.length = length;
	return zos_ipc_raw(number, &zos_scratch.xmem_io);
}

int zos_xmem_read(unsigned int handle, unsigned int offset, void *buffer,
		  unsigned int length)
{
	return zxmem_io(ZOS_SYSCALL_XMEM_READ, handle, offset, buffer,
			length);
}

int zos_xmem_write(unsigned int handle, unsigned int offset,
		   const void *buffer, unsigned int length)
{
	return zxmem_io(ZOS_SYSCALL_XMEM_WRITE, handle, offset, buffer,
			length);
}

int zos_xmem_info(unsigned int handle, struct zos_xmem_info *out)
{
	if (out == (struct zos_xmem_info *)0) {
		return -ZOS_EINVAL;
	}
	zos_scratch.xmem_info.handle = handle;
	zos_scratch.xmem_info.buffer = (unsigned int)out;
	return zos_ipc_raw(ZOS_SYSCALL_XMEM_INFO, &zos_scratch.xmem_info);
}

int zos_xmem_map(unsigned int handle, unsigned int *address)
{
	int token;

	if (address == (unsigned int *)0) {
		return -ZOS_EINVAL;
	}
	zos_scratch.xmem_map.handle = handle;
	zos_scratch.xmem_map.address = 0u;
	token = zos_ipc_raw(ZOS_SYSCALL_XMEM_MAP, &zos_scratch.xmem_map);
	if (token < 0) {
		return token;
	}
	*address = zos_scratch.xmem_map.address;
	return token;
}
