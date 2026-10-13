/* KonaOS CONSOLE drivers.
 *
 * One source, two build-selected CONSOLE implementations (architecture
 * 5.5):
 *
 *   console-v9958-hid   input HID, output VIDEO  (the production build)
 *   console-serial      input and output SERIAL  (-DKOS_CONSOLE_SERIAL=1)
 *
 * The host packer selects the implementation by packing the matching
 * manifest (drivers/console/console-v9958-hid.json or
 * console-serial.json) into the store and by resolving that manifest's
 * required dependencies (the order below).  There is no runtime probing
 * and no fallback: the driver forwards each console operation to the
 * endpoint its build selected, and a missing required number fails its
 * init (the loader then fails the boot on the early console, 5.3).
 *
 * Every operation is one forwarded call: the caller's descriptor travels
 * down the chain (4.9), so a WRITE moves the whole transfer through the
 * console and its output driver without a staging copy here.  The
 * console owns no buffer, no IRQ and no state beyond the two handles.
 */

#include "kos/errno.h"
#include "driver.h"
#include "kos/protocol.h"

/* Manifest dependency order, one entry per build. */
#if defined(KOS_CONSOLE_SERIAL)
/* console-serial.json: the SERIAL driver serves both directions. */
#define CONSOLE_IN_DEP    0u
#define CONSOLE_OUT_DEP   0u
#define CONSOLE_IN_READ   KSIO_OP_READ
#define CONSOLE_IN_GETC   KSIO_OP_GETC
#define CONSOLE_IN_POLL   KSIO_OP_POLL
#define CONSOLE_OUT_WRITE KSIO_OP_WRITE
#define CONSOLE_OUT_PUTC  KSIO_OP_PUTC
#else
/* console-v9958-hid.json: HID input, VIDEO output. */
#define CONSOLE_IN_DEP    0u
#define CONSOLE_OUT_DEP   1u
#define CONSOLE_IN_READ   KHID_OP_READ_TEXT
#define CONSOLE_IN_GETC   KHID_OP_GETC
#define CONSOLE_IN_POLL   KHID_OP_POLL
#define CONSOLE_OUT_WRITE KVIDEO_OP_WRITE
#define CONSOLE_OUT_PUTC  KVIDEO_OP_PUTC
#endif

static const struct driver_api *console_api;
static uint8_t console_in;
static uint8_t console_out;

static int16_t console_forward(uint8_t handle, uint8_t op,
			       const struct kos_ipc_frame *frame)
{
	struct kos_ipc_frame inner = *frame;

	if (handle == 0u) {
		return (int16_t)-KOS_ENODEV;
	}
	inner.op = op;
	return console_api->call(handle, op, &inner);
}

int16_t console_driver_dispatch(struct kos_ipc_frame *frame)
{
	switch (frame->op) {
	case KCONSOLE_OP_WRITE:
		return console_forward(console_out, CONSOLE_OUT_WRITE, frame);
	case KCONSOLE_OP_READ:
		return console_forward(console_in, CONSOLE_IN_READ, frame);
	case KCONSOLE_OP_GETC:
		return console_forward(console_in, CONSOLE_IN_GETC, frame);
	case KCONSOLE_OP_POLL:
		return console_forward(console_in, CONSOLE_IN_POLL, frame);
	case KCONSOLE_OP_PUTC:
		return console_forward(console_out, CONSOLE_OUT_PUTC, frame);
	default:
		return (int16_t)-KOS_EINVAL;
	}
}

/* The host resolved every required dependency before the image was
 * packed, so a zero here is a malformed bundle: refuse to initialize and
 * the loader fails the boot (5.3). */
uint8_t console_driver_init(const struct driver_boot *boot)
{
	console_api = boot->api;
	console_in = boot->deps[CONSOLE_IN_DEP];
	console_out = boot->deps[CONSOLE_OUT_DEP];
	if (console_in == 0u || console_out == 0u) {
		return 1u;
	}
	return 0u;
}
