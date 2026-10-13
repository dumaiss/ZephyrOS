/* KonaOS HID keyboard/text driver implementation (drivers/hid/).
 *
 * The HID driver as a bank-6 driver image: the 16-byte local ring,
 * the fetch-on-doorbell engine, and the stuck-doorbell penalty behind the
 * HID protocol of abi/kos/protocol.h (class KOS_CLASS_HID, instance 0,
 * version KOS_PROTOCOL_VERSION).
 *
 * Input path (the deployed MCU is the authority, the firmware is never
 * modified):
 *
 *   - the MCU translates a USB boot keyboard to terminal bytes and keeps
 *     its own 128-byte queue; `/CTSB` is asserted while that queue holds
 *     anything, and the IOC class maps the level to
 *     KIOC_STATUS_HID_PENDING.  HID samples that semantic bit only; it
 *     never touches SIO1, RR0, CTS or any port.
 *   - CMD_HID_INPUT carries one request byte: the maximum number of
 *     bytes wanted.  The reply DATA is `queued`, `dropped`, then up to
 *     24 translated bytes.  The controller DEQUEUES what it sends, so a
 *     fetch asks for exactly the free space of the local ring: asking
 *     for more would lose bytes that do not fit locally.
 *   - every reply is validated (transport success, RSP_HID_INPUT,
 *     controller status, 2..26 DATA length, count within both the
 *     request and the ring capacity) before a single byte is committed.
 *     A malformed reply leaves the ring unchanged and is treated as an
 *     empty fetch.
 *
 * Failure policy -- a broken IOC link must not wedge the console:
 *
 *   - an asserted semantic doorbell followed by a failed or empty fetch
 *     imposes a 64-call penalty during which the doorbell is ignored, so
 *     a dead controller cannot put a full IOC timeout on every console
 *     poll.  Successful data or a released doorbell clears the penalty
 *     immediately.
 *   - transport failures are reported as "no input" (0 / EAGAIN), never
 *     as a poll error, and a link that is down at init keeps the engine
 *     registered: the first ordinary operation retries the whole-link
 *     recovery.  The console is chosen by the build and does not probe
 *     KHID_OP_READY, so a down link is a recoverable condition, not a
 *     boot decision.
 *
 * Translation is the MCU's: this driver copies bytes verbatim, including
 * NUL, control bytes and multi-byte VT100 key sequences.
 *
 * The whole driver is hot: the engine is reachable after boot and its
 * bring-up is dependency discovery, which needs the bootstrap interface
 * (mode-11 window) and therefore cannot live in the boot tier.  The
 * driver declares its IOC dependency in the image metadata, so a
 * missing IOC image is a loader boot error before any HID byte runs.
 */

#include "kos/errno.h"
#include "driver.h"

#include "kos/protocol.h"

#include "ioc_protocol.h"

#include "hid_driver.h"

struct khid_state hid_state;

static const struct driver_api *hid_api;

/* One command exchange and one reply, through the IOC driver endpoint.
 * The transport owns framing, CRC and the rolling sequence. */
static uint8_t hid_fetch(struct khid_state *state)
{
	uint8_t free_space = (uint8_t)(KHID_RING_SIZE - state->count);
	uint8_t payload;
	uint16_t insert;
	uint16_t room;
	uint16_t first;
	struct kos_ipc_frame frame;
	int16_t result;

	if (free_space == 0u) {
		return 0u;
	}

	/* Ask for exactly the local free space, never above the protocol's
	 * per-reply maximum. */
	if (free_space > IOC_HID_INPUT_MAX_DATA) {
		free_space = IOC_HID_INPUT_MAX_DATA;
	}
	frame.op = KIOC_OP_COMMAND;
	frame.flags = 0u;
	frame.desc = KOS_IPC_DESC_NONE;
	frame.arg0 = IOC_CMD_HID_INPUT;
	frame.arg1 = 1u;
	frame.payload[0] = free_space;
	result = hid_api->call(state->ioc_handle, KIOC_OP_COMMAND, &frame);
	if (result != 0) {
		return 0u;
	}
	if (frame.arg0 != IOC_RSP_HID_INPUT) {
		return 0u;
	}
	if (frame.payload[2] != 0u) {
		return 0u;
	}
	/* DATA = queued, dropped, then the translated bytes. */
	if (frame.arg1 < IOC_HID_INPUT_META_LEN ||
	    frame.arg1 > (uint8_t)(IOC_HID_INPUT_META_LEN +
				   IOC_HID_INPUT_MAX_DATA)) {
		return 0u;
	}
	payload = (uint8_t)(frame.arg1 - IOC_HID_INPUT_META_LEN);

	/* The MCU dequeued what it sent: anything beyond what was asked
	 * for, or beyond the ring's actual space, cannot be committed. */
	if (payload > free_space) {
		return 0u;
	}
	if (payload > (uint8_t)(KHID_RING_SIZE - state->count)) {
		return 0u;
	}
	/* The translated bytes follow the two DATA head bytes in the reply
	 * frame the IOC driver staged for this call, so the commit is one
	 * same-domain copy per ring run - no intermediate frame, and no
	 * byte-at-a-time index (the container format rejects the
	 * byte-fragmented address form a byte cursor compiles to). */
	if (payload != 0u) {
		uint8_t *ring = state->ring;
		uint16_t source = (uint16_t)((uint16_t)(frame.payload[0] |
							((uint16_t)
							 frame.payload[1]
							 << 8)) +
					     3u +
					     IOC_HID_INPUT_META_LEN);

		insert = state->head;
		room = (uint16_t)(KHID_RING_SIZE - insert);
		first = payload;
		if (first > room) {
			first = room;
		}
		if (hid_api->move(source, (uint16_t)(ring + insert),
				  first) < 0) {
			return 0u;
		}
		if (first < payload &&
		    hid_api->move((uint16_t)(source + first),
				  (uint16_t)ring,
				  (uint16_t)(payload - first)) < 0) {
			return 0u;
		}
	}
	state->head = (uint8_t)((state->head + payload) &
				(KHID_RING_SIZE - 1u));
	state->count = (uint8_t)(state->count + payload);
	return payload;
}

/* The fetch-on-doorbell engine shared by poll/getc/read_text.  Returns 1
 * when a byte is buffered after the call, 0 otherwise.  Only here (and
 * only while the semantic doorbell is asserted) is any IOC command
 * issued. */
static uint8_t hid_driver(struct khid_state *state)
{
	struct kos_ipc_frame frame;
	int16_t status;

	if (state->count != 0u) {
		return 1u;
	}
	if (state->ioc_handle == 0u) {
		return 0u;
	}

	frame.op = KIOC_OP_STATUS;
	frame.flags = 0u;
	frame.desc = KOS_IPC_DESC_NONE;
	frame.arg0 = 0u;
	frame.arg1 = 0u;
	status = hid_api->call(state->ioc_handle, KIOC_OP_STATUS, &frame);
	if (status < 0 ||
	    ((uint16_t)status & KIOC_STATUS_HID_PENDING) == 0u) {
		/* A line that can still fall is not stuck: the released
		 * doorbell clears the penalty at no IOC cost. */
		state->penalty = 0u;
		return 0u;
	}
	if (state->penalty != 0u) {
		--state->penalty;
		return 0u;
	}
	if (hid_fetch(state) != 0u) {
		state->penalty = 0u;
		return 1u;
	}

	/* Asserted doorbell, nothing delivered: transport failure or a
	 * controller stuck with the line asserted.  Ignore the doorbell
	 * for a bounded number of calls instead of paying an IOC round
	 * trip on every console poll. */
	state->penalty = KHID_STUCK_PENALTY;
	return 0u;
}

int hid_read_text(void *vstate, uint8_t *dst, uint16_t max_len)
{
	struct khid_state *state = (struct khid_state *)vstate;
	uint16_t done = 0u;

	if (state == (struct khid_state *)0) {
		return (-KOS_ENODEV);
	}
	if (dst == (uint8_t *)0 || max_len == 0u) {
		return (-KOS_EINVAL);
	}
	if (state->count == 0u) {
		(void)hid_driver(state);
	}
	while (done < max_len && state->count != 0u) {
		uint16_t tail = state->tail;

		dst[done] = state->ring[tail];
		state->tail = (uint8_t)((tail + 1u) & (KHID_RING_SIZE - 1u));
		state->count = (uint8_t)(state->count - 1u);
		++done;
	}
	return (int)done;
}

int hid_poll(void *vstate)
{
	struct khid_state *state = (struct khid_state *)vstate;

	if (state == (struct khid_state *)0) {
		return (-KOS_ENODEV);
	}
	return hid_driver(state) != 0u ? 1 : 0;
}

int hid_getc(void *vstate)
{
	struct khid_state *state = (struct khid_state *)vstate;
	uint16_t tail;
	uint8_t byte;

	if (state == (struct khid_state *)0) {
		return (-KOS_ENODEV);
	}
	if (state->count == 0u) {
		(void)hid_driver(state);
	}
	if (state->count == 0u) {
		return (-KOS_EAGAIN);
	}
	tail = state->tail;
	byte = state->ring[tail];
	state->tail = (uint8_t)((tail + 1u) & (KHID_RING_SIZE - 1u));
	state->count = (uint8_t)(state->count - 1u);
	return (int)byte;
}

/* READ_TEXT: drain the ring into the caller's descriptor in bounded
 * chunks through the driver I/O staging block. */
static int16_t hid_driver_read(struct kos_ipc_frame *frame)
{
	uint16_t offset = frame->arg0;
	uint16_t length = frame->arg1;
	uint16_t done = 0u;

	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);
		int result;

		if (chunk > KHID_READ_BATCH) {
			chunk = KHID_READ_BATCH;
		}
		result = hid_read_text(&hid_state, hid_state.read_buffer,
				       chunk);
		if (result < 0) {
			return done != 0u ? (int16_t)done : (int16_t)result;
		}
		if (result == 0) {
			break;
		}
		if (hid_api->copy_out(frame->desc, (uint16_t)(offset + done),
				      hid_state.read_buffer_address,
				      (uint16_t)result) < 0) {
			return (int16_t)-KOS_EINVAL;
		}
		done = (uint16_t)(done + (uint16_t)result);
	}
	return (int16_t)done;
}

int16_t hid_driver_dispatch(struct kos_ipc_frame *frame)
{
	switch (frame->op) {
	case KHID_OP_READ_TEXT:
		return hid_driver_read(frame);
	case KHID_OP_POLL:
		return (int16_t)hid_poll(&hid_state);
	case KHID_OP_GETC:
		return (int16_t)hid_getc(&hid_state);
	case KHID_OP_READY:
		/* Ready once the dependency is present and its link is up:
		 * the console composes its input from this probe. */
		return (int16_t)(hid_state.ioc_handle != 0u ? 1 : 0);
	default:
		return (int16_t)-KOS_EINVAL;
	}
}

uint8_t hid_driver_init(const struct driver_boot *boot)
{
	uint8_t handle;
	struct kos_ipc_frame frame;

	hid_api = boot->api;
	hid_state.read_buffer_address = (uint16_t)hid_state.read_buffer;
	hid_state.head = 0u;
	hid_state.tail = 0u;
	hid_state.count = 0u;
	hid_state.penalty = 0u;

	/* The one dependency (the IOC transport, manifest position 0): the
	 * host resolved it to the endpoint number; the driver never looks a
	 * peer up at run time.  The manifest declares it required, so the
	 * packer refused a build without it; a zero here is a malformed
	 * bundle and fails the init (the loader fails the boot, 5.3). */
	handle = boot->deps[HID_DEP_IOC];
	hid_state.ioc_handle = handle;
	if (handle == 0u) {
		return 1u;
	}

	/* Probe the link once: a down link keeps the ring empty and the
	 * engine registered; the first ordinary operation retries the
	 * whole-link recovery.  KHID_OP_READY reports the link state. */
	frame.op = KIOC_OP_READY;
	frame.flags = 0u;
	frame.desc = KOS_IPC_DESC_NONE;
	frame.arg0 = 0u;
	frame.arg1 = 0u;
	if (hid_api->call(handle, KIOC_OP_READY, &frame) != 1) {
		hid_state.ioc_handle = 0u;
	}
	return 0u;
}
