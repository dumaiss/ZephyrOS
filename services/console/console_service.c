/* ZephyrOS console resident service (work package 41).
 *
 * The M8 console binding as a bank-6 service image: it composes the
 * boot's input and output services behind one endpoint, the console
 * protocol of abi/zos/protocol.h (class ZOS_CLASS_CONSOLE, instance 0,
 * version ZOS_PROTOCOL_VERSION).  It is the endpoint the SDK console
 * stubs and the monitor use; the early console and the panic path do not
 * depend on it (architecture 36.12).
 *
 * Composition (the M8 policy, now over host-resolved dependencies):
 *   input   HID when its endpoint is present, its declared dependency
 *           resolved and its link up (ZHID_OP_READY); otherwise the
 *           serial service; otherwise none;
 *   output  the video service when present; otherwise the serial
 *           service; otherwise none.
 * Each direction falls back independently, and the fallback choice
 * happens once at boot (-console-service init-), exactly where the M8
 * device-registry binding decided.  The three endpoints are wired by the
 * host packer (architecture 36.11): the image declares symbolic
 * dependencies on HID, SERIAL and VIDEO, and reads the resolved numbers
 * from the boot bundle; the READY probe stays a protocol interaction, not
 * a lookup.
 *
 * Every operation is one forwarded call: the caller's descriptor travels
 * down the chain (36.8), so a WRITE moves the whole transfer through the
 * console and its output service without a staging copy here.  The
 * console owns no buffer, no IRQ and no state beyond the two handles.
 */

#include "zephyros/errno.h"
#include "zephyros/ipc.h"
#include "zos/protocol.h"

/* Manifest dependency order (services/manifests/console.json). */
#define CONSOLE_DEP_HID    0u
#define CONSOLE_DEP_SERIAL 1u
#define CONSOLE_DEP_VIDEO  2u

static const struct ipc_service_api *console_api;
static uint8_t console_in;
static uint8_t console_out;
/* Which protocol the bound endpoint speaks: HID input vs serial input,
 * video output vs serial output. */
static uint8_t console_in_hid;
static uint8_t console_out_video;

static int16_t console_forward(uint8_t handle, uint8_t op,
			       const struct zos_ipc_frame *frame)
{
	struct zos_ipc_frame inner = *frame;

	inner.op = op;
	return console_api->call(handle, op, &inner);
}

int16_t console_service_dispatch(struct zos_ipc_frame *frame)
{
	switch (frame->op) {
	case ZCONSOLE_OP_WRITE:
		if (console_out == 0u) {
			return (int16_t)-ZOS_ENODEV;
		}
		return console_forward(console_out,
				       console_out_video != 0u ?
				       ZVIDEO_OP_WRITE : ZSIO_OP_WRITE,
				       frame);
	case ZCONSOLE_OP_READ:
		if (console_in == 0u) {
			return (int16_t)-ZOS_ENODEV;
		}
		return console_forward(console_in,
				       console_in_hid != 0u ?
				       ZHID_OP_READ_TEXT : ZSIO_OP_READ,
				       frame);
	case ZCONSOLE_OP_GETC:
		if (console_in == 0u) {
			return (int16_t)-ZOS_ENODEV;
		}
		return console_forward(console_in,
				       console_in_hid != 0u ?
				       ZHID_OP_GETC : ZSIO_OP_GETC,
				       frame);
	case ZCONSOLE_OP_POLL:
		if (console_in == 0u) {
			return (int16_t)-ZOS_ENODEV;
		}
		return console_forward(console_in,
				       console_in_hid != 0u ?
				       ZHID_OP_POLL : ZSIO_OP_POLL,
				       frame);
	case ZCONSOLE_OP_PUTC:
		if (console_out == 0u) {
			return (int16_t)-ZOS_ENODEV;
		}
		return console_forward(console_out,
				       console_out_video != 0u ?
				       ZVIDEO_OP_PUTC : ZSIO_OP_PUTC,
				       frame);
	default:
		return (int16_t)-ZOS_EINVAL;
	}
}

/* Preferred input: the HID service, but only when its engine is ready
 * (the ZHID_OP_READY probe, which is where a missing or down IOC link
 * shows); preferred output: the video service.  Each direction falls
 * back independently to the serial service, so one missing direction
 * never clears a valid other direction. */
void console_service_init(const struct ipc_service_boot *boot)
{
	const struct ipc_service_api *api = boot->api;
	uint8_t handle;
	struct zos_ipc_frame frame;

	console_api = api;
	console_in = 0u;
	console_out = 0u;
	console_in_hid = 0u;
	console_out_video = 0u;

	/* Preferred input: the HID endpoint, but only when its engine is
	 * ready (the ZHID_OP_READY probe, which is where a missing or down
	 * IOC link shows); otherwise the serial endpoint.  The host
	 * resolved both numbers; 0 means the build selected no such
	 * driver. */
	handle = boot->deps[CONSOLE_DEP_HID];
	if (handle != 0u) {
		frame.op = ZHID_OP_READY;
		frame.flags = 0u;
		frame.desc = ZOS_IPC_DESC_NONE;
		frame.arg0 = 0u;
		frame.arg1 = 0u;
		if (api->call(handle, ZHID_OP_READY, &frame) == 1) {
			console_in = handle;
			console_in_hid = 1u;
		}
	}
	if (console_in == 0u) {
		console_in = boot->deps[CONSOLE_DEP_SERIAL];
	}

	/* Preferred output: the video endpoint; otherwise the serial
	 * endpoint.  Each direction falls back independently. */
	handle = boot->deps[CONSOLE_DEP_VIDEO];
	if (handle != 0u) {
		console_out = handle;
		console_out_video = 1u;
	} else {
		console_out = boot->deps[CONSOLE_DEP_SERIAL];
	}
}
