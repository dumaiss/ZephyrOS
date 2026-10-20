/* One versioned console render stream as a single descriptor transaction.
 *
 * The stream layout is the public kos/console_render.h ABI.  Pending
 * buffered output is flushed before the request so bytes already accepted
 * by the console keep their order.  A backend without the extension
 * answers -KOS_ENOTSUP; libtui then uses its ANSI renderer.
 */

#include "kos.h"
#include "kos/protocol.h"
#include "kos/console_render.h"

int console_render(const void *stream, unsigned int length)
{
	struct kos_ipc_request req;
	int result;

	if (stream == (const void *)0 ||
	    length < KCONSOLE_RENDER_HEADER_SIZE ||
	    length > KOS_IPC_DESC_MAX) {
		return -KOS_EINVAL;
	}
	result = console_flush();
	if (result < 0) {
		return result;
	}
	req.frame.op = KCONSOLE_OP_RENDER;
	req.frame.flags = 0u;
	req.frame.arg0 = 0u;
	req.frame.arg1 = length;
	req.buffer = (void *)stream;
	req.length = length;
	req.direction = KOS_IPC_DESC_IN;
	return call((unsigned char)KOS_HANDLE_STDOUT, &req);
}
