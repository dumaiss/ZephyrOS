/* ZephyrOS IOC command/status driver implementation (drivers/ioc/).
 *
 * The class surface lives in include/zephyros/ioc.h.  This file owns the
 * command exchange: sequence stamping, exact common-packet construction
 * and CRC, reply capture/validation, state, and the foreground busy
 * guard.  All SIO1/B port I/O and the streaming loops live in ioc_io.asm;
 * the descriptor and module entry live in ioc_module.c.
 *
 * Wire discipline (deployed host contract level 08h, never re-negotiated):
 *
 *   A5 5A LEN_LO LEN_HI TYPE SEQ STATUS DATA... CRC_HI CRC_LO
 *
 *   LEN counts TYPE + SEQ + STATUS + DATA (3..29 on this lane); the CRC
 *   is CRC-16-CCITT (1021h, preset 0, MSB-first, no final XOR) over
 *   LEN_LO through the final DATA byte.
 *
 * The receiver boundary is established once by the LINK_SYNC bring-up
 * reply and then left alone: an ordinary transaction never issues a
 * channel reset or Enter Hunt.  Every wait is bounded and every exit
 * restores the exact entry interrupt state.
 *
 * This source is compiled into the IOC ZMOD module; the state below
 * lives in module BSS (`_DATA`) and is relocated with the module image.
 * The module reaches the kernel only through the passed `zdriver_api`.
 */

#include "zephyros/platform.h"
#include "zephyros/device.h"
#include "zephyros/ioc.h"

#include "ioc_driver.h"
#include "ioc_proto.h"

/* Largest raw wire frame: markers + LEN + fixed header + DATA + CRC. */
#define IOC_WIRE_MAX (2u + 2u + ZOS_IOC_PACKET_FIXED_LEN + \
		      ZOS_IOC_CMD_MAX_DATA + ZOS_IOC_PACKET_CRC_BYTES)

/* Reply capture outcomes reported by ioc_io.asm. */
#define IOC_RX_CAPTURED     0u
#define IOC_RX_TIMEOUT      1u
#define IOC_RX_BAD_SCAN     3u
#define IOC_RX_BAD_HEADER   5u

/* Shared block for the assembly transport routines (offsets pinned by
 * the comments in ioc_io.asm). */
struct ioc_io_block {
	uint8_t *buf;
	uint8_t len;
	uint8_t hunt;
	uint8_t resume;
};

extern uint8_t ioc_io_wait_ready(void);
extern void ioc_io_ctrl_write(uint8_t value);
extern uint8_t ioc_io_tx(struct ioc_io_block *io);
extern uint8_t ioc_io_rx(struct ioc_io_block *io);
extern uint8_t ioc_io_status_sample(void);
extern uint8_t ioc_io_irq_save_disable(void);
extern void ioc_io_irq_restore(uint8_t token);

struct ioc_state {
	uint8_t busy;      /* foreground bus/serialization guard          */
	uint8_t rx_synced; /* command-lane character boundary established */
	uint8_t seq;       /* rolling transaction sequence                */
	uint8_t irq_token; /* entry IFF across the masked transaction     */
	uint8_t packet[IOC_WIRE_MAX];
	uint8_t raw[IOC_WIRE_MAX];
};

struct ioc_state ioc_state;

static struct ioc_io_block ioc_io;

/* CRC-16-CCITT (1021h, preset 0, MSB-first, no final XOR). */
static uint16_t ioc_crc16(uint16_t crc, const uint8_t *data, uint8_t count)
{
	uint8_t i;

	while (count != 0) {
		crc ^= (uint16_t)((uint16_t)(*data++) << 8);
		for (i = 0; i < 8u; ++i) {
			if ((crc & 0x8000u) != 0u) {
				crc = (uint16_t)((uint16_t)(crc << 1) ^
						 0x1021u);
			} else {
				crc = (uint16_t)(crc << 1);
			}
		}
		--count;
	}
	return crc;
}

/* Build the exact common request packet and its wire CRC. */
static void ioc_build_request(struct ioc_state *state,
			      const struct zioc_frame *request)
{
	uint8_t *packet = state->packet;
	uint8_t len = (uint8_t)(ZOS_IOC_PACKET_FIXED_LEN + request->length);
	uint16_t crc;
	uint8_t i;

	packet[0] = ZOS_IOC_PACKET_SYNC0;
	packet[1] = ZOS_IOC_PACKET_SYNC1;
	packet[2] = len;
	packet[3] = 0;
	packet[4] = request->type;
	packet[5] = state->seq;
	packet[6] = request->status;
	for (i = 0; i < request->length; ++i) {
		packet[7u + i] = request->data[i];
	}
	crc = ioc_crc16(0u, &packet[2], (uint8_t)(2u + len));
	packet[7u + request->length] = (uint8_t)(crc >> 8);
	packet[8u + request->length] = (uint8_t)(crc & 0xFFu);
}

void ioc_hw_init(void)
{
	/* The one and only channel reset: after this, ordinary
	 * transactions never touch synchronisation state. */
	ioc_io_ctrl_write(ZOS_IOC_WR0_RESET_CHANNEL);

	/* WR1: no interrupts, no WAIT/DMA. */
	ioc_io_ctrl_write(ZOS_SIO_WR_SELECT_WR1);
	ioc_io_ctrl_write(ZOS_IOC_WR1_INT_WAIT_OFF);

	/* WR4: External Sync, x1 clock. */
	ioc_io_ctrl_write(ZOS_SIO_WR_SELECT_WR4);
	ioc_io_ctrl_write(ZOS_IOC_WR4_EXTSYNC_X1);

	/* WR7: FFh underrun fill (never the A5 packet marker). */
	ioc_io_ctrl_write(ZOS_IOC_WR_SELECT_WR7);
	ioc_io_ctrl_write(ZOS_IOC_WR7_UNDERRUN_FILL);

	/* WR3: receiver disabled; no character boundary exists yet. */
	ioc_io_ctrl_write(ZOS_SIO_WR_SELECT_WR3);
	ioc_io_ctrl_write(ZOS_IOC_WR3_RX_OFF);

	/* WR5: 8-bit TX enabled, RTS inactive. */
	ioc_io_ctrl_write(ZOS_SIO_WR_SELECT_WR5);
	ioc_io_ctrl_write(ZOS_IOC_WR5_TX_RTS_OFF);
}

int ioc_command(void *vstate, const struct zioc_frame *request,
		struct zioc_frame *reply)
{
	struct ioc_state *state = (struct ioc_state *)vstate;
	uint8_t result;
	uint8_t data_len;
	uint8_t i;
	uint16_t crc;

	if (state == (struct ioc_state *)0 ||
	    request == (const struct zioc_frame *)0 ||
	    reply == (struct zioc_frame *)0) {
		return ZIOC_EINVAL;
	}
	if (request->length > ZIOC_MAX_DATA) {
		return ZIOC_EINVAL;
	}
	if (state->busy != 0u) {
		return ZIOC_EBUSY;
	}
	state->busy = 1u;

	/* Rolling sequence: the transport owns it; zero is not special. */
	state->seq = (uint8_t)(state->seq + 1u);
	ioc_build_request(state, request);

	/* Backpressure: the MCU advertises COMMAND_READY only while it is
	 * in command-idle.  Bounded; runs before any lane traffic. */
	if (ioc_io_wait_ready() != 0u) {
		state->busy = 0u;
		return ZIOC_ETIMEDOUT;
	}

	/* Interrupts off for the bounded send/arm segment.  The exact
	 * entry state is restored on every exit below. */
	ioc_io.buf = state->packet;
	ioc_io.len = (uint8_t)(2u + 2u + ZOS_IOC_PACKET_FIXED_LEN +
			       request->length + ZOS_IOC_PACKET_CRC_BYTES);
	ioc_io.hunt = (state->rx_synced == 0u) ? 1u : 0u;
	ioc_io.resume = 0u;

	state->irq_token = ioc_io_irq_save_disable();
	if (ioc_io_tx(&ioc_io) != 0u) {
		ioc_io_irq_restore(state->irq_token);
		state->busy = 0u;
		return ZIOC_ETIMEDOUT;
	}

	/* The receiver is armed and the MCU turnaround guard is running;
	 * the long wait stays interruptible when the caller had them on. */
	ioc_io.buf = state->raw;
	ioc_io.resume = state->irq_token;
	result = ioc_io_rx(&ioc_io);
	ioc_io_irq_restore(state->irq_token);
	state->busy = 0u;

	if (result == IOC_RX_TIMEOUT) {
		return ZIOC_ETIMEDOUT;
	}
	if (result == IOC_RX_BAD_SCAN || result == IOC_RX_BAD_HEADER) {
		return ZIOC_EBADFRAME;
	}
	if (result != IOC_RX_CAPTURED) {
		return ZIOC_ETIMEDOUT;
	}

	/* The capture loop bounds LEN; repeat it here so the validation
	 * below cannot be fooled by a future assembly change. */
	if (state->raw[1] != 0u || state->raw[0] < ZOS_IOC_PACKET_FIXED_LEN ||
	    state->raw[0] > (uint8_t)(ZOS_IOC_PACKET_FIXED_LEN +
				      ZOS_IOC_CMD_MAX_DATA)) {
		return ZIOC_EBADFRAME;
	}
	data_len = (uint8_t)(state->raw[0] - ZOS_IOC_PACKET_FIXED_LEN);

	/* CRC over LEN_LO through the final DATA byte. */
	crc = ioc_crc16(0u, state->raw, (uint8_t)(2u + state->raw[0]));
	if (state->raw[2u + state->raw[0]] != (uint8_t)(crc >> 8) ||
	    state->raw[3u + state->raw[0]] != (uint8_t)(crc & 0xFFu)) {
		return ZIOC_EBADCRC;
	}

	/* The reply must echo this exchange's sequence. */
	if (state->raw[3] != state->seq) {
		return ZIOC_EBADSEQ;
	}

	/* Exchange type binding: the deployed firmware answers the request
	 * class with bit 7 set; RSP_UNKNOWN_COMMAND is its own class. */
	if (state->raw[2] != (uint8_t)(request->type | 0x80u) &&
	    state->raw[2] != IOC_RSP_UNKNOWN_CMD) {
		return ZIOC_EBADFRAME;
	}

	/* A CRC-verified reply is the proof that the persistent character
	 * boundary is established: no Enter Hunt from here on. */
	state->rx_synced = 1u;

	reply->type = state->raw[2];
	reply->status = state->raw[4];
	reply->length = data_len;
	for (i = 0; i < data_len; ++i) {
		reply->data[i] = state->raw[5u + i];
	}
	for (i = data_len; i < ZIOC_MAX_DATA; ++i) {
		reply->data[i] = 0u;
	}
	return ZIOC_OK;
}

uint16_t ioc_status(void *vstate)
{
	struct ioc_state *state = (struct ioc_state *)vstate;

	/* Sampling the doorbell while the lane is mid-transaction would
	 * report a level the current exchange owns; the guard makes it
	 * "no pending input" instead. */
	if (state == (struct ioc_state *)0 || state->busy != 0u) {
		return 0u;
	}
	if (ioc_io_status_sample() == 0u) {
		return 0u;
	}
	return ZIOC_STATUS_HID_PENDING;
}

int ioc_link_bringup(void)
{
	struct zioc_frame request;
	struct zioc_frame reply;
	uint8_t tries = ZOS_IOC_LINK_BRINGUP_TRIES;
	int result;

	request.type = ZOS_IOC_CMD_LINK_SYNC;
	request.status = 0u;
	request.length = 0u;

	while (tries != 0u) {
		--tries;
		/* Re-arm local Hunt for every attempt: the MCU clears its
		 * own established flag on each LINK_SYNC request, so each
		 * reply can carry a fresh /SYNC edge - but a reply the host
		 * never completed must not leave us believing otherwise. */
		ioc_state.rx_synced = 0u;
		result = ioc_command(&ioc_state, &request, &reply);
		if (result == ZIOC_OK &&
		    reply.type == ZOS_IOC_RSP_LINK_SYNC) {
			return ZIOC_OK;
		}
	}
	ioc_state.rx_synced = 0u;
	return ZIOC_ETIMEDOUT;
}

const struct zioc_ops ioc_ops = {
	ioc_command,
	0,
	0,
	ioc_status
};
