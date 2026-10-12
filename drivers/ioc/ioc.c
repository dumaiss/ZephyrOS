/* KonaOS IOC command + bulk transport driver implementation (drivers/ioc/).
 *
 * The class surface lives in drivers/include/ioc.h.  This file owns the
 * command exchange (sequence stamping, exact common-packet construction
 * and CRC, reply capture/validation) and the READY/DONE bulk admission
 * state.  All SIO1 port I/O and the timing-critical streaming loops live
 * in ioc_io.asm; the descriptor and module entry live in ioc_module.c.
 *
 * Wire discipline (deployed host contract level 08h, never re-negotiated):
 *
 *   A5 5A LEN_LO LEN_HI TYPE SEQ STATUS DATA... CRC_HI CRC_LO
 *
 * Both lanes use it.  LEN counts TYPE + SEQ + STATUS + DATA (3..29 on the
 * command lane, 4..515 on the bulk lane); the CRC is CRC-16-CCITT (1021h,
 * preset 0, MSB-first, no final XOR) over LEN_LO through the final DATA
 * byte.  The deployed pair runs the bulk lane with DATA CRC bypassed on
 * both ends: this driver still consumes the two trailer bytes and never
 * compares them, but marker, header, and admission validation are never
 * bypassed.
 *
 * Lane discipline:
 *
 *   - The command-lane receiver boundary is established once by the
 *     LINK_SYNC bring-up reply and then left alone: an ordinary command
 *     never issues a channel reset or Enter Hunt.
 *   - The bulk lane is armed per transfer; Enter Hunt is used only until
 *     the first verified bulk packet.  A READY reply binds the following
 *     bulk packet to its transfer id, direction, length, and TYPE/SEQ.
 *   - A write is not complete until a matching CMD_XFER_STATUS DONE
 *     record reports the same id and success; until then only that query
 *     is accepted, and a failed DONE is never reported as success.
 *   - Any bulk-lane failure clears the admission state and marks the
 *     link for whole-link recovery: the next command performs the
 *     bounded LINK_SYNC sequence before normal traffic.
 *
 * Every wait is bounded and every exit restores the exact entry interrupt
 * state.  This source is compiled into the IOC driver image; the state
 * below lives in its DRIVER_IO area.  The driver reaches the nucleus
 * only through the driver bootstrap interface.
 */

#include "ioc.h"

#include "hardware.h"

#include "ioc_driver.h"
#include "ioc_proto.h"

/* The deployed pair bypasses bulk DATA CRC on both sides.  Fail the build
 * if the paired platform headers ever disagree with this driver's mode
 * instead of silently growing a one-sided check. */
#if KOS_IOC_BULK_CRC_BYPASS != 1
#error "IOC bulk driver expects the deployed pair's CRC bypass mode"
#endif

/* Largest raw wire frame: markers + LEN + fixed header + DATA + CRC. */
#define IOC_WIRE_MAX (2u + 2u + KOS_IOC_PACKET_FIXED_LEN + \
		      KOS_IOC_CMD_MAX_DATA + KOS_IOC_PACKET_CRC_BYTES)

/* Reply capture outcomes reported by ioc_io.asm (command lane). */
#define IOC_RX_CAPTURED     0u
#define IOC_RX_TIMEOUT      1u
#define IOC_RX_BAD_SCAN     3u
#define IOC_RX_BAD_HEADER   5u

/* Bulk-lane outcomes reported by ioc_io.asm. */
#define IOC_BULK_OK         0u
#define IOC_BULK_TIMEOUT    1u
#define IOC_BULK_BAD_FRAME  3u
#define IOC_BULK_HW_ERROR   4u

/* Foreground admission phase of the bulk lifecycle. */
#define IOC_PHASE_IDLE          0u
#define IOC_PHASE_ADMITTED      1u
#define IOC_PHASE_DONE_REQUIRED 2u

/* Shared block for the assembly command transport routines (offsets
 * pinned by the comments in ioc_io.asm). */
struct ioc_io_block {
	uint8_t *buf;
	uint8_t len;
	uint8_t hunt;
	uint8_t resume;
};

/* Shared block for the assembly bulk-lane routines (offsets pinned by
 * the comments in ioc_io.asm).  A read captures the header bytes; a
 * write has them built here before the lane is entered. */
struct ioc_bulk_block {
	uint8_t *buf;      /* payload source/destination           */
	uint16_t len;      /* payload byte count, 1..512           */
	uint8_t hunt;      /* read: 1 = Enter Hunt before the scan */
	uint8_t header[5]; /* LEN_LO LEN_HI TYPE SEQ STATUS        */
};

extern uint8_t ioc_io_wait_ready(void);
extern void ioc_io_ctrl_write(uint8_t value);
extern uint8_t ioc_io_tx(struct ioc_io_block *io);
extern uint8_t ioc_io_rx(struct ioc_io_block *io);
extern void ioc_io_bulk_init(void);
extern uint8_t ioc_io_bulk_read(struct ioc_bulk_block *io);
extern uint8_t ioc_io_bulk_write(struct ioc_bulk_block *io);
extern uint8_t ioc_io_status_sample(void);
extern uint8_t ioc_io_irq_save_disable(void);
extern void ioc_io_irq_restore(uint8_t token);

struct ioc_state {
	uint8_t busy;        /* foreground bus/serialization guard        */
	uint8_t rx_synced;   /* command-lane character boundary proven    */
	uint8_t bulk_synced; /* bulk-lane character boundary proven       */
	uint8_t seq;         /* rolling transaction sequence              */
	uint8_t irq_token;   /* entry IFF across the masked transaction   */
	uint8_t phase;       /* IDLE / ADMITTED / DONE_REQUIRED           */
	uint8_t admit_dir;   /* READY direction while admitted            */
	uint8_t admit_id;    /* READY transfer id                         */
	uint16_t admit_len;  /* READY DATA length                         */
	uint8_t admit_tx_type; /* write: command TYPE the MCU armed       */
	uint8_t admit_rx_type; /* read: response TYPE the MCU armed       */
	uint8_t admit_seq;   /* exchange sequence the packet is bound to  */
	uint8_t recover;     /* whole-link recovery required before use   */
	uint8_t packet[IOC_WIRE_MAX];
	uint8_t raw[IOC_WIRE_MAX];
	struct ioc_bulk_block bulk;
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
			      const struct kioc_frame *request)
{
	uint8_t *packet = state->packet;
	uint8_t len = (uint8_t)(KOS_IOC_PACKET_FIXED_LEN + request->length);
	uint16_t crc;
	uint8_t i;

	packet[0] = KOS_IOC_PACKET_SYNC0;
	packet[1] = KOS_IOC_PACKET_SYNC1;
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

/* Is `type` a request class the pinned firmware can use to arm the bulk
 * lane?  READY has no independent tag on the wire, and immutable
 * successful non-bulk replies are also eight bytes long (an HID fetch
 * whose `queued`/`dropped` header looks like id/direction/length, or a
 * PROFILE bulk-tx page).  Without this gate an ordinary success would
 * strand the phase as ADMITTED and reject real commands as busy. */
static uint8_t ioc_bulk_arming_request(uint8_t type)
{
	switch (type) {
	case IOC_CMD_BULK_TEST:
	case IOC_CMD_SD_READ_BULK:
	case IOC_CMD_SD_WRITE_BULK:
	case IOC_CMD_SD_READ_REC:
	case IOC_CMD_SD_WRITE_REC:
	case IOC_CMD_SD_READ_BLOCK:
	case IOC_CMD_FS_READ:
	case IOC_CMD_FS_WRITE:
	case IOC_CMD_FS2_READ:
	case IOC_CMD_FS2_WRITE:
		return 1u;
	default:
		return 0u;
	}
}

/* Recognize the exact READY payload shape and bind the following bulk
 * phase to it.  `type`/`seq` were already verified by the caller; this
 * only adds the bulk-specific shape checks.  Returns 1 when a bulk phase
 * was admitted. */
static uint8_t ioc_ready_admit(struct ioc_state *state,
			       const struct kioc_frame *request,
			       const struct kioc_frame *reply)
{
	uint8_t id = reply->data[IOC_OFF_READY_XFER_ID];
	uint8_t dir = reply->data[IOC_OFF_READY_DIRECTION];
	uint16_t len = (uint16_t)reply->data[IOC_OFF_READY_LEN_LO] |
		       ((uint16_t)reply->data[IOC_OFF_READY_LEN_HI] << 8);

	/* Nonzero transfer id, known direction, 1..512 bytes.  Anything
	 * else is an ordinary successful reply that happens to be 8
	 * bytes long. */
	if (id == 0u || dir > IOC_BULK_DIR_Z80_TO_MCU ||
	    len == 0u || len > KIOC_BULK_MAX) {
		return 0u;
	}

	state->phase = IOC_PHASE_ADMITTED;
	state->admit_id = id;
	state->admit_dir = dir;
	state->admit_len = len;
	state->admit_seq = state->seq;
	if (dir == IOC_BULK_DIR_Z80_TO_MCU) {
		/* The MCU armed its receiver with the request class. */
		state->admit_tx_type = request->type;
	} else {
		/* The MCU sends the response class it armed with. */
		state->admit_rx_type = reply->type;
	}
	return 1u;
}

/* One command exchange, including the READY/DONE admission state.
 * ioc_command() adds the whole-link recovery gate. */
static int ioc_command_inner(struct ioc_state *state,
			     const struct kioc_frame *request,
			     struct kioc_frame *reply)
{
	uint8_t result;
	uint8_t data_len;
	uint8_t i;
	uint16_t crc;

	if (request->length > KIOC_MAX_DATA) {
		return KIOC_EINVAL;
	}
	if (state->busy != 0u) {
		return KIOC_EBUSY;
	}
	/* A READY admits exactly one bulk phase; no command may displace
	 * it.  A write awaiting its DONE record accepts only that query. */
	if (state->phase == IOC_PHASE_ADMITTED) {
		return KIOC_EBUSY;
	}
	if (state->phase == IOC_PHASE_DONE_REQUIRED &&
	    request->type != IOC_CMD_XFER_STATUS) {
		return KIOC_EBUSY;
	}
	state->busy = 1u;

	/* Rolling sequence: the transport owns it; zero is not special. */
	state->seq = (uint8_t)(state->seq + 1u);
	ioc_build_request(state, request);

	/* Backpressure: the MCU advertises COMMAND_READY only while it is
	 * in command-idle.  Bounded; runs before any lane traffic. */
	if (ioc_io_wait_ready() != 0u) {
		state->busy = 0u;
		return KIOC_ETIMEDOUT;
	}

	/* Interrupts off for the bounded send/arm segment.  The exact
	 * entry state is restored on every exit below. */
	ioc_io.buf = state->packet;
	ioc_io.len = (uint8_t)(2u + 2u + KOS_IOC_PACKET_FIXED_LEN +
			       request->length + KOS_IOC_PACKET_CRC_BYTES);
	ioc_io.hunt = (state->rx_synced == 0u) ? 1u : 0u;
	ioc_io.resume = 0u;

	state->irq_token = ioc_io_irq_save_disable();
	if (ioc_io_tx(&ioc_io) != 0u) {
		ioc_io_irq_restore(state->irq_token);
		state->busy = 0u;
		return KIOC_ETIMEDOUT;
	}

	/* The receiver is armed and the MCU turnaround guard is running;
	 * the long wait stays interruptible when the caller had them on. */
	ioc_io.buf = state->raw;
	ioc_io.resume = state->irq_token;
	result = ioc_io_rx(&ioc_io);
	ioc_io_irq_restore(state->irq_token);
	state->busy = 0u;

	if (result == IOC_RX_TIMEOUT) {
		return KIOC_ETIMEDOUT;
	}
	if (result == IOC_RX_BAD_SCAN || result == IOC_RX_BAD_HEADER) {
		return KIOC_EBADFRAME;
	}
	if (result != IOC_RX_CAPTURED) {
		return KIOC_ETIMEDOUT;
	}

	/* The capture loop bounds LEN; repeat it here so the validation
	 * below cannot be fooled by a future assembly change. */
	if (state->raw[1] != 0u || state->raw[0] < KOS_IOC_PACKET_FIXED_LEN ||
	    state->raw[0] > (uint8_t)(KOS_IOC_PACKET_FIXED_LEN +
				      KOS_IOC_CMD_MAX_DATA)) {
		return KIOC_EBADFRAME;
	}
	data_len = (uint8_t)(state->raw[0] - KOS_IOC_PACKET_FIXED_LEN);

	/* CRC over LEN_LO through the final DATA byte. */
	crc = ioc_crc16(0u, state->raw, (uint8_t)(2u + state->raw[0]));
	if (state->raw[2u + state->raw[0]] != (uint8_t)(crc >> 8) ||
	    state->raw[3u + state->raw[0]] != (uint8_t)(crc & 0xFFu)) {
		return KIOC_EBADCRC;
	}

	/* The reply must echo this exchange's sequence. */
	if (state->raw[3] != state->seq) {
		return KIOC_EBADSEQ;
	}

	/* Exchange type binding: the deployed firmware answers the request
	 * class with bit 7 set; RSP_UNKNOWN_COMMAND is its own class. */
	if (state->raw[2] != (uint8_t)(request->type | 0x80u) &&
	    state->raw[2] != IOC_RSP_UNKNOWN_CMD) {
		return KIOC_EBADFRAME;
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
	for (i = data_len; i < KIOC_MAX_DATA; ++i) {
		reply->data[i] = 0u;
	}

	/* DONE interlock: a completed write is complete only when the
	 * matching DONE record reports the same transfer id and success.
	 * A mismatch is an error/recovery state and never success. */
	if (state->phase == IOC_PHASE_DONE_REQUIRED) {
		state->phase = IOC_PHASE_IDLE;
		if (reply->type != IOC_RSP_XFER_STATUS ||
		    reply->length < IOC_DONE_MIN_DATA) {
			state->recover = 1u;
			return KIOC_EBADFRAME;
		}
		if (reply->data[IOC_OFF_DONE_XFER_ID] != state->admit_id) {
			state->recover = 1u;
			return KIOC_EBADFRAME;
		}
		if (reply->data[IOC_OFF_DONE_STATUS] != IOC_STATUS_OK) {
			state->recover = 1u;
			return KIOC_EIO;
		}
		return KIOC_OK;
	}

	/* Exact READY shape from a request class that can arm the bulk lane?
	 * Then the bulk phase is admitted.  The class gate is what keeps
	 * every ordinary eight-byte success (HID fetch, PROFILE page) from
	 * ever changing the bulk phase. */
	if (ioc_bulk_arming_request(request->type) != 0u &&
	    reply->status == 0u && reply->length == IOC_READY_PAYLOAD_LEN) {
		(void)ioc_ready_admit(state, request, reply);
	}
	return KIOC_OK;
}

int ioc_command(void *vstate, const struct kioc_frame *request,
		struct kioc_frame *reply)
{
	struct ioc_state *state = (struct ioc_state *)vstate;

	if (state == (struct ioc_state *)0 ||
	    request == (const struct kioc_frame *)0 ||
	    reply == (struct kioc_frame *)0) {
		return KIOC_EINVAL;
	}
	if (request->length > KIOC_MAX_DATA) {
		return KIOC_EINVAL;
	}
	if (state->busy != 0u) {
		return KIOC_EBUSY;
	}

	/* A failed bulk phase leaves the bulk lane untrusted; the next
	 * operation re-establishes both lanes together before any normal
	 * traffic.  Malformed arguments never trigger I/O. */
	if (state->recover != 0u) {
		if (ioc_link_bringup() != KIOC_OK) {
			return KIOC_ETIMEDOUT;
		}
	}
	return ioc_command_inner(state, request, reply);
}

int ioc_bulk_read(void *vstate, void *dst, uint16_t length)
{
	struct ioc_state *state = (struct ioc_state *)vstate;
	struct ioc_bulk_block *bulk;
	uint16_t wire_len;
	uint8_t result;

	if (state == (struct ioc_state *)0 || dst == (void *)0) {
		return KIOC_EINVAL;
	}
	if (length == 0u || length > KIOC_BULK_MAX) {
		return KIOC_EINVAL;
	}
	if (state->busy != 0u) {
		return KIOC_EBUSY;
	}
	/* Invalid sequencing fails before any lane I/O: a bulk phase must
	 * have been admitted by a READY, with the same direction and the
	 * exact length. */
	if (state->phase != IOC_PHASE_ADMITTED ||
	    state->admit_dir != IOC_BULK_DIR_MCU_TO_Z80 ||
	    state->admit_len != length) {
		return KIOC_EBUSY;
	}
	state->busy = 1u;

	bulk = &state->bulk;
	bulk->buf = (uint8_t *)dst;
	bulk->len = length;
	/* Enter Hunt only until the first verified bulk packet. */
	bulk->hunt = (state->bulk_synced == 0u) ? 1u : 0u;

	result = ioc_io_bulk_read(bulk);

	state->busy = 0u;
	state->phase = IOC_PHASE_IDLE;

	if (result != IOC_BULK_OK) {
		state->recover = 1u;
		if (result == IOC_BULK_TIMEOUT) {
			return KIOC_ETIMEDOUT;
		}
		if (result == IOC_BULK_HW_ERROR) {
			return KIOC_EIO;
		}
		return KIOC_EBADFRAME;
	}

	/* The captured header must be exactly the packet the READY
	 * authorized: wire length, TYPE, sequence, and a zero STATUS.
	 * Validation happens after the stream is safely in RAM. */
	wire_len = (uint16_t)(length + KOS_IOC_PACKET_FIXED_LEN);
	if (bulk->header[0] != (uint8_t)wire_len ||
	    bulk->header[1] != (uint8_t)(wire_len >> 8) ||
	    bulk->header[2] != state->admit_rx_type ||
	    bulk->header[3] != state->admit_seq ||
	    bulk->header[4] != 0u) {
		state->recover = 1u;
		return KIOC_EBADFRAME;
	}

	/* A fully validated bulk read is the only proof the bulk-lane
	 * character boundary is real; from here the lane is left alone. */
	state->bulk_synced = 1u;
	return KIOC_OK;
}

int ioc_bulk_write(void *vstate, const void *src, uint16_t length)
{
	struct ioc_state *state = (struct ioc_state *)vstate;
	struct ioc_bulk_block *bulk;
	uint16_t wire_len;
	uint8_t result;

	if (state == (struct ioc_state *)0 || src == (const void *)0) {
		return KIOC_EINVAL;
	}
	if (length == 0u || length > KIOC_BULK_MAX) {
		return KIOC_EINVAL;
	}
	if (state->busy != 0u) {
		return KIOC_EBUSY;
	}
	if (state->phase != IOC_PHASE_ADMITTED ||
	    state->admit_dir != IOC_BULK_DIR_Z80_TO_MCU ||
	    state->admit_len != length) {
		return KIOC_EBUSY;
	}
	state->busy = 1u;

	/* The header is built before the lane is entered: the OUTI-shaped
	 * stream cannot afford arithmetic between bytes, and the packet is
	 * bound to the READY's TYPE and sequence. */
	bulk = &state->bulk;
	bulk->buf = (uint8_t *)src;
	bulk->len = length;
	bulk->hunt = 0u;
	wire_len = (uint16_t)(length + KOS_IOC_PACKET_FIXED_LEN);
	bulk->header[0] = (uint8_t)wire_len;
	bulk->header[1] = (uint8_t)(wire_len >> 8);
	bulk->header[2] = state->admit_tx_type;
	bulk->header[3] = state->admit_seq;
	bulk->header[4] = 0u;

	result = ioc_io_bulk_write(bulk);

	state->busy = 0u;
	state->phase = IOC_PHASE_IDLE;

	if (result != IOC_BULK_OK) {
		state->recover = 1u;
		if (result == IOC_BULK_TIMEOUT) {
			return KIOC_ETIMEDOUT;
		}
		return KIOC_EIO;
	}

	/* The lane carried the bytes, but bytes reaching the controller is
	 * not proof they were stored: only the matching DONE record can
	 * report the write complete, and until it arrives only the DONE
	 * query is accepted. */
	state->phase = IOC_PHASE_DONE_REQUIRED;
	return KIOC_OK;
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
	return KIOC_STATUS_HID_PENDING;
}

int ioc_link_bringup(void)
{
	struct kioc_frame request;
	struct kioc_frame reply;
	uint8_t tries = KOS_IOC_LINK_BRINGUP_TRIES;
	int result;

	request.type = KOS_IOC_CMD_LINK_SYNC;
	request.status = 0u;
	request.length = 0u;

	while (tries != 0u) {
		--tries;
		/* Re-arm BOTH lanes for every attempt: the MCU's LINK_SYNC
		 * handler resynchronises command and bulk together, so the
		 * two ends must agree on which boundaries are valid.  A
		 * reply the host never completed must not leave us
		 * believing otherwise. */
		ioc_state.rx_synced = 0u;
		ioc_state.bulk_synced = 0u;
		ioc_state.phase = IOC_PHASE_IDLE;
		ioc_state.recover = 0u;
		/* The recovery exchange itself must bypass the recovery
		 * gate: call the inner exchange, not ioc_command(). */
		result = ioc_command_inner(&ioc_state, &request, &reply);
		if (result == KIOC_OK &&
		    reply.type == KOS_IOC_RSP_LINK_SYNC) {
			return KIOC_OK;
		}
	}
	ioc_state.rx_synced = 0u;
	ioc_state.bulk_synced = 0u;
	ioc_state.phase = IOC_PHASE_IDLE;
	ioc_state.recover = 1u;
	return KIOC_ETIMEDOUT;
}
