/* ZephyrOS HID keyboard/text driver implementation (drivers/hid/).
 *
 * The class surface lives in include/zephyros/hid.h.  This file owns the
 * 16-byte local ring, the fetch-on-doorbell engine, and the
 * stuck-doorbell penalty; the descriptor and module entry live in
 * hid_module.c.
 *
 * Input path (the deployed MCU is the authority, the firmware is never
 * modified):
 *
 *   - the MCU translates a USB boot keyboard to terminal bytes and keeps
 *     its own 128-byte queue; `/CTSB` is asserted while that queue holds
 *     anything, and the IOC class maps the level to
 *     ZIOC_STATUS_HID_PENDING.  HID samples that semantic bit only; it
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
 *     as a poll error; the console falls through to whatever else is
 *     bound and the penalty throttles the retry.
 *
 * Translation is the MCU's: this driver copies bytes verbatim, including
 * NUL, control bytes and multi-byte VT100 key sequences.
 *
 * This source is compiled into the HID ZMOD module; the state below
 * lives in module BSS (`_DATA`) and is relocated with the module image.
 * The module reaches the kernel only through the passed `zdriver_api`.
 */

#include "zephyros/device.h"
#include "zephyros/hid.h"

#include "zephyros/ioc_protocol.h"

#include "hid_driver.h"

struct zhid_state hid_state;

/* One command exchange and one reply, in native IOC frames.  The
 * transport owns framing, CRC and the rolling sequence. */
static uint8_t hid_fetch(struct zhid_state *state)
{
	uint8_t free_space = (uint8_t)(ZHID_RING_SIZE - state->count);
	uint8_t payload;
	uint8_t i;
	struct zioc_frame request;
	struct zioc_frame reply;
	int result;

	if (free_space == 0u) {
		return 0u;
	}

	/* Ask for exactly the local free space, never above the protocol's
	 * per-reply maximum. */
	if (free_space > IOC_HID_INPUT_MAX_DATA) {
		free_space = IOC_HID_INPUT_MAX_DATA;
	}
	request.type = IOC_CMD_HID_INPUT;
	request.status = 0u;
	request.length = 1u;
	request.data[IOC_OFF_HID_INPUT_MAX] = free_space;

	result = state->ioc->command(state->ioc_ctx, &request, &reply);
	if (result != ZIOC_OK) {
		return 0u;
	}
	if (reply.type != IOC_RSP_HID_INPUT) {
		return 0u;
	}
	if (reply.status != 0u) {
		return 0u;
	}
	/* DATA = queued, dropped, then the translated bytes. */
	if (reply.length < IOC_HID_INPUT_META_LEN ||
	    reply.length > (uint8_t)(IOC_HID_INPUT_META_LEN +
				     IOC_HID_INPUT_MAX_DATA)) {
		return 0u;
	}
	payload = (uint8_t)(reply.length - IOC_HID_INPUT_META_LEN);

	/* The MCU dequeued what it sent: anything beyond what was asked
	 * for, or beyond the ring's actual space, cannot be committed. */
	if (payload > free_space) {
		return 0u;
	}
	if (payload > (uint8_t)(ZHID_RING_SIZE - state->count)) {
		return 0u;
	}

	for (i = 0u; i < payload; ++i) {
		state->ring[state->head] = reply.data[IOC_HID_INPUT_META_LEN + i];
		state->head = (uint8_t)((state->head + 1u) &
					(ZHID_RING_SIZE - 1u));
	}
	state->count = (uint8_t)(state->count + payload);
	return payload;
}

/* The fetch-on-doorbell engine shared by poll/getc/read_text.  Returns 1
 * when a byte is buffered after the call, 0 otherwise.  Only here (and
 * only while the semantic doorbell is asserted) is any IOC command
 * issued. */
static uint8_t hid_service(struct zhid_state *state)
{
	uint16_t status;

	if (state->count != 0u) {
		return 1u;
	}

	status = state->ioc->status(state->ioc_ctx);
	if ((status & ZIOC_STATUS_HID_PENDING) == 0u) {
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
	state->penalty = ZHID_STUCK_PENALTY;
	return 0u;
}

int hid_read_text(void *vstate, uint8_t *dst, uint16_t max_len)
{
	struct zhid_state *state = (struct zhid_state *)vstate;
	uint16_t done = 0u;

	if (state == (struct zhid_state *)0 || state->ioc ==
	    (const struct zioc_ops *)0) {
		return ZDEV_ENODEV;
	}
	if (dst == (uint8_t *)0 || max_len == 0u) {
		return ZDEV_EINVAL;
	}
	if (state->count == 0u) {
		(void)hid_service(state);
	}
	while (done < max_len && state->count != 0u) {
		dst[done] = state->ring[state->tail];
		state->tail = (uint8_t)((state->tail + 1u) &
					(ZHID_RING_SIZE - 1u));
		state->count = (uint8_t)(state->count - 1u);
		++done;
	}
	return (int)done;
}

int hid_poll(void *vstate)
{
	struct zhid_state *state = (struct zhid_state *)vstate;

	if (state == (struct zhid_state *)0 || state->ioc ==
	    (const struct zioc_ops *)0) {
		return ZDEV_ENODEV;
	}
	return hid_service(state) != 0u ? 1 : 0;
}

int hid_getc(void *vstate)
{
	struct zhid_state *state = (struct zhid_state *)vstate;
	uint8_t byte;

	if (state == (struct zhid_state *)0 || state->ioc ==
	    (const struct zioc_ops *)0) {
		return ZDEV_ENODEV;
	}
	if (state->count == 0u) {
		(void)hid_service(state);
	}
	if (state->count == 0u) {
		return ZDEV_EAGAIN;
	}
	byte = state->ring[state->tail];
	state->tail = (uint8_t)((state->tail + 1u) & (ZHID_RING_SIZE - 1u));
	state->count = (uint8_t)(state->count - 1u);
	return (int)byte;
}

int hid_driver_init(const struct zdriver_api *api)
{
	zdev_handle_t handle;
	const void *class_ops = (const void *)0;
	void *ioc_ctx = (void *)0;
	const struct zioc_ops *ops;

	if (api == (const struct zdriver_api *)0) {
		return ZDRIVER_FAIL;
	}

	/* Dependency selection is by class plus required capabilities.
	 * Name lookup and direct symbol references are forbidden. */
	handle = api->device_find(ZDEV_IOC,
				  ZCAP_IOC_COMMAND | ZCAP_IOC_STATUS, 0u);
	if (handle == ZDEV_HANDLE_INVALID) {
		return ZDRIVER_FAIL;
	}
	if (api->device_interface(handle, ZDEV_IOC, &class_ops, &ioc_ctx) !=
	    ZDEV_OK) {
		return ZDRIVER_FAIL;
	}
	ops = (const struct zioc_ops *)class_ops;
	if (ops == (const struct zioc_ops *)0 ||
	    ops->command == (int (*)(void *, const struct zioc_frame *,
				     struct zioc_frame *))0 ||
	    ops->status == (uint16_t (*)(void *))0) {
		return ZDRIVER_FAIL;
	}

	hid_state.ioc = ops;
	hid_state.ioc_ctx = ioc_ctx;
	hid_state.head = 0u;
	hid_state.tail = 0u;
	hid_state.count = 0u;
	hid_state.penalty = 0u;
	return ZDRIVER_OK;
}

const struct zhid_ops hid_ops = {
	hid_read_text,
	(int (*)(void *, uint8_t, struct zhid_gamepad_state *))0
};

const struct zchar_ops hid_char_ops = {
	(int (*)(void *, char))0,
	(int (*)(void *, const char *, uint16_t))0,
	hid_poll,
	hid_getc
};
