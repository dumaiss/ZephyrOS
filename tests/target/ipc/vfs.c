/* ZephyrOS IPC acceptance: the `vfs` synthetic namespace driver (test
 * artifact).
 *
 * Top of the object-read chain: the client passes the object handle, VFS
 * calls the FAT provider, which calls IOC.  In the direct variant the
 * descriptor id passes down the chain and IOC copies straight to the
 * caller; in the staged variant the block is moved up through the
 * providers' own buffers and VFS copies the requested slice out.  The
 * same dispatch also implements the nucleus -> driver upcalls (exit
 * notification and the executable-object stream), the recursive depth
 * probe (VFS declares itself re-entrant) and one op whose status passes a
 * driver error through unchanged.
 */

#include "zos/errno.h"
#include "driver.h"

#include "ipc_test.h"

static const struct driver_api *api;
static uint8_t self_handle;
static uint8_t fat_handle;
static uint8_t vfs_block[IPC_BLOCK_BYTES];
/* The image holds only bank-relative addresses, and the container format
 * rejects byte-fragmented address stores, so the buffer address is taken
 * once (a plain 16-bit store) and offsets add to it at run time. */
static uint16_t vfs_block_address;

static int16_t vfs_read(struct zos_ipc_frame *frame)
{
	struct zos_ipc_frame inner = *frame;
	uint16_t offset = frame->arg1;
	uint16_t length = IPC_FRAME_WORD(frame->payload, 0u);
	int16_t result;

	result = api->call(fat_handle, IPC_OP_FAT_READ, &inner);
	if (result < 0) {
		return result;
	}
	if ((frame->flags & IPC_FRAME_STAGED) != 0u) {
		uint16_t source = IPC_FRAME_WORD(inner.payload, 0u);
		uint16_t block_length = IPC_FRAME_WORD(inner.payload, 2u);

		result = api->move(source, vfs_block_address, block_length);
		if (result < 0) {
			return result;
		}
		result = api->copy_out(frame->desc, offset,
				       (uint16_t)(vfs_block_address + offset),
				       length);
		if (result < 0) {
			return result;
		}
	}
	return result;
}

static int16_t vfs_exec(struct zos_ipc_frame *frame)
{
	struct zos_ipc_frame inner = *frame;
	int16_t result;

	/* The nucleus holds the object handle; VFS streams it through the
	 * provider chain without ever seeing a pathname or a caller
	 * address. */
	inner.op = IPC_OP_FAT_READ;
	inner.arg0 = frame->arg0;
	inner.flags = IPC_FRAME_STREAM;
	result = api->call(fat_handle, IPC_OP_FAT_READ, &inner);
	if (result < 0) {
		return result;
	}
	{
		/* The restricted API moves data only between the domain and a
		 * nucleus-issued descriptor, so the header lands in the
		 * driver's own image (a stack slot is common or fixed
		 * memory, which the domain check refuses on purpose). */
		static uint8_t header[4];
		uint16_t source = IPC_FRAME_WORD(inner.payload, 0u);
		uint16_t offset = inner.arg1;

		result = api->move((uint16_t)(source + offset),
				   (uint16_t)header, 4u);
		if (result < 0) {
			return result;
		}
		if (header[0] != IPC_STREAM_MAGIC) {
			return -ZOS_EINVAL;
		}
	}
	return (int16_t)IPC_STREAM_BYTES;
}

/* The endpoint dispatch entry the manifest names: a module-global
 * symbol so the packer can resolve its hot-image offset. */
int16_t vfs_dispatch(struct zos_ipc_frame *frame)
{
	switch (frame->op) {
	case IPC_OP_VFS_READ:
		return vfs_read(frame);
	case IPC_OP_VFS_SELF: {
		struct zos_ipc_frame inner = *frame;

		if (frame->arg0 == 0u) {
			return 0;
		}
		inner.arg0 = (uint8_t)(frame->arg0 - 1u);
		return api->call(self_handle, IPC_OP_VFS_SELF, &inner);
	}
	case IPC_OP_VFS_ON_EXIT:
		return (int16_t)frame->arg0;
	case IPC_OP_VFS_EXEC:
		return vfs_exec(frame);
	default:
		return -ZOS_EINVAL;
	}
}

uint8_t ipc_vfs_init(const struct driver_boot *boot)
{
	api = boot->api;
	vfs_block_address = (uint16_t)vfs_block;
	/* Manifest dependencies: 0 is this image's own endpoint (the
	 * recursion probe), 1 the FAT provider; both were resolved on the
	 * host.  VFS declares itself re-entrant in the image metadata: a
	 * name-resolution path may re-enter the namespace driver while it
	 * is active. */
	self_handle = boot->deps[0];
	fat_handle = boot->deps[1];
	return 0u;
}
