/* KonaOS IOC command + bulk transport driver implementation (drivers/ioc/).
 *
 * The class surface lives in drivers/include/ioc.h.  This file owns the
 * command exchange's state (the busy guard, the rolling sequence, the
 * lane sync flags) and the READY/DONE bulk admission state.  The wire
 * work - the exact common packet and its CRC, the send, the reply's
 * capture and validation, every SIO1 port access and the timing-critical
 * streaming loops - lives in ioc_io.asm; the endpoint dispatch and init
 * live in ioc_proto.c.
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

/* The read-path phase instrument: everything below is compiled only in a
 * KOS_IOC_PHASES build (the measurement image), and the header's own doc
 * note owns what it does and when the counting runs. */
#include "ioc_phases.h"

/* The deployed pair bypasses bulk DATA CRC on both sides.  Fail the build
 * if the paired platform headers ever disagree with this driver's mode
 * instead of silently growing a one-sided check. */
#if KOS_IOC_BULK_CRC_BYPASS != 1
#error "IOC bulk driver expects the deployed pair's CRC bypass mode"
#endif

/* Command exchange outcomes reported by ioc_io.asm (ioc_io_exchange). */
#define IOC_X_OK            0u
#define IOC_X_BAD_FRAME     4u
#define IOC_X_BAD_CRC       5u
#define IOC_X_BAD_SEQ       6u

/* Bulk-lane outcomes reported by ioc_io.asm. */
#define IOC_BULK_OK         0u
#define IOC_BULK_TIMEOUT    1u
#define IOC_BULK_BAD_FRAME  3u
#define IOC_BULK_HW_ERROR   4u

/* Foreground admission phase of the bulk lifecycle. */
#define IOC_PHASE_IDLE          0u
#define IOC_PHASE_ADMITTED      1u
#define IOC_PHASE_DONE_REQUIRED 2u

/* The command exchange's block (offsets pinned by the comment in
 * ioc_io.asm). */
struct ioc_exchange {
	uint8_t type;
	uint8_t status;
	uint8_t length;
	const uint8_t *data;
	struct kioc_frame *reply;
	uint8_t seq;
	uint8_t hunt;
	uint8_t unknown;
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

extern void ioc_io_ctrl_write(uint8_t value);
extern uint8_t ioc_io_exchange(struct ioc_exchange *x);
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
	uint8_t phase;       /* IDLE / ADMITTED / DONE_REQUIRED           */
	uint8_t admit_dir;   /* READY direction while admitted            */
	uint8_t admit_id;    /* READY transfer id                         */
	uint16_t admit_len;  /* READY DATA length                         */
	uint8_t admit_tx_type; /* write: command TYPE the MCU armed       */
	uint8_t admit_rx_type; /* read: response TYPE the MCU armed       */
	uint8_t admit_seq;   /* exchange sequence the packet is bound to  */
	uint8_t recover;     /* whole-link recovery required before use   */
	struct ioc_bulk_block bulk;
};

struct ioc_state ioc_state;

/* ioc_io_run_read (ioc_io.asm) reads these fields by offset. */
extern uint8_t ioc_io_run_read(struct ioc_state *state);
#if defined(__SDCC)
_Static_assert(__builtin_offsetof(struct ioc_state, bulk_synced) == 2,
	       "ioc_io.asm IOC_ST_BULK_SYNCED");
_Static_assert(__builtin_offsetof(struct ioc_state, admit_rx_type) == 10,
	       "ioc_io.asm IOC_ST_RX_TYPE");
_Static_assert(__builtin_offsetof(struct ioc_state, admit_seq) == 11,
	       "ioc_io.asm IOC_ST_ADMIT_SEQ");
_Static_assert(__builtin_offsetof(struct ioc_state, bulk) == 13,
	       "ioc_io.asm IOC_ST_BULK");
_Static_assert(__builtin_offsetof(struct ioc_bulk_block, len) == 2 &&
	       __builtin_offsetof(struct ioc_bulk_block, hunt) == 4 &&
	       __builtin_offsetof(struct ioc_bulk_block, header) == 5,
	       "ioc_io.asm IOC_ST_LEN/HUNT/HEADER");
_Static_assert(KIOC_BULK_MAX == 512u && IOC_BULK_BAD_FRAME == 3u,
	       "ioc_io.asm IOC_RUN_PACKET/IOC_RUN_BAD_FRAME");
#endif /* __SDCC */

#if KOS_IOC_PHASES
/* The stamps this driver's exchange contributed to the last block read
 * (abi/kos/protocol.h KFILE_OP_PHASES).  The IOC-FAT driver reads the
 * table from its own bank-6 mapping through the reply handshake this
 * driver publishes, exactly as it reads the staged reply frame. */
struct kfile_phases ioc_phases;

/* The bulk receive's three stamps are inline in ioc_io.asm, and they are
 * deliberately not the retaking stamp the phases outside the receive use:
 * the receiver's three-byte FIFO is the whole budget there - the lane is
 * clocked at about one byte per 250 T-states, so about 750 T-states in
 * all - and the call into C that ran the retaking stamp cost the receiver
 * service gaps of 741 and 878 T-states at the two sites of the first
 * board run's failing transfer, which lost the byte the controller was
 * sending (every bulk transfer ended in EIO).  Two raw counter reads cost
 * 88 T-states instead.  What the assembly therefore does not do is mark
 * its own work valid: the bits are set here, outside the masked window,
 * and only for a transfer whose header validated, so a failed or short
 * transfer reports its partial stamps as nothing rather than as a
 * measurement.  A raw pair may be torn (the two reads are 15 T-states
 * apart and either counter can reload between them); the fetching program
 * rejects such a pair instead of resolving it. */
static void ioc_phases_bulk_done(void)
{
	ioc_phases.valid |= (uint16_t)((1u << KFILE_PHASE_ADMIT) |
				       (1u << KFILE_PHASE_FIRST) |
				       (1u << KFILE_PHASE_LAST));
}
#endif

static struct ioc_exchange ioc_x;


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
	case IOC_CMD_FS2_READ_RUN:
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
			       const struct ioc_request *request,
			       const struct kioc_frame *reply)
{
	uint8_t id = reply->data[IOC_OFF_READY_XFER_ID];
	uint8_t dir = reply->data[IOC_OFF_READY_DIRECTION];
	uint16_t len = (uint16_t)reply->data[IOC_OFF_READY_LEN_LO] |
		       ((uint16_t)reply->data[IOC_OFF_READY_LEN_HI] << 8);

	/* Nonzero transfer id, known direction, 1..512 bytes (a run:
	 * up to KIOC_RUN_MAX, in 512-byte packets).  Anything else is an
	 * ordinary successful reply that happens to be 8 bytes long. */
	if (id == 0u || dir > IOC_BULK_DIR_Z80_TO_MCU || len == 0u ||
	    len > ((request->type == IOC_CMD_FS2_READ_RUN) ? KIOC_RUN_MAX
							  : KIOC_BULK_MAX)) {
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
			     const struct ioc_request *request,
			     struct kioc_frame *reply)
{
	uint8_t result;

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
#if KOS_IOC_PHASES
	/* Stamped before the exchange, which builds the request, waits for
	 * the ready line and sends it masked: the pre-mask stamp stays
	 * outside the window that loses ticks. */
	ioc_phases_stamp(&ioc_phases, KFILE_PHASE_SENT);
#endif

	/* The whole wire exchange is ioc_io_exchange: build, COMMAND_READY,
	 * the masked send, the reply and its validation (length, CRC,
	 * sequence, type binding), delivered into `reply`. */
	ioc_x.type = request->type;
	ioc_x.status = request->status;
	ioc_x.length = request->length;
	ioc_x.data = request->data;
	ioc_x.reply = reply;
	ioc_x.seq = state->seq;
	ioc_x.hunt = (state->rx_synced == 0u) ? 1u : 0u;
	ioc_x.unknown = IOC_RSP_UNKNOWN_CMD;
	result = ioc_io_exchange(&ioc_x);
	state->busy = 0u;
	if (result == IOC_X_BAD_FRAME) {
		return KIOC_EBADFRAME;
	}
	if (result == IOC_X_BAD_CRC) {
		return KIOC_EBADCRC;
	}
	if (result == IOC_X_BAD_SEQ) {
		return KIOC_EBADSEQ;
	}
	if (result != IOC_X_OK) {
		return KIOC_ETIMEDOUT;
	}

#if KOS_IOC_PHASES
	/* The reply is received and validated (length, CRC, sequence and
	 * type): the last step the instrument stamps on this lane. */
	ioc_phases_stamp(&ioc_phases, KFILE_PHASE_REPLY);
#endif

	/* A CRC-verified reply is the proof that the persistent character
	 * boundary is established: no Enter Hunt from here on. */
	state->rx_synced = 1u;

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

int ioc_command(void *vstate, const struct ioc_request *request,
		struct kioc_frame *reply)
{
	struct ioc_state *state = (struct ioc_state *)vstate;

	if (state == (struct ioc_state *)0 ||
	    request == (const struct ioc_request *)0 ||
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
	uint8_t result;

	if (state == (struct ioc_state *)0 || dst == (void *)0) {
		return KIOC_EINVAL;
	}
	if (length == 0u || length > KIOC_RUN_MAX) {
		return KIOC_EINVAL;
	}
	if (state->busy != 0u) {
		return KIOC_EBUSY;
	}
	/* Invalid sequencing fails before any lane I/O: a bulk phase must
	 * have been admitted by a READY, with the same direction and the
	 * exact length (only a run's READY admits more than one packet). */
	if (state->phase != IOC_PHASE_ADMITTED ||
	    state->admit_dir != IOC_BULK_DIR_MCU_TO_Z80 ||
	    state->admit_len != length) {
		return KIOC_EBUSY;
	}
	state->busy = 1u;

	/* The packets, each header validated against the READY after its
	 * stream is safely in RAM (ioc_io_run_read). */
	state->bulk.buf = (uint8_t *)dst;
	state->bulk.len = length;
	result = ioc_io_run_read(state);

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
#if KOS_IOC_PHASES
	ioc_phases_bulk_done();
#endif
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

uint16_t ioc_admitted_read(void *vstate)
{
	struct ioc_state *state = (struct ioc_state *)vstate;

	if (state->phase != IOC_PHASE_ADMITTED ||
	    state->admit_dir != IOC_BULK_DIR_MCU_TO_Z80) {
		return 0u;
	}
	return state->admit_len;
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
	struct ioc_request request;
	struct kioc_frame reply;
	uint8_t tries = KOS_IOC_LINK_BRINGUP_TRIES;
	int result;

	request.type = KOS_IOC_CMD_LINK_SYNC;
	request.status = 0u;
	request.length = 0u;
	request.data = (const uint8_t *)0;

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
