/* ZephyrOS IPC acceptance: the `echo` synthetic driver (test artifact).
 *
 * The null call: the cheapest possible protocol on the IPC substrate, used
 * to measure the raw crossing and dispatch cost, to prove re-entrancy
 * refusal (ECHO_SELF calls its own endpoint from inside its own dispatch)
 * and to expose the descriptor id the nucleus handed the call
 * (ECHO_DESC).
 *
 * The image's hot segment carries the dispatch entry and this init entry;
 * the boot segment (echo_boot.c) records its single bring-up run.  The
 * resident loader has already registered the declared endpoint when the
 * init runs, so the init resolves its own handle by class/instance, the
 * same discovery a peer uses.
 */

#include "zos/errno.h"
#include "driver.h"

#include "ipc_test.h"

static const struct driver_api *api;
static uint8_t self_handle;

/* The endpoint dispatch entry the manifest names: a module-global
 * symbol so the packer can resolve its hot-image offset. */
int16_t echo_dispatch(struct zos_ipc_frame *frame)
{
	switch (frame->op) {
	case IPC_OP_ECHO_EXCHANGE:
		frame->payload[1] = frame->payload[0];
		return (int16_t)((uint16_t)frame->payload[0] + 1u);
	case IPC_OP_ECHO_NULL:
		return (int16_t)ZOS_IPC_FRAME_SIZE;
	case IPC_OP_ECHO_SELF: {
		struct zos_ipc_frame inner = *frame;

		/* The endpoint is active: the nucleus must refuse this with
		 * -ZOS_EBUSY because echo declares no re-entrancy. */
		return api->call(self_handle, IPC_OP_ECHO_NULL, &inner);
	}
	case IPC_OP_ECHO_DESC:
		return (int16_t)frame->desc;
	default:
		return -ZOS_EINVAL;
	}
}

uint8_t ipc_echo_init(const struct driver_boot *boot)
{
	api = boot->api;
	/* Manifest dependency 0: this image's own endpoint, resolved on the
	 * host (the ECHO_SELF refusal probe calls it). */
	self_handle = boot->deps[0];
	return 0u;
}
