/* KonaOS IPC acceptance: the `echo` synthetic driver (test artifact).
 *
 * The null call: the cheapest possible protocol on the IPC substrate, used
 * to measure the raw crossing and dispatch cost, to prove re-entrancy
 * refusal (ECHO_SELF calls its own endpoint from inside its own dispatch)
 * and to expose the descriptor id the nucleus handed the call
 * (ECHO_DESC).
 *
 * The image's hot segment carries the dispatch entry and this init entry;
 * the boot segment (echo_boot.c) records its single bring-up run.  The
 * resident loader has already registered the declared endpoint when the
 * init runs, so the init resolves its own handle by class/instance, the
 * same discovery a peer uses.
 */

#include "kos/errno.h"
#include "driver.h"

#include "ipc_test.h"

static const struct driver_api *api;
static uint8_t self_handle;

/* The mapped segment (echo_map.c) and an io-segment staging buffer for
 * the copy probe. */
extern const uint8_t echo_mapped[IPC_SPAN_BYTES];
extern uint8_t echo_mapped_bss[IPC_SPAN_BYTES];
static uint8_t echo_stage[16];
static uint16_t echo_stage_address;
/* Set at init: an address taken in a local splits into bytes, which the
 * packer cannot relocate. */
static const uint8_t *echo_pattern;
static uint8_t *echo_scratch;

static __sfr __at 0x00 echo_latch;

/* The bulk-map span probe (IPC_OP_ECHO_SPAN, ipc_test.h). */
static int16_t echo_span(struct kos_ipc_frame *frame)
{
	struct kos_ipc_frame inner = *frame;
	uint8_t desc = frame->desc;
	uint8_t caller_q = (uint8_t)(echo_latch & 0x07u);
	uint8_t mask = 0u;
	uint8_t ok = 1u;
	uint8_t i;
	const uint8_t *pattern;
	uint8_t *scratch;

	if (api->bulk_map() != 0) {
		return -KOS_EIO;
	}
	if ((echo_latch & 0x07u) == 6u) {
		mask |= IPC_SPAN_LATCH;
	}
	pattern = echo_pattern;
	for (i = 0u; i < IPC_SPAN_BYTES; ++i) {
		if (*pattern++ != (uint8_t)(0xC0u + i)) {
			ok = 0u;
		}
	}
	if (ok) {
		mask |= IPC_SPAN_MAPPED;
	}
	scratch = echo_scratch;
	for (i = 0u; i < IPC_SPAN_BYTES; ++i) {
		*scratch++ = (uint8_t)(i ^ 0x5Au);
	}
	ok = 1u;
	scratch = echo_scratch;
	for (i = 0u; i < IPC_SPAN_BYTES; ++i) {
		if (*scratch++ != (uint8_t)(i ^ 0x5Au)) {
			ok = 0u;
		}
	}
	if (ok) {
		mask |= IPC_SPAN_BSS;
	}
	if (api->bulk_map() == -KOS_EBUSY) {
		mask |= IPC_SPAN_NEST;
	}
	if (api->call(self_handle, IPC_OP_ECHO_NULL, &inner) == -KOS_EBUSY) {
		mask |= IPC_SPAN_CALL;
	}
	if (api->copy_in(desc, 0u, echo_stage_address, 16u) ==
	    -KOS_EBUSY) {
		mask |= IPC_SPAN_COPY;
	}
	if (api->bulk_read(0u, echo_stage_address, 1u) == -KOS_EBUSY) {
		mask |= IPC_SPAN_BULK;
	}
	api->bulk_unmap();
	if ((echo_latch & 0x07u) != caller_q) {
		mask &= (uint8_t)~IPC_SPAN_LATCH;
	}
	if (api->copy_in(desc, 0u, echo_stage_address, 16u) == 16) {
		mask |= IPC_SPAN_AFTER;
	}
	return (int16_t)mask;
}

/* The endpoint dispatch entry the manifest names: a module-global
 * symbol so the packer can resolve its hot-image offset. */
int16_t echo_dispatch(struct kos_ipc_frame *frame)
{
	switch (frame->op) {
	case IPC_OP_ECHO_EXCHANGE:
		frame->payload[1] = frame->payload[0];
		return (int16_t)((uint16_t)frame->payload[0] + 1u);
	case IPC_OP_ECHO_NULL:
		return (int16_t)KOS_IPC_FRAME_SIZE;
	case IPC_OP_ECHO_SELF: {
		struct kos_ipc_frame inner = *frame;

		/* The endpoint is active: the nucleus must refuse this with
		 * -KOS_EBUSY because echo declares no re-entrancy. */
		return api->call(self_handle, IPC_OP_ECHO_NULL, &inner);
	}
	case IPC_OP_ECHO_DESC:
		return (int16_t)frame->desc;
	case IPC_OP_ECHO_SPAN:
		return echo_span(frame);
	case IPC_OP_ECHO_LEAK:
		/* Deliberately left open: the nucleus closes it. */
		return api->bulk_map();
	default:
		return -KOS_EINVAL;
	}
}

uint8_t ipc_echo_init(const struct driver_boot *boot)
{
	api = boot->api;
	echo_stage_address = (uint16_t)echo_stage;
	echo_pattern = echo_mapped;
	echo_scratch = echo_mapped_bss;
	/* Manifest dependency 0: this image's own endpoint, resolved on the
	 * host (the ECHO_SELF refusal probe calls it). */
	self_handle = boot->deps[0];
	return 0u;
}
