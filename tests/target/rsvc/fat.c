/* ZephyrOS resident-service spike: the `fat` synthetic provider (test only).
 *
 * Middle of the vfs -> fat -> ioc chain.  It discovers the IOC endpoint at
 * init, forwards the read, and in the staged variant moves the 512-byte
 * block from the IOC buffer into its own staging buffer through the
 * nucleus move primitive.
 */

#include "zephyros/errno.h"

#include "rsvc_test.h"

static const struct rsvc_api *api;
static uint8_t ioc_handle;
static uint8_t fat_block[RSVC_BLOCK_BYTES];
static uint16_t fat_block_address;

static uint16_t frame_word(const struct rsvc_frame *frame, uint8_t offset)
{
	return (uint16_t)(frame->payload[offset] |
			  ((uint16_t)frame->payload[offset + 1u] << 8));
}

static int16_t fat_read(struct rsvc_frame *frame)
{
	struct rsvc_frame inner = *frame;
	int16_t result;

	result = api->call(ioc_handle, RSVC_OP_IOC_READ, &inner);
	if (result < 0) {
		return result;
	}
	if ((frame->flags & RSVC_FRAME_STAGED) != 0u) {
		uint16_t source = frame_word(&inner, 0u);
		uint16_t length = frame_word(&inner, 2u);
		uint16_t checksum;

		result = api->move(source, (uint16_t)fat_block, length);
		if (result < 0) {
			return result;
		}
		checksum = (uint16_t)(fat_block[0] +
				      fat_block[RSVC_BLOCK_BYTES - 1u]);
		api->note(RSVC_NOTE_CHAIN_WORDS, checksum);
		inner.payload[0] = (uint8_t)(fat_block_address & 0xFFu);
		inner.payload[1] = (uint8_t)(fat_block_address >> 8);
	}
	*frame = inner;
	return result;
}

static int16_t fat_dispatch(struct rsvc_frame *frame)
{
	api->note(RSVC_NOTE_SERVICE_CALLS, 1u);
	if (frame->op != RSVC_OP_FAT_READ) {
		return -ZOS_EINVAL;
	}
	return fat_read(frame);
}

void rsvc_fat_init(const struct rsvc_api *table)
{
	api = table;
	fat_block_address = (uint16_t)fat_block;
	(void)table->register_ep(RSVC_CLASS_FAT, 0u, (uint16_t)fat_dispatch, 0u, 1u);
	/* Dependency discovery: the provider finds its transport by
	 * class/instance, never by a raw address or bank number. */
	ioc_handle = (uint8_t)table->lookup(RSVC_CLASS_IOC, 0u);
}
