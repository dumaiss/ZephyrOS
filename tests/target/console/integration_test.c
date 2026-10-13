/* KonaOS console integration acceptance.
 *
 * The integration diagnostic of the resident drivers.  The kernel calls kos_run_console_integration_selftest()
 * from the late boot hook, after the resident driver loader has placed
 * and started the store's drivers and after the nucleus console probe
 * bound the console endpoint; the unchanged monitor launcher runs
 * afterwards on the same image, so the human HID/V9958 session is this
 * test's visible continuation.
 *
 * This is a *bank-7 client* of the drivers (see
 * tests/target/include/driver_client.h): it holds no descriptor, so it
 * drives the descriptor-less forms - console PUTC/GETC/POLL and the IOC
 * command exchange whose reply it stages out of the driver window
 * through the common copy gate.
 *
 * The transcript goes through the direct early SIO, so it is the
 * pass/fail record independent of the console driver it exercises; the
 * replay cross-checks the visible side (the marker characters appear in
 * the V9958 trace and never on the serial lane, and the typed bytes the
 * diagnostic reads back are the ones the emulated MCU queued for HID).
 *
 * Coverage:
 *
 *   - every class the production store carries answers an endpoint
 *     lookup at KOS_PROTOCOL_VERSION;
 *   - the console endpoint speaks the console protocol, and the
 *     composition the build selected (`console-v9958-hid`: video output,
 *     HID input) is what this image's manifest implies;
 *   - a written marker reaches the visible console, and a typed line
 *     queued for HID arrives through the console input;
 *   - the IOC transport answers a PING through its own endpoint with the
 *     pinned controller level (the cross-driver row);
 *   - malformed and unknown operations are refused, and the console
 *     still works afterwards.
 */

#include "kos/protocol.h"

#include "kos/errno.h"
#include "ioc.h"
#include "kernel.h"

#include "console_integration_selftest.h"
#include "driver_client.h"

#define INTEGRATION_TAG "console integration: "
/* Bytes the replay queues for HID: the typed line this diagnostic reads
 * back through the console (CR terminates it).  On hardware the same
 * bytes come from the USB keyboard, so the read waits for them. */
#define INTEGRATION_TYPED "K41"
#define INTEGRATION_TYPED_END '\r'
#define INTEGRATION_READ_MAX 8u
/* The wait for a key, in polls: the replay has the line queued before
 * this hook runs, so it succeeds immediately; on hardware the operator
 * types it on the USB keyboard, which the old 512-poll budget expired
 * before - the hardware run printed `FAIL read` even though the
 * composition row is fine. */
#define INTEGRATION_POLL_BUDGET 100000u
/* PING request DATA (the pinned firmware accepts up to 24 bytes). */
#define INTEGRATION_PING_DATA_LEN 4u
#define INTEGRATION_PING_LEVEL_INDEX 16u
#define INTEGRATION_PING_RESPONSE 0x81u
#define INTEGRATION_LEVEL_PINNED 0x4Au

uint8_t ace_integration_result;

static uint8_t integration_console;
static uint8_t integration_ioc;
static uint8_t integration_failed;

static void integration_puts(const char *text)
{
	driver_cli_puts(INTEGRATION_TAG);
	driver_cli_puts(text);
	driver_cli_puts("\r\n");
}

static void integration_fail(uint8_t code, const char *phase)
{
	if (integration_failed == 0u) {
		integration_failed = 1u;
		ace_integration_result = code;
		driver_cli_puts(INTEGRATION_TAG "FAIL ");
		driver_cli_puts(phase);
		driver_cli_puts("\r\n");
	}
}

/* One class lookup row: `<name>=01` when the endpoint is registered at
 * the running protocol version, `<name>=00` when it is not. */
static void integration_probe(uint8_t class_id, const char *name)
{
	uint8_t handle = 0u;
	uint8_t present = driver_cli_endpoint(class_id, &handle);

	driver_cli_puts(" ");
	driver_cli_puts(name);
	driver_cli_puts(present != 0u ? "=01" : "=00");
	if (present == 0u) {
		integration_fail(KOS_CONSOLE_INTEGRATION_FAIL_BOOT, "drivers");
	}
}

/* One console output marker: the replay looks for these characters in the
 * V9958 trace, never on the serial lane. */
static int integration_write(const char *text)
{
	while (*text != '\0') {
		if (driver_cli_putc(integration_console, KCONSOLE_OP_PUTC,
				     (uint8_t)*text) < 0) {
			return 0;
		}
		++text;
	}
	return 1;
}

/* The typed line, one byte per call through the console input: the
 * console asks HID because its engine answered ready, which is the
 * composition row this reads back.  The poll waits for the key rather
 * than assuming it is already pending (see INTEGRATION_POLL_BUDGET). */
static uint8_t integration_read(uint8_t *out)
{
	uint32_t attempts = 0u;
	uint8_t count = 0u;

	while (attempts < INTEGRATION_POLL_BUDGET &&
	       count < INTEGRATION_READ_MAX) {
		int16_t byte = driver_cli_getc(integration_console,
						KCONSOLE_OP_GETC);

		if (byte < 0) {
			++attempts;
			continue;
		}
		if ((uint8_t)byte == (uint8_t)INTEGRATION_TYPED_END) {
			break;
		}
		out[count] = (uint8_t)byte;
		++count;
	}
	return count;
}

/* The cross-driver row: an IOC PING through the IOC endpoint, with the
 * 29-byte reply staged out of the driver window.  Returns 1 when the
 * reply is the pinned one and fills in the controller level. */
static uint8_t integration_ping(uint8_t *level)
{
	struct kos_ipc_frame frame;
	uint8_t reply[KIOC_FRAME_SIZE];
	uint16_t address;
	uint8_t index;

	driver_cli_frame(&frame, KIOC_OP_COMMAND, 0x01u,
			  INTEGRATION_PING_DATA_LEN);
	for (index = 0u; index < INTEGRATION_PING_DATA_LEN; ++index) {
		frame.payload[index] = (uint8_t)(0x40u + index);
	}
	if (driver_cli_call(integration_ioc, &frame) != 0) {
		return 0u;
	}
	if (frame.arg0 != INTEGRATION_PING_RESPONSE ||
	    frame.payload[2] != 0u) {
		return 0u;
	}
	address = (uint16_t)((uint16_t)frame.payload[0] |
			     ((uint16_t)frame.payload[1] << 8));
	if (driver_cli_stage_read(address, KIOC_FRAME_SIZE, reply) == 0) {
		return 0u;
	}
	if (reply[0] != INTEGRATION_PING_RESPONSE || reply[1] != 0u) {
		return 0u;
	}
	/* The PING reply echoes the request DATA and pins the controller
	 * level plus the link-synced flag the firmware reports. */
	for (index = 0u; index < INTEGRATION_PING_DATA_LEN; ++index) {
		if (reply[3u + index] != (uint8_t)(0x40u + index)) {
			return 0u;
		}
	}
	*level = reply[3u + INTEGRATION_PING_LEVEL_INDEX];
	return (uint8_t)(reply[3u + INTEGRATION_PING_LEVEL_INDEX + 1u] ==
			 0x40u);
}

/* Unknown operations on the resident classes are refused, and a handle
 * the nucleus never issued is refused before any driver runs. */
static uint8_t integration_rejects(void)
{
	struct kos_ipc_frame frame;
	uint8_t video = 0u;
	uint8_t hid = 0u;

	driver_cli_frame(&frame, 0x7Fu, 0u, 0u);
	if (driver_cli_call(integration_console, &frame) != -KOS_EINVAL) {
		return 0u;
	}
	driver_cli_frame(&frame, 0x7Fu, 0u, 0u);
	if (driver_cli_call(integration_ioc, &frame) != -KOS_EINVAL) {
		return 0u;
	}
	if (driver_cli_endpoint(KOS_CLASS_VIDEO, &video) != 0u) {
		driver_cli_frame(&frame, 0x7Fu, 0u, 0u);
		if (driver_cli_call(video, &frame) != -KOS_EINVAL) {
			return 0u;
		}
	}
	if (driver_cli_endpoint(KOS_CLASS_HID, &hid) != 0u) {
		driver_cli_frame(&frame, 0x7Fu, 0u, 0u);
		if (driver_cli_call(hid, &frame) != -KOS_EINVAL) {
			return 0u;
		}
	}
	driver_cli_frame(&frame, KCONSOLE_OP_PUTC, (uint8_t)'x', 0u);
	if (driver_cli_call((uint8_t)0xFEu, &frame) >= 0) {
		return 0u;
	}
	return 1u;
}

void kos_run_console_integration_selftest(void)
{
	uint8_t video = 0u;
	uint8_t hid = 0u;
	uint8_t typed[INTEGRATION_READ_MAX];
	uint8_t typed_count;
	uint8_t level = 0u;
	uint8_t index;

	ace_integration_result = KOS_CONSOLE_INTEGRATION_OK;
	integration_failed = 0u;

	driver_cli_puts(INTEGRATION_TAG "drivers");
	integration_probe(KOS_CLASS_SERIAL, "sio");
	integration_probe(KOS_CLASS_IOC, "ioc");
	integration_probe(KOS_CLASS_HID, "hid");
	integration_probe(KOS_CLASS_VIDEO, "video");
	integration_probe(KOS_CLASS_CONSOLE, "console");
	driver_cli_puts("\r\n");
	if (driver_cli_endpoint(KOS_CLASS_CONSOLE, &integration_console) ==
	    0u || integration_failed != 0u) {
		integration_fail(KOS_CONSOLE_INTEGRATION_FAIL_SHAPE, "console");
		return;
	}

	/* The composition rows: the console prefers the video output and
	 * the HID input (when its engine answers ready), which is what this
	 * store carries.  The replay proves the choice from the visible
	 * side; these lines record what the diagnostic observed. */
	integration_puts("console v01");
	if (driver_cli_endpoint(KOS_CLASS_VIDEO, &video) != 0u) {
		integration_puts("output video");
	} else {
		integration_puts("output serial");
	}
	if (driver_cli_endpoint(KOS_CLASS_HID, &hid) != 0u &&
	    driver_cli_getc(hid, KHID_OP_READY) == 1) {
		integration_puts("input hid");
	} else {
		integration_puts("input serial");
	}

	/* The visible marker: characters written through the console. */
	if (integration_write("C41\r\n") == 0) {
		integration_fail(KOS_CONSOLE_INTEGRATION_FAIL_CONSOLE, "write");
		return;
	}
	/* The typed line, queued for HID by the replay. */
	typed_count = integration_read(typed);
	if (typed_count != (uint8_t)(sizeof(INTEGRATION_TYPED) - 1u)) {
		integration_fail(KOS_CONSOLE_INTEGRATION_FAIL_CONSOLE, "read");
		return;
	}
	driver_cli_puts(INTEGRATION_TAG "echo ");
	for (index = 0u; index < typed_count; ++index) {
		if (typed[index] != (uint8_t)INTEGRATION_TYPED[index]) {
			integration_fail(KOS_CONSOLE_INTEGRATION_FAIL_CONSOLE,
					 "typed");
			return;
		}
		driver_cli_putc(integration_console, KCONSOLE_OP_PUTC,
				 typed[index]);
	}
	driver_cli_puts("\r\n");
	/* Drained: the poll row the console protocol exposes. */
	if (driver_cli_poll(integration_console, KCONSOLE_OP_POLL) != 0) {
		integration_fail(KOS_CONSOLE_INTEGRATION_FAIL_CONSOLE, "poll");
		return;
	}

	if (driver_cli_endpoint(KOS_CLASS_IOC, &integration_ioc) != 0u) {
		uint8_t synced = integration_ping(&level);

		driver_cli_puts(INTEGRATION_TAG "ioc level ");
		driver_cli_hex8(level);
		driver_cli_puts("\r\n");
		if (synced == 0u || level != INTEGRATION_LEVEL_PINNED) {
			integration_fail(KOS_CONSOLE_INTEGRATION_FAIL_IOC, "ioc");
			return;
		}
	}

	if (integration_rejects() == 0u) {
		integration_fail(KOS_CONSOLE_INTEGRATION_FAIL_MALFORMED,
				 "rejects");
		return;
	}
	integration_puts("rejected calls OK");

	/* The console still works after the rejected calls. */
	if (integration_write("integration done\r\n") == 0) {
		integration_fail(KOS_CONSOLE_INTEGRATION_FAIL_CONSOLE,
				 "recovery");
		return;
	}
	integration_puts("OK");
}
