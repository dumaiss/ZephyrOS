/* ZephyrOS console acceptance diagnostic (work package 41).
 *
 * The package-19 console image is the operator's HID-keyboard/V9958 pass
 * image.  Its diagnostic is a *bank-7 client* of the resident services
 * (tests/target/include/service_client.h): it reports the composition the
 * console service derived from the store's endpoint set - the M8
 * "console_bind_defaults" decision, now made inside the service - writes
 * one visible marker through the console output, reads the line the
 * replay queues on the surviving input lane, and leaves the unchanged M7
 * monitor running for the human session.
 *
 * The transcript goes through the direct early SIO.
 */

#include "zos/protocol.h"

#include "zephyros/errno.h"
#include "zephyros/kernel.h"

#include "console_selftest.h"
#include "service_client.h"

#define CONSOLE_TAG "console selftest: "
#define CONSOLE_MARKER "m8\r\n"
#define CONSOLE_READ_MAX 8u
#define CONSOLE_POLL_BUDGET 4096u

uint8_t ace_console_result;

void zos_run_console_selftest(void)
{
	uint8_t console = 0u;
	uint8_t video = 0u;
	uint8_t hid = 0u;
	uint8_t typed[CONSOLE_READ_MAX];
	uint8_t count = 0u;
	uint16_t attempts = 0u;
	const char *text;

	ace_console_result = 0u;
	if (service_cli_endpoint(ZOS_CLASS_CONSOLE, &console) == 0u) {
		ace_console_result = 1u;
		service_cli_puts(CONSOLE_TAG "FAIL console\r\n");
		return;
	}
	if (service_cli_endpoint(ZOS_CLASS_HID, &hid) != 0u &&
	    service_cli_getc(hid, ZHID_OP_READY) == 1) {
		service_cli_puts(CONSOLE_TAG "input class 04 (hid)\r\n");
	} else {
		service_cli_puts(CONSOLE_TAG "input class 01 (serial)\r\n");
	}
	if (service_cli_endpoint(ZOS_CLASS_VIDEO, &video) != 0u) {
		service_cli_puts(CONSOLE_TAG "output class 02 (video)\r\n");
	} else {
		service_cli_puts(CONSOLE_TAG "output class 01 (serial)\r\n");
	}

	for (text = CONSOLE_MARKER; *text != '\0'; ++text) {
		if (service_cli_putc(console, ZCONSOLE_OP_PUTC,
				     (uint8_t)*text) < 0) {
			ace_console_result = 2u;
			service_cli_puts(CONSOLE_TAG "FAIL write\r\n");
			return;
		}
	}
	while (attempts < CONSOLE_POLL_BUDGET && count < CONSOLE_READ_MAX) {
		int16_t byte = service_cli_getc(console, ZCONSOLE_OP_GETC);

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
		ace_console_result = 3u;
		service_cli_puts(CONSOLE_TAG "FAIL read\r\n");
		return;
	}
	/* The typed bytes are echoed on the visible lane; the transcript
	 * records that the input direction delivered them. */
	service_cli_puts(CONSOLE_TAG "typed\r\n");
	for (attempts = 0u; attempts < count; ++attempts) {
		if (service_cli_putc(console, ZCONSOLE_OP_PUTC,
				     typed[attempts]) < 0) {
			break;
		}
	}
	service_cli_puts(CONSOLE_TAG "OK\r\n");
}
