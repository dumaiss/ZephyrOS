/* ZephyrOS console fallback acceptance (work package 41).
 *
 * The package-20 fallback matrix, re-pointed at the resident services:
 * each image boots a *subset* of the service set (the store's manifest is
 * the contract now, exactly as production's is) and this diagnostic
 * reports the composition the console service derived from it, then
 * proves both directions on the lanes that survived:
 *
 *   ZOS_CONSOLE_FALLBACK=nohid           sio ioc v9958 console
 *       input  = the serial service (no HID endpoint), output = video;
 *   ZOS_CONSOLE_FALLBACK=nohid-nov9958   sio ioc console
 *       both directions = the serial service (the serial-only row).
 *
 * A *bank-7* client of the services (tests/target/include/service_client.h):
 * descriptor-less calls only, and the typed byte the replay queues on the
 * surviving input lane is read back through the console protocol.
 */

#include "zos/protocol.h"

#include "zephyros/errno.h"
#include "zephyros/kernel.h"

#include "console_fallback_selftest.h"
#include "service_client.h"

#define FALLBACK_TAG "console fallback: "
#define FALLBACK_READ_MAX 4u
#define FALLBACK_POLL_BUDGET 4096u

uint8_t ace_fallback_result;

static uint8_t fallback_console;
static uint8_t fallback_failed;

void zos_run_console_fallback_selftest(void)
{
	uint8_t video = 0u;
	uint8_t hid = 0u;
	uint8_t typed[FALLBACK_READ_MAX];
	uint8_t count = 0u;
	uint16_t attempts = 0u;
	uint8_t index;

	ace_fallback_result = 0u;
	fallback_failed = 0u;

	if (service_cli_endpoint(ZOS_CLASS_CONSOLE, &fallback_console) == 0u) {
		ace_fallback_result = 1u;
		service_cli_puts(FALLBACK_TAG "FAIL console\r\n");
		return;
	}

	/* The composition rows: input is HID only when its engine answers
	 * ready, output is video only when the video endpoint is there. */
	if (service_cli_endpoint(ZOS_CLASS_HID, &hid) != 0u &&
	    service_cli_getc(hid, ZHID_OP_READY) == 1) {
		service_cli_puts(FALLBACK_TAG "input hid\r\n");
	} else {
		service_cli_puts(FALLBACK_TAG "input serial\r\n");
	}
	if (service_cli_endpoint(ZOS_CLASS_VIDEO, &video) != 0u) {
		service_cli_puts(FALLBACK_TAG "output video\r\n");
	} else {
		service_cli_puts(FALLBACK_TAG "output serial\r\n");
	}

	/* One visible marker through the console output (no transcript
	 * line: the marker's lane is the output direction's proof, and the
	 * replay looks for these characters on exactly one lane). */
	for (index = 0u; index < 4u; ++index) {
		static const char marker[] = "F41\r\n";

		if (service_cli_putc(fallback_console, ZCONSOLE_OP_PUTC,
				     (uint8_t)marker[index]) < 0) {
			ace_fallback_result = 2u;
			service_cli_puts(FALLBACK_TAG "FAIL write\r\n");
			return;
		}
	}

	/* The typed line, queued by the replay on the surviving input lane. */
	while (attempts < FALLBACK_POLL_BUDGET && count < FALLBACK_READ_MAX) {
		int16_t byte = service_cli_getc(fallback_console,
						ZCONSOLE_OP_GETC);

		if (byte < 0) {
			++attempts;
			continue;
		}
		typed[count] = (uint8_t)byte;
		++count;
		if ((uint8_t)byte == (uint8_t)'\r') {
			break;
		}
	}
	if (count == 0u) {
		ace_fallback_result = 3u;
		service_cli_puts(FALLBACK_TAG "FAIL read\r\n");
		return;
	}
	service_cli_puts(FALLBACK_TAG "echo ");
	for (index = 0u; index < count; ++index) {
		service_cli_hex8(typed[index]);
	}
	service_cli_puts("\r\n");
	service_cli_puts(FALLBACK_TAG "OK\r\n");
}
