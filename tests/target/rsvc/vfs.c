/* ZephyrOS resident-service spike: the `vfs` synthetic namespace service
 * (test only).
 *
 * Top of the object-read chain: VFS turns a name into an object handle
 * (here the client passes the handle the nucleus holds), then calls the
 * FAT provider, which calls IOC.  The same dispatch also implements the
 * two nucleus -> service upcalls (process-exit notification and the
 * executable-stream read the loader performs through a nucleus-held object
 * handle), and the re-entrant self-call depth probe.
 */

#include "zephyros/errno.h"

#include "rsvc_test.h"

static const struct rsvc_api *api;
static uint8_t self_handle;
static uint8_t fat_handle;
static uint8_t vfs_block[RSVC_BLOCK_BYTES];

static int16_t vfs_read(struct rsvc_frame *frame)
{
	struct rsvc_frame inner = *frame;
	int16_t result;

	inner.op = RSVC_OP_FAT_READ;
	result = api->call(fat_handle, RSVC_OP_FAT_READ, &inner);
	if (result < 0) {
		return result;
	}
	if ((frame->flags & RSVC_FRAME_STAGED) != 0u) {
		uint16_t source = (uint16_t)(inner.payload[0] |
					    ((uint16_t)inner.payload[1] << 8));
		uint16_t checksum;

		result = api->move(source, (uint16_t)vfs_block,
				   RSVC_BLOCK_BYTES);
		if (result < 0) {
			return result;
		}
		checksum = (uint16_t)(vfs_block[0] +
				      vfs_block[RSVC_BLOCK_BYTES - 1u]);
		api->note(RSVC_NOTE_CHAIN_WORDS, checksum);
		result = api->copy_out(frame->desc, (uint16_t)vfs_block,
				       RSVC_BLOCK_BYTES);
		if (result < 0) {
			return result;
		}
	}
	frame->result = result;
	return result;
}

static int16_t vfs_exec(struct rsvc_frame *frame)
{
	struct rsvc_frame inner = *frame;
	int16_t result;

	/* The loader holds the object handle; VFS streams it through the
	 * provider chain without ever seeing a pathname. */
	inner.op = RSVC_OP_FAT_READ;
	inner.arg0 = frame->arg0;
	inner.flags = RSVC_FRAME_STREAM;
	result = api->call(fat_handle, RSVC_OP_FAT_READ, &inner);
	if (result < 0) {
		return result;
	}
	{
		/* The restricted API moves data only between the domain and a
		 * nucleus-issued descriptor, so the header lands in the
		 * service's own image (a stack slot is common memory, which
		 * the domain check refuses on purpose). */
		static uint8_t header[4];
		uint16_t source = (uint16_t)(inner.payload[0] |
					    ((uint16_t)inner.payload[1] << 8));

		result = api->move(source, (uint16_t)header, 4u);
		if (result < 0) {
			return result;
		}
		if (header[0] != RSVC_STREAM_MAGIC) {
			return -ZOS_EINVAL;
		}
	}
	api->note(RSVC_NOTE_STREAM_BYTES, RSVC_STREAM_BYTES);
	return (int16_t)RSVC_STREAM_BYTES;
}

static int16_t vfs_dispatch(struct rsvc_frame *frame)
{
	api->note(RSVC_NOTE_SERVICE_CALLS, 1u);
	switch (frame->op) {
	case RSVC_OP_VFS_READ:
		return vfs_read(frame);
	case RSVC_OP_VFS_SELF: {
		struct rsvc_frame inner = *frame;

		if (frame->arg0 == 0u) {
			return 0;
		}
		inner.arg0 = (uint8_t)(frame->arg0 - 1u);
		return api->call(self_handle, RSVC_OP_VFS_SELF, &inner);
	}
	case RSVC_OP_VFS_ON_EXIT:
		api->note(RSVC_NOTE_EXIT_NOTIFY, 1u);
		return (int16_t)frame->arg0;
	case RSVC_OP_VFS_EXEC:
		return vfs_exec(frame);
	default:
		return -ZOS_EINVAL;
	}
}

void rsvc_vfs_init(const struct rsvc_api *table)
{
	api = table;
	/* VFS declares itself re-entrant: a name-resolution path may
	 * re-enter the namespace service while it is active. */
	self_handle = (uint8_t)table->register_ep(RSVC_CLASS_VFS, 0u, (uint16_t)vfs_dispatch,
						  RSVC_EP_REENTRANT, 1u);
	fat_handle = (uint8_t)table->lookup(RSVC_CLASS_FAT, 0u);
}
