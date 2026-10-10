/* ZephyrOS IOC resident service (work package 41).
 *
 * The M8 `ioc` transport as a bank-6 service image: the command/reply
 * lane (SIO1/B) and the bulk lane (SIO1/A) behind the IOC protocol of
 * abi/zos/protocol.h (class ZOS_CLASS_IOC, instance 0, version
 * ZOS_PROTOCOL_VERSION).
 *
 * Segments (architecture 36.6):
 *   hot  this file: the protocol dispatch; ioc.c keeps the transport,
 *        the framing, CRC and the READY/DONE admission; ioc_io.asm keeps
 *        the port I/O and the timing-critical streaming loops.  The
 *        whole-link recovery path (`state->recover`) is reachable at run
 *        time, so ioc_link_bringup() is hot too - not boot-only.
 *   boot ioc_boot.c: the one-time SIO1/B and SIO1/A programming.
 *
 * The service owns no IRQ: the transport is polled with bounded waits,
 * and the command lane is programmed with interrupts off (WR1), exactly
 * as the M8 driver ran.
 *
 * Data movement (36.8): the reply frame and the bulk block live in the
 * service's SERVICE_IO area.  The staged form returns the service-domain
 * address so a service client can stage with `move`; the descriptor form
 * copies in/out through the nucleus, which is how application clients
 * and the temporary arena-module path reach the same buffers.
 */

#include "zephyros/errno.h"
#include "zephyros/ioc.h"
#include "zephyros/ipc.h"
#include "zos/protocol.h"

#include "ioc_driver.h"

/* The staged reply frame: TYPE, STATUS, LENGTH, DATA[26]. */
static uint8_t ioc_reply_frame[ZIOC_PROTOCOL_FRAME_SIZE];
/* The bulk transfer block: one admitted lane transfer, 1..512 bytes. */
static uint8_t ioc_bulk_buffer[ZIOC_BULK_MAX];

static const struct ipc_service_api *ioc_api;
static uint16_t ioc_reply_address;
static uint16_t ioc_bulk_address;
/* 1 once the bounded LINK_SYNC bring-up succeeded and no recovery is
 * pending; ZIOC_OP_READY is the console-side composition probe. */
static uint8_t ioc_link_ready;

static int16_t ioc_service_command(struct zos_ipc_frame *frame)
{
	struct zioc_frame request;
	struct zioc_frame reply;
	uint16_t target;
	uint8_t index;
	int result;

	/* Own-array bound: the copy below indexes request.data and
	 * frame->payload with arg1 as the count. */
	if (frame->arg1 > ZIOC_PROTOCOL_REQUEST_MAX) {
		return (int16_t)-ZOS_EINVAL;
	}
	request.type = frame->arg0;
	request.status = 0u;
	request.length = frame->arg1;
	for (index = 0u; index < frame->arg1; ++index) {
		request.data[index] = frame->payload[index];
	}
	result = ioc_command((void *)&ioc_state, &request, &reply);
	if (result != ZIOC_OK) {
		return (int16_t)result;
	}
	/* Stage the whole native reply frame for the descriptor and staged
	 * forms, then describe it in the reply words. */
	ioc_reply_frame[0] = reply.type;
	ioc_reply_frame[1] = reply.status;
	ioc_reply_frame[2] = reply.length;
	/* The DATA copy runs through a word cursor: an index of its own
	 * keeps the staging store out of the container's rejected
	 * byte-fragmented address form (tools/zmodpack.py). */
	target = 3u;
	for (index = 0u; index < reply.length; ++index) {
		ioc_reply_frame[target] = reply.data[index];
		++target;
	}
	frame->arg0 = reply.type;
	frame->arg1 = reply.length;
	frame->payload[0] = (uint8_t)ioc_reply_address;
	frame->payload[1] = (uint8_t)(ioc_reply_address >> 8);
	frame->payload[2] = reply.status;
	/* The staging handshake for a caller that cannot hold a descriptor
	 * (the bank-7 IOC-FAT shim): where this service's bulk buffer is,
	 * so it can place the bytes a descriptor-less BULK_WRITE commits. */
	frame->payload[3] = (uint8_t)ioc_bulk_address;
	frame->payload[4] = (uint8_t)(ioc_bulk_address >> 8);
	return 0;
}

static int16_t ioc_service_reply(struct zos_ipc_frame *frame)
{
	uint16_t offset = frame->arg0;
	uint16_t length = frame->arg1;

	/* Own-array bound: the staged reply frame is this driver's array
	 * and the nucleus copy below names it as the source. */
	if (length == 0u || offset > ZIOC_PROTOCOL_FRAME_SIZE ||
	    length > (uint16_t)(ZIOC_PROTOCOL_FRAME_SIZE - offset)) {
		return (int16_t)-ZOS_EINVAL;
	}
	return ioc_api->copy_out(frame->desc, offset,
				 (uint16_t)(ioc_reply_address + offset),
				 length);
}

static int16_t ioc_service_bulk_read(struct zos_ipc_frame *frame)
{
	uint16_t length = frame->arg1;
	int result;

	/* The transfer length is not re-validated here: ioc_bulk_read()
	 * refuses 0 and anything above ZIOC_BULK_MAX before any I/O, and
	 * the copy below runs only on its success. */
	result = ioc_bulk_read((void *)&ioc_state, ioc_bulk_buffer, length);
	if (result != ZIOC_OK) {
		return (int16_t)result;
	}
	if (frame->desc != ZOS_IPC_DESC_NONE) {
		if (ioc_api->copy_out(frame->desc, frame->arg0,
				      ioc_bulk_address, length) < 0) {
			return (int16_t)-ZOS_EINVAL;
		}
	}
	frame->payload[0] = (uint8_t)ioc_bulk_address;
	frame->payload[1] = (uint8_t)(ioc_bulk_address >> 8);
	return (int16_t)length;
}

static int16_t ioc_service_bulk_write(struct zos_ipc_frame *frame)
{
	uint16_t length = frame->arg1;
	int result;

	/* The transfer length is not re-validated here: ioc_bulk_write()
	 * refuses 0 and anything above ZIOC_BULK_MAX before any I/O. */
	if (frame->desc != ZOS_IPC_DESC_NONE) {
		/* The descriptor form stages the caller's bytes into the
		 * service block first (the LOADER-mediated equivalent of
		 * the staged form's `move`). */
		if (ioc_api->copy_in(frame->desc, frame->arg0,
				     ioc_bulk_address, length) < 0) {
			return (int16_t)-ZOS_EINVAL;
		}
	}
	result = ioc_bulk_write((void *)&ioc_state, ioc_bulk_buffer,
				length);
	if (result != ZIOC_OK) {
		return (int16_t)result;
	}
	return (int16_t)length;
}

int16_t ioc_service_dispatch(struct zos_ipc_frame *frame)
{
	switch (frame->op) {
	case ZIOC_OP_COMMAND:
		return ioc_service_command(frame);
	case ZIOC_OP_REPLY:
		return ioc_service_reply(frame);
	case ZIOC_OP_BULK_READ:
		return ioc_service_bulk_read(frame);
	case ZIOC_OP_BULK_WRITE:
		return ioc_service_bulk_write(frame);
	case ZIOC_OP_STATUS:
		return (int16_t)ioc_status((void *)&ioc_state);
	case ZIOC_OP_READY:
		return (int16_t)(ioc_link_ready != 0u ? 1 : 0);
	default:
		return (int16_t)-ZOS_EINVAL;
	}
}

uint8_t ioc_service_init(const struct ipc_service_boot *boot)
{
	ioc_api = boot->api;
	ioc_reply_address = (uint16_t)ioc_reply_frame;
	ioc_bulk_address = (uint16_t)ioc_bulk_buffer;
	/* The one bounded LINK_SYNC bring-up, here in the mode-11 window
	 * where the bootstrap interface is visible.  A down link is not a
	 * load refusal: the service initializes, ZIOC_OP_READY reports 0,
	 * and the first ordinary operation attempts the whole-link
	 * recovery.  A build whose own dependencies need the link fails
	 * its boot through those drivers (42e). */
	ioc_link_ready = (uint8_t)(ioc_link_bringup() == ZIOC_OK ? 1u : 0u);
	return 0u;
}
