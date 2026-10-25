/* truncate(handle, length): KFILE_OP_TRUNCATE, its own libkos member so
 * programs that do not truncate do not link it (docs/sdk.md). */

#include "kos.h"
#include "kos/protocol.h"

int truncate(unsigned int handle, unsigned long length)
{
	struct kos_ipc_request req;

	req.frame.op = KFILE_OP_TRUNCATE;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.frame.payload[1] = (unsigned char)(length & 0xFFu);
	req.frame.payload[2] = (unsigned char)((length >> 8) & 0xFFu);
	req.frame.payload[3] = (unsigned char)((length >> 16) & 0xFFu);
	req.frame.payload[4] = (unsigned char)((length >> 24) & 0xFFu);
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	return call((unsigned char)handle, &req);
}
