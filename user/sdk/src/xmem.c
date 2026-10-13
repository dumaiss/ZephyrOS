/* KonaOS libkos: extended-memory wrappers.
 *
 * Every argument block is marshalled through the shared low-window
 * scratch union (kos_internal.h, the 16 bytes exe.mk places at 0100h), so
 * the family needs no gate block of its own; the raw gate stub is the
 * shared one (user/sdk/src/ipcgate.asm).  The aperture helpers are
 * register-only (user/sdk/src/xmemgate.asm) - no syscall is involved in
 * opening or closing the aperture.
 */

#include "kos.h"
#include "kos_internal.h"

int xalloc(unsigned int size, unsigned int flags)
{
	kos_scratch.xmem_alloc.size = size;
	kos_scratch.xmem_alloc.flags = flags;
	return kos_ipc_raw(KOS_SYSCALL_XMEM_ALLOC,
			   &kos_scratch.xmem_alloc);
}

int xfree(unsigned int handle)
{
	kos_scratch.xmem_free.handle = handle;
	return kos_ipc_raw(KOS_SYSCALL_XMEM_FREE, &kos_scratch.xmem_free);
}

static int zxmem_io(unsigned char number, unsigned int handle,
		    unsigned int offset, const void *buffer,
		    unsigned int length)
{
	kos_scratch.xmem_io.handle = handle;
	kos_scratch.xmem_io.offset = offset;
	kos_scratch.xmem_io.buffer = (unsigned int)buffer;
	kos_scratch.xmem_io.length = length;
	return kos_ipc_raw(number, &kos_scratch.xmem_io);
}

int xread(unsigned int handle, unsigned int offset, void *buffer,
		  unsigned int length)
{
	return zxmem_io(KOS_SYSCALL_XMEM_READ, handle, offset, buffer,
			length);
}

int xwrite(unsigned int handle, unsigned int offset,
		   const void *buffer, unsigned int length)
{
	return zxmem_io(KOS_SYSCALL_XMEM_WRITE, handle, offset, buffer,
			length);
}

int xinfo(unsigned int handle, struct kos_xmem_info *out)
{
	if (out == (struct kos_xmem_info *)0) {
		return -KOS_EINVAL;
	}
	kos_scratch.xmem_info.handle = handle;
	kos_scratch.xmem_info.buffer = (unsigned int)out;
	return kos_ipc_raw(KOS_SYSCALL_XMEM_INFO, &kos_scratch.xmem_info);
}

int xmap(unsigned int handle, unsigned int *address)
{
	int token;

	if (address == (unsigned int *)0) {
		return -KOS_EINVAL;
	}
	kos_scratch.xmem_map.handle = handle;
	kos_scratch.xmem_map.address = 0u;
	token = kos_ipc_raw(KOS_SYSCALL_XMEM_MAP, &kos_scratch.xmem_map);
	if (token < 0) {
		return token;
	}
	*address = kos_scratch.xmem_map.address;
	return token;
}
