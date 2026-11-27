/* KonaOS IOC resident driver.
 *
 * The IOC transport as a bank-6 driver image: the command/reply
 * lane (SIO1/B) and the bulk lane (SIO1/A) behind the IOC protocol of
 * abi/kos/protocol.h (class KOS_CLASS_IOC, instance 0, version
 * KOS_PROTOCOL_VERSION).
 *
 * Segments (architecture 5.2):
 *   hot  this file: the protocol dispatch; ioc.c keeps the transport,
 *        the framing, CRC and the READY/DONE admission; ioc_io.asm keeps
 *        the port I/O and the timing-critical streaming loops.  The
 *        whole-link recovery path (`state->recover`) is reachable at run
 *        time, so ioc_link_bringup() is hot too - not boot-only.
 *   boot ioc_boot.c: the one-time SIO1/B and SIO1/A programming.
 *
 * The driver owns no IRQ: the transport is polled with bounded waits,
 * and the command lane is programmed with interrupts off (WR1).
 *
 * Data movement (4.9): the reply frame and the bulk block live in the
 * driver's DRIVER_IO area.  The staged form returns the driver-domain
 * address so a driver client can stage with `move`; the descriptor form
 * copies in/out through the nucleus, which is how application clients
 * and the temporary arena-module path reach the same buffers.
 */

#include "kos/errno.h"
#include "ioc.h"
#include "driver.h"
#include "kos/protocol.h"

#include "ioc_driver.h"
#include "ioc_proto.h"

/* The staged reply frame: TYPE, STATUS, LENGTH, DATA[26]. */
static uint8_t ioc_reply_frame[KIOC_PROTOCOL_FRAME_SIZE];
/* The bulk transfer block: one admitted lane transfer, 1..512 bytes. */
static uint8_t ioc_bulk_buffer[KIOC_BULK_MAX];

/* Both read by ioc_driver_command_read (ioc_io.asm). */
const struct driver_api *ioc_api;
uint16_t ioc_reply_address;
static uint16_t ioc_bulk_address;
#if KOS_IOC_PHASES
/* The phase table's address, captured once at init: the reply publishes
 * it as bytes, and a byte-fragmented store of a relocatable address is
 * exactly the form the container packer refuses. */
static uint16_t ioc_phases_address;
#endif
/* 1 once the bounded LINK_SYNC bring-up succeeded and no recovery is
 * pending; KIOC_OP_READY is the console-side composition probe. */
static uint8_t ioc_link_ready;


int16_t ioc_driver_command(struct kos_ipc_frame *frame)
{
	struct ioc_request request;
	/* The staged reply frame is the native frame's layout, so the
	 * exchange writes the reply into it directly. */
	struct kioc_frame *reply = (struct kioc_frame *)ioc_reply_address;
	int result;

	/* Own-array bound: the exchange copies arg1 bytes of the payload
	 * into its wire packet. */
	if (frame->arg1 > KIOC_PROTOCOL_REQUEST_MAX) {
		return (int16_t)-KOS_EINVAL;
	}
	request.type = frame->arg0;
	request.status = 0u;
	request.length = frame->arg1;
	request.data = frame->payload;
	result = ioc_command((void *)&ioc_state, &request, reply);
	if (result != KIOC_OK) {
		return (int16_t)result;
	}
	frame->arg0 = reply->type;
	frame->arg1 = reply->length;
	frame->payload[0] = (uint8_t)ioc_reply_address;
	frame->payload[1] = (uint8_t)(ioc_reply_address >> 8);
	frame->payload[2] = reply->status;
#if KOS_IOC_PHASES
	/* The measurement build's handshake: where this
	 * driver's read-path phase table is, so the IOC-FAT driver can read
	 * the stamps of the exchange it just ran. */
	frame->payload[5] = (uint8_t)ioc_phases_address;
	frame->payload[6] = (uint8_t)(ioc_phases_address >> 8);
#endif
	return 0;
}

static int16_t ioc_driver_reply(struct kos_ipc_frame *frame)
{
	uint16_t offset = frame->arg0;
	uint16_t length = frame->arg1;

	/* Own-array bound: the staged reply frame is this driver's array
	 * and the nucleus copy below names it as the source. */
	if (length == 0u || offset > KIOC_PROTOCOL_FRAME_SIZE ||
	    length > (uint16_t)(KIOC_PROTOCOL_FRAME_SIZE - offset)) {
		return (int16_t)-KOS_EINVAL;
	}
	return ioc_api->copy_out(frame->desc, offset,
				 (uint16_t)(ioc_reply_address + offset),
				 length);
}

/* The direct form's buffer: a resident caller names a buffer of its own in
 * a payload word (payload[0..1]; payload[22..23] for COMMAND_READ), and
 * the bulk loop moves the bytes straight to or from it.
 * Every resident image is mapped together in bank 6's hot tier
 * (6000h-DFFFh, formats/include/mod.h), so the address is valid here as it
 * is in the caller; anything outside that tier is refused, except that
 * COMMAND_READ (ioc_io.asm) also accepts a buffer inside the live borrow. */
#define IOC_DIRECT_BASE 0x6000u
#define IOC_DIRECT_END  0xE000u

static uint8_t *ioc_direct_at(const uint8_t *word, uint16_t length)
{
	uint16_t address = (uint16_t)((uint16_t)word[0] |
				      ((uint16_t)word[1] << 8));

	if (address < IOC_DIRECT_BASE || length > IOC_DIRECT_END ||
	    address > (uint16_t)(IOC_DIRECT_END - length)) {
		return (uint8_t *)0;
	}
	return (uint8_t *)address;
}

static uint8_t *ioc_direct_buffer(const struct kos_ipc_frame *frame,
				  uint16_t length)
{
	return ioc_direct_at(frame->payload, length);
}


static int16_t ioc_driver_bulk_read(struct kos_ipc_frame *frame)
{
	uint16_t length = frame->arg1;
	uint8_t *dst = (uint8_t *)ioc_bulk_address;
	int result;

	/* One packet's length: the descriptor form stages it in the
	 * 512-byte buffer (a run is KIOC_OP_COMMAND_READ's), so a longer one
	 * becomes 0, which ioc_bulk_read() refuses before any I/O; the copy
	 * below runs only on its success. */
	if (length > KIOC_BULK_MAX) {
		length = 0u;
	}
	if (frame->desc == KOS_IPC_DESC_NONE) {
		dst = ioc_direct_buffer(frame, length);
		if (dst == (uint8_t *)0) {
			return (int16_t)-KOS_EINVAL;
		}
	}
	result = ioc_bulk_read((void *)&ioc_state, dst, length);
	if (result != KIOC_OK) {
		return (int16_t)result;
	}
	if (frame->desc != KOS_IPC_DESC_NONE) {
		if (ioc_api->copy_out(frame->desc, frame->arg0,
				      ioc_bulk_address, length) < 0) {
			return (int16_t)-KOS_EINVAL;
		}
	}
	return (int16_t)length;
}

static int16_t ioc_driver_bulk_write(struct kos_ipc_frame *frame)
{
	uint16_t length = frame->arg1;
	const uint8_t *src = (const uint8_t *)ioc_bulk_address;
	int result;

	/* The transfer length is not re-validated here: ioc_bulk_write()
	 * refuses 0 and anything above KIOC_BULK_MAX before any I/O. */
	if (frame->desc != KOS_IPC_DESC_NONE) {
		/* The descriptor form stages the caller's bytes into this
		 * driver's buffer first. */
		if (ioc_api->copy_in(frame->desc, frame->arg0,
				     ioc_bulk_address, length) < 0) {
			return (int16_t)-KOS_EINVAL;
		}
	} else {
		src = ioc_direct_buffer(frame, length);
		if (src == (const uint8_t *)0) {
			return (int16_t)-KOS_EINVAL;
		}
	}
	result = ioc_bulk_write((void *)&ioc_state, src, length);
	if (result != KIOC_OK) {
		return (int16_t)result;
	}
	return (int16_t)length;
}

/* KIOC_OP_COMMAND_READ: the command, then the bulk receive its READY
 * admitted, in one call - a run's packets back to back when it admitted
 * more than one - in ioc_io.asm.  The READY is judged before the first
 * byte lands: ioc_command admits only a status-0, eight-byte READY with a
 * nonzero transfer id from a bulk-arming class, ioc_admitted_read only
 * the MCU-to-Z80 direction, and the assembly adds the capacity
 * (payload[20..21]), the echoed offset (READY DATA 4-7 against the
 * request's DATA 2-5) and the buffer (payload[22..23]: the hot tier, or
 * inside the live borrow, KOS_IPC_DESC_OUT).  A refused READY leaves the
 * controller mid-phase, so it is abandoned (phase IDLE, and the link
 * recovers before the next command) and the call answers -KOS_EINVAL.
 * Each packet's header and trailer still follow its payload on the wire
 * (ioc_io_run_read). */
extern int16_t ioc_driver_command_read(struct kos_ipc_frame *frame);
#if defined(__SDCC)
_Static_assert(KIOC_COMMAND_READ_CAPACITY == 20u &&
	       KIOC_COMMAND_READ_BUFFER == 22u && KIOC_OK == 0 &&
	       KOS_EINVAL == 2 && KOS_IPC_DESC_OUT == 1u &&
	       __builtin_offsetof(struct driver_api, borrowed) == 34u,
	       "ioc_io.asm IOC_CR_*");
#endif

/* The RT clock's three operations (KIOC_OP_RT_CLOCK, KOS_OP_ACQUIRE,
 * KOS_OP_RELEASE), in ioc_io.asm; anything else is -KOS_EINVAL. */
extern int16_t ioc_driver_rt(struct kos_ipc_frame *frame);

int16_t ioc_driver_dispatch(struct kos_ipc_frame *frame)
{
	switch (frame->op) {
	case KIOC_OP_COMMAND:
		return ioc_driver_command(frame);
	case KIOC_OP_REPLY:
		return ioc_driver_reply(frame);
	case KIOC_OP_BULK_READ:
		return ioc_driver_bulk_read(frame);
	case KIOC_OP_BULK_WRITE:
		return ioc_driver_bulk_write(frame);
	case KIOC_OP_STATUS:
		return (int16_t)ioc_status((void *)&ioc_state);
	case KIOC_OP_READY:
		return (int16_t)(ioc_link_ready != 0u ? 1 : 0);
	case KIOC_OP_COMMAND_READ:
		return ioc_driver_command_read(frame);
	default:
		return ioc_driver_rt(frame);
	}
}

uint8_t ioc_driver_init(const struct driver_boot *boot)
{
	ioc_api = boot->api;
	ioc_reply_address = (uint16_t)ioc_reply_frame;
	ioc_bulk_address = (uint16_t)ioc_bulk_buffer;
#if KOS_IOC_PHASES
	ioc_phases_address = (uint16_t)&ioc_phases;
#endif
	/* The one bounded LINK_SYNC bring-up, here in the mode-11 window
	 * where the bootstrap interface is visible.  A down link is not a
	 * load refusal: the driver initializes, KIOC_OP_READY reports 0,
	 * and the first ordinary operation attempts the whole-link
	 * recovery.  A build whose own dependencies need the link fails
	 * its boot through those drivers. */
	ioc_link_ready = (uint8_t)(ioc_link_bringup() == KIOC_OK ? 1u : 0u);
	return 0u;
}
