/* KonaOS libkos: the sound card client over the SOUND driver.
 *
 * `/dev/sound` (abi/kos/protocol.h, class KOS_CLASS_SOUND).  INFO and
 * RESET are descriptor-less; WRITE carries its {target, value} pairs in
 * one IN descriptor per call, at most KOS_IPC_DESC_MAX bytes each, so a
 * longer sequence is several calls.
 *
 * Return convention matches kos/console.h: a non-negative result, or the
 * negated native error code.
 */

#include "kos.h"

#include "kos/sound.h"

static int sound_call(int handle, uint8_t op, const void *buffer,
		      uint16_t length, struct kos_ipc_request *req)
{
	req->frame.op = op;
	req->frame.flags = 0u;
	req->frame.desc = KOS_IPC_DESC_NONE;
	req->frame.arg0 = 0u;
	req->frame.arg1 = length;
	req->buffer = (void *)buffer;
	req->length = length;
	req->direction = KOS_IPC_DESC_IN;
	return call((unsigned char)handle, req);
}

int sound_open(unsigned char exclusive)
{
	unsigned char type = 0u;
	int handle = bind("/dev/sound",
			  exclusive != 0u ? KOS_BIND_EXCLUSIVE : 0u, &type);

	if (handle < 0) {
		return handle;
	}
	if (type != KOS_TYPE_SOUND) {
		(void)unbind((unsigned char)handle);
		return -KOS_EINVAL;
	}
	return handle;
}

int sound_close(int handle)
{
	return unbind((unsigned char)handle);
}

int sound_info(int handle, struct ksound_info *info)
{
	struct kos_ipc_request req;
	int result;

	if (info == (struct ksound_info *)0) {
		return -KOS_EINVAL;
	}
	result = sound_call(handle, KSOUND_OP_INFO, (const void *)0, 0u, &req);
	if (result < 0) {
		return result;
	}
	memcpy(info, req.frame.payload, sizeof(struct ksound_info));
	return 0;
}

int sound_write(int handle, const struct ksound_write *writes,
		unsigned int count)
{
	struct kos_ipc_request req;
	const uint8_t *at = (const uint8_t *)writes;
	unsigned int left = count;
	unsigned int done = 0u;

	if (writes == (const struct ksound_write *)0 && count != 0u) {
		return -KOS_EINVAL;
	}
	while (left != 0u) {
		unsigned int chunk = left;
		int result;

		if (chunk > KOS_IPC_DESC_MAX / sizeof(struct ksound_write)) {
			chunk = KOS_IPC_DESC_MAX / sizeof(struct ksound_write);
		}
		result = sound_call(handle, KSOUND_OP_WRITE, at,
				    (uint16_t)(chunk * 2u), &req);
		if (result < 0) {
			return done != 0u ? (int)done : result;
		}
		at += chunk * 2u;
		done += chunk;
		left -= chunk;
	}
	return (int)done;
}

int sound_reset(int handle)
{
	struct kos_ipc_request req;

	return sound_call(handle, KSOUND_OP_RESET, (const void *)0, 0u, &req);
}
