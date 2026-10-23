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
 * The production console also owns a serial rescue path: three 1Bh bytes
 * from the serial line toggle input ownership between HID and SERIAL and
 * arm a tee that mirrors WRITE and PUTC to SERIAL (VIDEO first).  The
 * rescue state is three bytes in the driver I/O area, read and written
 * only by the dispatch paths.
 *
 * Every operation is one forwarded call: the caller's descriptor travels
 * down the chain (4.9), so a WRITE moves the whole transfer through the
 * console and its output driver without a staging copy here; the tee is
 * two such calls.  The console owns no buffer beyond the one-byte serial
 * staging byte, and no IRQ.
 */

#include "kos/errno.h"
#include "driver.h"
#include "kos/protocol.h"
#include "kos/console_render.h"

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
/* The grid the serial console's programs draw: the V9958 console's, so a
 * program looks the same on either. */
#define CONSOLE_SERIAL_COLS 85u
#define CONSOLE_SERIAL_ROWS 26u
#else
/* console-v9958-hid.json: HID input, VIDEO output, SERIAL as the
 * rescue path (dependency position 2). */
#define CONSOLE_IN_DEP       0u
#define CONSOLE_OUT_DEP      1u
#define CONSOLE_SERIAL_DEP   2u
#define CONSOLE_IN_READ      KHID_OP_READ_TEXT
#define CONSOLE_IN_GETC      KHID_OP_GETC
#define CONSOLE_IN_POLL      KHID_OP_POLL
#define CONSOLE_OUT_WRITE    KVIDEO_OP_WRITE
#define CONSOLE_OUT_PUTC     KVIDEO_OP_PUTC
#define CONSOLE_OUT_RENDER   KVIDEO_OP_RENDER
#define CONSOLE_SERIAL_GETC  KSIO_OP_GETC
#define CONSOLE_SERIAL_WRITE KSIO_OP_WRITE
#define CONSOLE_SERIAL_PUTC  KSIO_OP_PUTC
#endif

static const struct driver_api *console_api;
static uint8_t console_in;
static uint8_t console_out;
#if !defined(KOS_CONSOLE_SERIAL)
static uint8_t console_serial;

/* The serial rescue state: three bytes, read and written only by the
 * console_driver_dispatch paths. */
struct console_rescue {
	uint8_t input_owner; /* 0: HID owns input; 1: SERIAL owns it */
	uint8_t tee_armed;   /* WRITE and PUTC are mirrored to SERIAL */
	uint8_t esc_count;   /* consecutive 1Bh bytes seen on SERIAL */
};
static struct console_rescue console_rescue;

/* The one-byte serial staging buffer: the drain holds the input byte a
 * call serves here, and READ copies it out of here. */
static uint8_t console_serial_byte;
static uint8_t console_serial_ready;

#define CONSOLE_ESC         0x1Bu
#define CONSOLE_ESC_TRIGGER 3u
#endif

/* Input already dequeued from the controller sits in the input driver's
 * local ring.  The reset drains it so nothing typed for the program that
 * is going away is read by the program that starts; the bound covers the
 * controller's own queue plus the ring, and the first empty answer ends
 * the drain earlier. */
#define CONSOLE_RESET_DRAIN 160u

static void console_flush_input(void)
{
	struct kos_ipc_frame frame;
	uint16_t guard = CONSOLE_RESET_DRAIN;

	if (console_in == 0u) {
		return;
	}
	frame.flags = 0u;
	frame.desc = KOS_IPC_DESC_NONE;
	frame.arg0 = 0u;
	frame.arg1 = 0u;
	do {
		frame.op = CONSOLE_IN_GETC;
		if (console_api->call(console_in, CONSOLE_IN_GETC,
				      &frame) < 0) {
			break;
		}
	} while (--guard != 0u);
}

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

#if !defined(KOS_CONSOLE_SERIAL)
/* Drain the serial line, detecting the rescue gesture.  Only the third
 * consecutive ESC is the gesture and is consumed; the first two ESCs of
 * a partial match are data, so a VT100 key sequence's lead-in reaches
 * the program (as ZSDOS's receive sink delivers them).  A byte other
 * than ESC resets the count and is dropped while the HID owns input
 * (the rescue drops a terminal that is not the console); while SERIAL
 * owns input the first data byte is held in the staging buffer for the
 * call to serve, and `drop` discards it instead (the reset drain).
 *
 * The caller's frame travels to GETC unchanged: GETC takes no
 * descriptor, so its fields are irrelevant to the serial driver. */
static void console_scan_serial(const struct kos_ipc_frame *frame,
				uint8_t drop)
{
	int16_t byte;

	if (console_serial == 0u || console_serial_ready != 0u) {
		return;
	}
	for (;;) {
		byte = console_forward(console_serial, CONSOLE_SERIAL_GETC,
				       frame);
		if (byte < 0) {
			break;
		}
		if ((uint8_t)byte == CONSOLE_ESC) {
			++console_rescue.esc_count;
			if (console_rescue.esc_count >=
			    CONSOLE_ESC_TRIGGER) {
				/* The third ESC: toggle input ownership
				 * and arm the tee.  Arming on takeover
				 * rescues a dark screen in one gesture. */
				console_rescue.esc_count = 0u;
				console_rescue.input_owner ^= 1u;
				console_rescue.tee_armed = 1u;
				continue;
			}
			/* The partial match's ESC is input, not the
			 * gesture. */
		} else {
			/* Any other byte breaks a partial match. */
			console_rescue.esc_count = 0u;
		}
		if (drop == 0u && console_rescue.input_owner != 0u) {
			console_serial_byte = (uint8_t)byte;
			console_serial_ready = 1u;
			break;
		}
	}
}

/* One input op.  The serial line is drained first, so the gesture is
 * detected on every input op; the op is then answered from whoever owns
 * input.  `hid_op` is the operation forwarded when the HID owns input,
 * and it picks the serial answer otherwise: GETC serves the held byte,
 * READ copies it out, POLL answers whether the byte is held. */
static int16_t console_input_op(struct kos_ipc_frame *frame, uint8_t hid_op)
{
	console_scan_serial(frame, 0u);
	if (console_rescue.input_owner == 0u) {
		return console_forward(console_in, hid_op, frame);
	}
	switch (hid_op) {
	case KHID_OP_GETC:
		if (console_serial_ready == 0u) {
			return (int16_t)-KOS_EAGAIN;
		}
		console_serial_ready = 0u;
		return (int16_t)console_serial_byte;
	case KHID_OP_READ_TEXT:
		if (console_serial_ready == 0u || frame->arg1 == 0u) {
			return 0;
		}
		console_serial_ready = 0u;
		/* One byte per call: the byte is inspected for the
		 * gesture before delivery, through the one-byte
		 * DRIVER_IO staging buffer. */
		if (console_api->copy_out(frame->desc, frame->arg0,
					  (uint16_t)&console_serial_byte,
					  1u) < 0) {
			return (int16_t)-KOS_EINVAL;
		}
		return 1;
	default:
		return (int16_t)console_serial_ready;
	}
}

#endif

/* KCONSOLE_OP_INFO: the grid programs draw.  The serial console answers
 * itself; the V9958 console asks VIDEO, which fills the caller's own
 * frame (the call names the operation in it; it is put back), and while
 * the tee is armed it withdraws RENDER, which it refuses then. */
static int16_t console_info(struct kos_ipc_frame *frame)
{
#if defined(KOS_CONSOLE_SERIAL)
	uint8_t *info = frame->payload;
	uint8_t left = KCONSOLE_INFO_SIZE - 3u;

	*info++ = CONSOLE_SERIAL_COLS;
	*info++ = CONSOLE_SERIAL_ROWS;
	*info++ = KCONSOLE_ATTR_REVERSE;
	do {
		*info++ = 0u;
	} while (--left != 0u);
	return 0;
#else
	int16_t result = console_api->call(console_out, KVIDEO_OP_INFO, frame);

	frame->op = KCONSOLE_OP_INFO;
	if (console_rescue.tee_armed != 0u) {
		frame->payload[3] &= (uint8_t)~KCONSOLE_INFO_RENDER;
	}
	return result;
#endif
}

int16_t console_driver_dispatch(struct kos_ipc_frame *frame)
{
	switch (frame->op) {
	case KCONSOLE_OP_WRITE:
#if !defined(KOS_CONSOLE_SERIAL)
		if (console_rescue.tee_armed != 0u) {
			int16_t result;

			/* VIDEO first; a SERIAL failure does not fail
			 * the call. */
			result = console_forward(console_out,
						 CONSOLE_OUT_WRITE, frame);
			(void)console_forward(console_serial,
					      CONSOLE_SERIAL_WRITE, frame);
			return result;
		}
#endif
		return console_forward(console_out, CONSOLE_OUT_WRITE, frame);
	case KCONSOLE_OP_READ:
#if !defined(KOS_CONSOLE_SERIAL)
		return console_input_op(frame, KHID_OP_READ_TEXT);
#else
		return console_forward(console_in, CONSOLE_IN_READ, frame);
#endif
	case KCONSOLE_OP_GETC:
#if !defined(KOS_CONSOLE_SERIAL)
		return console_input_op(frame, KHID_OP_GETC);
#else
		return console_forward(console_in, CONSOLE_IN_GETC, frame);
#endif
	case KCONSOLE_OP_POLL:
#if !defined(KOS_CONSOLE_SERIAL)
		return console_input_op(frame, KHID_OP_POLL);
#else
		return console_forward(console_in, CONSOLE_IN_POLL, frame);
#endif
	case KCONSOLE_OP_PUTC:
#if !defined(KOS_CONSOLE_SERIAL)
		if (console_rescue.tee_armed != 0u) {
			int16_t result;

			result = console_forward(console_out,
						 CONSOLE_OUT_PUTC, frame);
			(void)console_forward(console_serial,
					      CONSOLE_SERIAL_PUTC, frame);
			return result;
		}
#endif
		return console_forward(console_out, CONSOLE_OUT_PUTC, frame);
	case KCONSOLE_OP_RESET:
		/* Input the program that is going away never read must not
		 * reach the program that starts. */
#if !defined(KOS_CONSOLE_SERIAL)
		/* The serial queue and the held byte are drained too, with
		 * the gesture still detected on the way.  While SERIAL
		 * owns input the HID queue is left alone: keyboard bytes
		 * typed meanwhile are delivered once ownership returns.
		 * The ownership and the tee survive the reset, so the
		 * rescue persists across the launcher's reset before
		 * every boot program. */
		console_serial_ready = 0u;
		console_scan_serial(frame, 1u);
		if (console_rescue.input_owner == 0u) {
			console_flush_input();
		}
		return console_forward(console_out, KVIDEO_OP_RESET, frame);
#else
		console_flush_input();
		/* This build has no video dependency and keeps no terminal
		 * state of its own: the attributes, the escape parser and the
		 * cursor belong to the video driver, which is not here. */
		return 0;
#endif
	case KCONSOLE_OP_INFO:
		return console_info(frame);
	case KCONSOLE_OP_RENDER:
#if defined(KOS_CONSOLE_SERIAL)
		return (int16_t)-KOS_ENOTSUP;
#else
		/* While the tee is armed, refuse the render stream: a
		 * client that falls back to ANSI text reaches both
		 * screens, and one that renders reaches the V9958 only. */
		if (console_rescue.tee_armed != 0u) {
			return (int16_t)-KOS_ENOTSUP;
		}
		return console_forward(console_out, CONSOLE_OUT_RENDER, frame);
#endif
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
#if !defined(KOS_CONSOLE_SERIAL)
	console_serial = boot->deps[CONSOLE_SERIAL_DEP];
#endif
	if (console_in == 0u || console_out == 0u
#if !defined(KOS_CONSOLE_SERIAL)
	    || console_serial == 0u
#endif
	   ) {
		return 1u;
	}
	return 0u;
}
