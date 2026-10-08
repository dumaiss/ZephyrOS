/* ZephyrOS resident-service spike: the `ioc` synthetic block service
 * (test only).
 *
 * Bottom of the vfs -> fat -> ioc chain.  READ(object, token) fills a
 * 512-byte block in bank 6, deterministic in (object, token), and either
 * copies it straight out through the caller descriptor (the "direct"
 * variant, one copy) or leaves it in its own buffer for `fat` to stage up
 * the chain (the "staged" variant, three copies).
 */

#include "zephyros/errno.h"

#include "rsvc_test.h"

#define IOC_BASE_PATTERN 0x40u
#define IOC_PATTERN_MASK 0x3Fu

static const struct rsvc_api *api;
static uint8_t ioc_block[RSVC_BLOCK_BYTES];
static uint16_t ioc_block_address;

static int16_t ioc_read(struct rsvc_frame *frame)
{
	uint8_t seed = (uint8_t)(frame->arg0 + frame->arg1);
	uint16_t index;
	uint16_t checksum = 0u;

	for (index = 0u; index < RSVC_BLOCK_BYTES; ++index) {
		ioc_block[index] = (uint8_t)(
			IOC_BASE_PATTERN +
			(uint8_t)((seed + (uint8_t)index) & IOC_PATTERN_MASK));
	}
	if ((frame->flags & RSVC_FRAME_STREAM) != 0u) {
		/* An executable object, not a plain data block: the first byte
		 * carries the container magic the reader checks before it
		 * streams anything. */
		ioc_block[0] = RSVC_STREAM_MAGIC;
	}
	checksum = (uint16_t)(ioc_block[0] + ioc_block[RSVC_BLOCK_BYTES - 1u]);
	api->note(RSVC_NOTE_CHAIN_WORDS, checksum);
	/* The block address travels up the chain in the reply frame so the
	 * next service can stage from it without knowing any absolute
	 * address in advance. */
	frame->payload[0] = (uint8_t)(ioc_block_address & 0xFFu);
	frame->payload[1] = (uint8_t)(ioc_block_address >> 8);
	frame->payload[2] = (uint8_t)RSVC_BLOCK_BYTES;
	frame->payload[3] = (uint8_t)(RSVC_BLOCK_BYTES >> 8);
	if ((frame->flags & RSVC_FRAME_STAGED) == 0u &&
	    frame->desc != RSVC_DESC_NONE) {
		/* A caller-supplied buffer gets the block directly; without
		 * one (the loader's object stream) the block stays here and
		 * the reply frame carries its address up the chain. */
		int16_t result = api->copy_out(frame->desc, (uint16_t)ioc_block,
					       RSVC_BLOCK_BYTES);

		if (result < 0) {
			return result;
		}
	}
	return (int16_t)RSVC_BLOCK_BYTES;
}

static int16_t ioc_dispatch(struct rsvc_frame *frame)
{
	api->note(RSVC_NOTE_SERVICE_CALLS, 1u);
	if (frame->op != RSVC_OP_IOC_READ) {
		return -ZOS_EINVAL;
	}
	return ioc_read(frame);
}

void rsvc_ioc_init(const struct rsvc_api *table)
{
	api = table;
	ioc_block_address = (uint16_t)ioc_block;
	(void)table->register_ep(RSVC_CLASS_IOC, 0u, (uint16_t)ioc_dispatch, 0u, 1u);
}
