/* ZephyrOS resident-service spike: the `echo` synthetic service (test only).
 *
 * The null call: the cheapest possible protocol on the IPC substrate, used
 * to measure the raw crossing and dispatch cost and to prove re-entrancy
 * refusal (ECHO_SELF calls its own endpoint from inside its own dispatch).
 */

#include "zephyros/errno.h"

#include "rsvc_test.h"

static const struct rsvc_api *api;
static uint8_t self_handle;

static int16_t echo_dispatch(struct rsvc_frame *frame)
{
	api->note(RSVC_NOTE_SERVICE_CALLS, 1u);
	switch (frame->op) {
	case RSVC_OP_ECHO_EXCHANGE:
		frame->payload[1] = frame->payload[0];
		return (int16_t)((uint16_t)frame->payload[0] + 1u);
	case RSVC_OP_ECHO_NULL:
		return (int16_t)RSVC_FRAME_MAX;
	case RSVC_OP_ECHO_SELF: {
		struct rsvc_frame inner = *frame;

		inner.op = RSVC_OP_ECHO_NULL;
		return api->call(self_handle, RSVC_OP_ECHO_NULL, &inner);
	}
	default:
		return -ZOS_EINVAL;
	}
}

void rsvc_echo_init(const struct rsvc_api *table)
{
	api = table;
	self_handle = (uint8_t)table->register_ep(RSVC_CLASS_ECHO, 0u, (uint16_t)echo_dispatch, 0u, 1u);
}
