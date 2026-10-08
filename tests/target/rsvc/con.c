/* ZephyrOS resident-service spike: the `con` synthetic console service
 * (test only).
 *
 * Client stub protocol: WRITE(desc, length) copies from the caller
 * descriptor into the service's own bank-6 buffer and forwards each byte
 * through the nucleus console primitive; POLL/GETC expose the input side.
 * PROBE deliberately tries to copy through a descriptor id supplied by
 * the caller, which is how the spike proves the descriptor validation
 * refuses forged ids (C2).
 */

#include "zephyros/errno.h"

#include "rsvc_test.h"

#define CON_BUFFER_BYTES 256u

static const struct rsvc_api *api;
static uint8_t con_buffer[CON_BUFFER_BYTES];

static int16_t con_write(struct rsvc_frame *frame)
{
	uint16_t length = frame->arg1;
	uint16_t index;
	int16_t result;

	if (length == 0u || length > CON_BUFFER_BYTES) {
		return -ZOS_EINVAL;
	}
	result = api->copy_in(frame->desc, (uint16_t)con_buffer, length);
	if (result < 0) {
		return result;
	}
	for (index = 0u; index < length; ++index) {
		api->console_putc(con_buffer[index]);
	}
	return (int16_t)length;
}

static int16_t con_dispatch(struct rsvc_frame *frame)
{
	api->note(RSVC_NOTE_SERVICE_CALLS, 1u);
	switch (frame->op) {
	case RSVC_OP_CON_WRITE:
		return con_write(frame);
	case RSVC_OP_CON_POLL:
		return api->console_poll();
	case RSVC_OP_CON_GETC:
		return api->console_getc();
	case RSVC_OP_CON_PROBE: {
		/* The id in payload[0] is whatever the client sent: it is not
		 * a live descriptor, so the nucleus must refuse the copy. */
		uint16_t probe = (uint16_t)(frame->payload[0] |
					    ((uint16_t)frame->payload[1] << 8));

		return api->copy_in((uint8_t)probe, (uint16_t)con_buffer, 4u);
	}
	default:
		return -ZOS_EINVAL;
	}
}

void rsvc_con_init(const struct rsvc_api *table)
{
	api = table;
	(void)table->register_ep(RSVC_CLASS_CON, 0u, (uint16_t)con_dispatch, 0u, 1u);
}
