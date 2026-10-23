/* The console's text grid and capabilities (KCONSOLE_OP_INFO).
 *
 * Its own libkos member, so a program that never asks links none of it.
 * The reply's struct kconsole_info (kos/protocol.h) is copied out of the
 * reply frame.
 */

#include "kos.h"
#include "kos/protocol.h"

int console_info(struct kconsole_info *out)
{
	struct kos_ipc_request req;
	int result;

	if (out == (struct kconsole_info *)0) {
		return -KOS_EINVAL;
	}
	memset(&req, 0, sizeof(req));
	req.frame.op = KCONSOLE_OP_INFO;
	result = call((unsigned char)KOS_HANDLE_STDOUT, &req);
	if (result < 0) {
		return result;
	}
	memcpy(out, req.frame.payload, sizeof(struct kconsole_info));
	return 0;
}
