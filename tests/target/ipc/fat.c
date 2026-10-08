/* ZephyrOS IPC acceptance: the `fat` synthetic provider (test artifact).
 *
 * Middle of the vfs -> fat -> ioc chain.  It discovers the IOC endpoint at
 * init, forwards the read, and in the staged variant moves the 512-byte
 * block from the IOC buffer into its own staging buffer through the
 * nucleus same-domain move primitive, then rewrites the reply's block
 * address so the block travels up the chain by descriptor, never by raw
 * cross-domain pointer.
 */

#include "zephyros/errno.h"
#include "zephyros/ipc.h"

#include "ipc_test.h"

static const struct ipc_service_api *api;
static uint8_t ioc_handle;
static uint8_t fat_block[IPC_BLOCK_BYTES];
static uint16_t fat_block_address;

static int16_t fat_read(struct zos_ipc_frame *frame)
{
	struct zos_ipc_frame inner = *frame;
	int16_t result;

	result = api->call(ioc_handle, IPC_OP_IOC_READ, &inner);
	if (result < 0) {
		return result;
	}
	if ((frame->flags & IPC_FRAME_STAGED) != 0u) {
		uint16_t source = IPC_FRAME_WORD(inner.payload, 0u);
		uint16_t length = IPC_FRAME_WORD(inner.payload, 2u);

		result = api->move(source, (uint16_t)fat_block, length);
		if (result < 0) {
			return result;
		}
		inner.payload[0] = IPC_WORD_LO(fat_block_address);
		inner.payload[1] = IPC_WORD_HI(fat_block_address);
		inner.payload[2] = IPC_WORD_LO(length);
		inner.payload[3] = IPC_WORD_HI(length);
	}
	*frame = inner;
	return result;
}

static int16_t fat_dispatch(struct zos_ipc_frame *frame)
{
	if (frame->op != IPC_OP_FAT_READ) {
		return -ZOS_EINVAL;
	}
	return fat_read(frame);
}

void ipc_fat_init(const struct ipc_service_api *table)
{
	api = table;
	fat_block_address = (uint16_t)fat_block;
	(void)table->register_ep(IPC_CLASS_FAT, 0u, (uint16_t)fat_dispatch,
				 0u, 1u);
	/* Dependency discovery: the provider finds its transport by
	 * class/instance, never by a raw address or bank number. */
	ioc_handle = (uint8_t)table->lookup(IPC_CLASS_IOC, 0u);
}
