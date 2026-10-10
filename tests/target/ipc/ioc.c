/* ZephyrOS IPC acceptance: the `ioc` synthetic block service (test
 * artifact).
 *
 * Bottom of the vfs -> fat -> ioc chain.  READ fills a 512-byte block in
 * bank 6, deterministic in the object handle, and either copies the
 * requested slice straight out through the caller descriptor (the
 * "direct" variant: the descriptor id travels down the chain and the
 * service holding the data copies it straight to the caller) or leaves
 * the block in its own buffer for `fat` to stage up the chain (the
 * "staged" variant).
 *
 * Frame convention: arg0 = object handle, arg1 = byte offset,
 * payload[0..1] = length; the reply carries the staged block address in
 * payload[0..1] and its length in payload[2..3].
 */

#include "zephyros/errno.h"
#include "zephyros/ipc.h"

#include "ipc_test.h"

static const struct ipc_service_api *api;
static uint8_t ioc_block[IPC_BLOCK_BYTES];
static uint16_t ioc_block_address;

static int16_t ioc_read(struct zos_ipc_frame *frame)
{
	uint8_t seed = frame->arg0;
	uint16_t offset = frame->arg1;
	uint16_t length = IPC_FRAME_WORD(frame->payload, 0u);
	uint16_t index;

	for (index = 0u; index < IPC_BLOCK_BYTES; ++index) {
		ioc_block[index] = (uint8_t)(
			IPC_IOC_PATTERN_BASE +
			(uint8_t)((seed + (uint8_t)index) &
				  IPC_IOC_PATTERN_MASK));
	}
	if ((frame->flags & IPC_FRAME_STREAM) != 0u) {
		/* An executable object, not a plain data block: the first byte
		 * carries the container magic the reader checks before it
		 * streams anything. */
		ioc_block[0] = IPC_STREAM_MAGIC;
	}
	if (offset > IPC_BLOCK_BYTES ||
	    length > (uint16_t)(IPC_BLOCK_BYTES - offset)) {
		return -ZOS_EINVAL;
	}
	frame->payload[0] = IPC_WORD_LO(ioc_block_address);
	frame->payload[1] = IPC_WORD_HI(ioc_block_address);
	frame->payload[2] = IPC_WORD_LO(IPC_BLOCK_BYTES);
	frame->payload[3] = IPC_WORD_HI(IPC_BLOCK_BYTES);
	if ((frame->flags & IPC_FRAME_STAGED) == 0u &&
	    frame->desc != ZOS_IPC_DESC_NONE) {
		/* A caller-supplied buffer gets the slice directly; without
		 * one (the loader's object stream) the block stays here. */
		int16_t result = api->copy_out(frame->desc, offset,
					       (uint16_t)(ioc_block_address +
							  offset),
					       length);

		if (result < 0) {
			return result;
		}
	}
	return (int16_t)length;
}

/* The endpoint dispatch entry the manifest names: a module-global
 * symbol so the packer can resolve its hot-image offset. */
int16_t ioc_dispatch(struct zos_ipc_frame *frame)
{
	if (frame->op != IPC_OP_IOC_READ) {
		return -ZOS_EINVAL;
	}
	return ioc_read(frame);
}

uint8_t ipc_ioc_init(const struct ipc_service_boot *boot)
{
	api = boot->api;
	ioc_block_address = (uint16_t)ioc_block;
	return 0u;
}
