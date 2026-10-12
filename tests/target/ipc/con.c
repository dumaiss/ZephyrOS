/* KonaOS IPC acceptance: the `con` console-like synthetic driver (test
 * artifact).
 *
 * WRITE(offset, length) copies a slice of the caller descriptor into the
 * driver's own bank-6 buffer through the nucleus copy primitive and
 * forwards every byte through the nucleus-owned early console; the client
 * checks the bytes on the wire, which proves the descriptor offset
 * convention end to end.  POLL/GETC expose the input side.  PROBE
 * deliberately tries to copy through a descriptor id supplied by the
 * caller, which is how the suite proves forged-id refusal.
 *
 * The ordinary buffer sits in this image's io segment, which the loader
 * places inside bank 6's 8000h-BFFFh, so it follows the driver I/O buffer
 * rule (correction 01 to architecture 36.8).  The IPC_FRAME_SLOW_IO
 * variant targets the buffer of this image's hot segment instead - the
 * deliberate violation that proves a driver buffer outside that range
 * still copies, through the chunked path, and is counted as slow.  The
 * hot-segment buffer lives in con_slow.c, compiled into the _HOT area.
 */

#include "kos/errno.h"
#include "driver.h"

#include "ipc_test.h"

#define CON_BUFFER_BYTES 256u

extern uint8_t ipc_con_slow_buffer[IPC_CON_SLOW_BYTES];

static const struct driver_api *api;
static uint8_t con_buffer[CON_BUFFER_BYTES];
static uint16_t con_slow_address;

static int16_t con_write(struct kos_ipc_frame *frame)
{
	uint16_t length = frame->arg1;
	uint16_t index;
	int16_t result;

	if (length == 0u || length > CON_BUFFER_BYTES ||
	    (uint16_t)frame->arg0 > (uint16_t)(CON_BUFFER_BYTES - length)) {
		return -KOS_EINVAL;
	}
	if ((frame->flags & IPC_FRAME_SLOW_IO) != 0u) {
		result = api->copy_in(frame->desc, frame->arg0,
				      con_slow_address, length);
		if (result < 0) {
			return result;
		}
		for (index = 0u; index < length; ++index) {
			api->console_putc(*(const uint8_t *)
					  (uint16_t)(con_slow_address + index));
		}
		return (int16_t)length;
	}
	result = api->copy_in(frame->desc, frame->arg0,
			      (uint16_t)con_buffer, length);
	if (result < 0) {
		return result;
	}
	for (index = 0u; index < length; ++index) {
		api->console_putc(con_buffer[index]);
	}
	return (int16_t)length;
}

/* The endpoint dispatch entry the manifest names: a module-global
 * symbol so the packer can resolve its hot-image offset. */
int16_t con_dispatch(struct kos_ipc_frame *frame)
{
	switch (frame->op) {
	case IPC_OP_CON_WRITE:
		return con_write(frame);
	case IPC_OP_CON_POLL:
		return api->console_poll();
	case IPC_OP_CON_GETC:
		return api->console_getc();
	case IPC_OP_CON_PROBE: {
		/* The id in payload[0] is whatever the client sent: it is not
		 * a live descriptor, so the nucleus must refuse the copy. */
		uint16_t probe = IPC_FRAME_WORD(frame->payload, 0u);

		return api->copy_in((uint8_t)probe, 0u,
				    (uint16_t)con_buffer, 4u);
	}
	default:
		return -KOS_EINVAL;
	}
}

uint8_t ipc_con_init(const struct driver_boot *boot)
{
	api = boot->api;
	con_slow_address = (uint16_t)ipc_con_slow_buffer;
	return 0u;
}
