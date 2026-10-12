/* ZephyrOS HID acceptance diagnostic, resident-driver edition (work
 * package 41).
 *
 * The kernel calls zos_run_hid_selftest() from the late boot hook, after
 * the driver loader placed and started the console-integration image's
 * `sio ioc hid v9958 console-v9958-hid` set (work package 42d merged the
 * per-device images; the console composes HID input with the V9958
 * output), and the nucleus console line completed.
 * The diagnostic is itself a bank-7 client of the resident drivers (see
 * tests/target/include/driver_client.h): it holds no descriptor, so it
 * drives the HID protocol through the descriptor-less byte forms -
 * ZHID_OP_READY, ZHID_OP_POLL and ZHID_OP_GETC - exactly the calls the
 * console driver forwards when a program reads its input.
 *
 * The transcript goes through the direct early SIO, so it is the
 * pass/fail record independent of the console driver it exercises; the
 * simulator replay pins it and cross-checks the peer side (the emulated
 * MCU's terminal queue, the fetch-on-doorbell requests and the drain).
 *
 * Coverage:
 *
 *   - endpoint discovery: the HID class answers a lookup at
 *     ZOS_PROTOCOL_VERSION v01, the IOC dependency endpoint is present,
 *     and the engine reports ZHID_OP_READY = 1;
 *   - the scripted typed pass the replay queues in the MCU's terminal
 *     queue ('a', 'B', '3', Enter, Backspace, Ctrl-X, Arrow-Up): POLL
 *     reports pending, repeated GETC returns the queued bytes exactly in
 *     FIFO order, and the drained driver answers -ZOS_EAGAIN;
 *   - the console endpoint's GETC delivers the pair the replay queues
 *     for the MCU: the production console's input is this same HID
 *     engine, so the console read drives the engine the diagnostic
 *     just drained;
 *   - malformed-reply atomicity: one injected short CMD_HID_INPUT reply
 *     delivers nothing from it; the clean pair queued behind it arrives
 *     afterwards in FIFO order (after the engine's bounded stuck-doorbell
 *     penalty), which is what the replay asserts byte-for-byte;
 *   - an unknown HID operation answers -ZOS_EINVAL, and a handle the
 *     nucleus never issued (0 or out of range) is refused before any
 *     driver runs.
 *
 * Final line: "hid selftest: OK", or "hid selftest: FAIL <phase>" with
 * the offending byte on the preceding "hid: detail XX" line.
 */

#include "zos/errno.h"
#include "zos/protocol.h"

#include "hid_selftest.h"
#include "driver_client.h"

#if ZOS_HID_SELFTEST

/* Failure codes recorded in ace_hid_selftest_result (0 = success). */
#define HID_FAIL_BOOT    0x01u /* endpoint discovery failed          */
#define HID_FAIL_READY   0x02u /* ZHID_OP_READY did not answer 1     */
#define HID_FAIL_TYPED   0x03u /* typed FIFO pass failed             */
#define HID_FAIL_CONSOLE 0x04u /* console input pass failed         */
#define HID_FAIL_FAULT   0x05u /* malformed reply disturbed the pass */
#define HID_FAIL_REJECTS 0x06u /* malformed call was not refused     */

uint8_t ace_hid_selftest_result;

static void hid_fail(const char *phase, uint8_t code, uint8_t detail)
{
	ace_hid_selftest_result = code;
	driver_cli_puts("hid: detail ");
	driver_cli_hex8(detail);
	driver_cli_puts("\r\nhid selftest: FAIL ");
	driver_cli_puts(phase);
	driver_cli_puts("\r\n");
}

#define HID_TYPED_LEN   9u
#define HID_PAIR_LEN    2u
/* The fault pass spans the engine's 64-call stuck-doorbell penalty:
 * 256 calls cover the penalty and the flush with margin. */
#define HID_FAULT_SPINS 256u
#define HID_CONSOLE_SPINS 256u

static uint8_t hid_endpoint;
static uint8_t ioc_endpoint;
static uint8_t console_endpoint;

/* The scripted key pass the replay queues for the MCU: printable keys,
 * Enter, Backspace, Ctrl-X and one VT100 Arrow-Up (ESC [ A).  The MCU's
 * terminal translation, not raw USB reports. */
static const uint8_t hid_typed[HID_TYPED_LEN] = {
	0x61u, 0x42u, 0x33u, 0x0Du, 0x08u, 0x18u,
	0x1Bu, 0x5Bu, 0x41u
};

/* The pair the replay queues when the console phase starts, and the pair
 * it queues behind the injected malformed reply. */
static const uint8_t hid_console[HID_PAIR_LEN] = { 0x4Fu, 0x4Bu };
static const uint8_t hid_fault[HID_PAIR_LEN] = { 0x71u, 0x72u };

/* One `<tag> <hex>` transcript line. */
static void hid_rx(const char *tag, uint8_t byte)
{
	driver_cli_puts("hid: ");
	driver_cli_puts(tag);
	driver_cli_hex8(byte);
	driver_cli_puts("\r\n");
}

/* One byte through `op`, bounded: 1 when it is exactly `expected`, 0
 * otherwise (including any error other than -ZOS_EAGAIN). */
static uint8_t hid_wait_byte(uint8_t handle, uint8_t op, uint8_t expected,
			     uint16_t spins)
{
	while (spins != 0u) {
		int16_t byte = driver_cli_getc(handle, op);

		if (byte >= 0) {
			return (uint8_t)((uint8_t)byte == expected);
		}
		if (byte != -ZOS_EAGAIN) {
			return 0u;
		}
		--spins;
	}
	return 0u;
}

void zos_run_hid_selftest(void)
{
	struct zos_ipc_frame frame;
	uint8_t index;

	ace_hid_selftest_result = HID_FAIL_BOOT;

	/* Endpoint discovery: a lookup succeeds only when the endpoint is
	 * registered at ZOS_PROTOCOL_VERSION, which is what v01 records. */
	if (driver_cli_endpoint(ZOS_CLASS_HID, &hid_endpoint) == 0u) {
		hid_fail("boot", HID_FAIL_BOOT, 0x01u);
		return;
	}
	driver_cli_puts("hid: class 04 v01 handle ");
	driver_cli_hex8(hid_endpoint);
	driver_cli_puts("\r\n");
	if (driver_cli_endpoint(ZOS_CLASS_IOC, &ioc_endpoint) == 0u) {
		hid_fail("boot", HID_FAIL_BOOT, 0x02u);
		return;
	}
	driver_cli_puts("hid: ioc class 05 v01 handle ");
	driver_cli_hex8(ioc_endpoint);
	driver_cli_puts("\r\n");

	/* The console input row: the console's GETC reaches this engine
	 * selected its input from at boot. */
	if (driver_cli_getc(hid_endpoint, ZHID_OP_READY) != 1) {
		hid_fail("ready", HID_FAIL_READY, 0x01u);
		return;
	}
	driver_cli_puts("hid: ready 1\r\n");

	/* Typed pass: the replay queued the scripted pass in the MCU's
	 * terminal queue before boot; the doorbell fetch delivers it to the
	 * driver ring, and the byte forms drain it in FIFO order. */
	ace_hid_selftest_result = HID_FAIL_TYPED;
	if (driver_cli_poll(hid_endpoint, ZHID_OP_POLL) != 1) {
		hid_fail("typed", HID_FAIL_TYPED, 0x01u);
		return;
	}
	driver_cli_puts("hid: poll 1\r\n");
	for (index = 0u; index < HID_TYPED_LEN; ++index) {
		int16_t byte = driver_cli_getc(hid_endpoint, ZHID_OP_GETC);

		if (byte < 0 || (uint8_t)byte != hid_typed[index]) {
			hid_fail("typed", HID_FAIL_TYPED, (uint8_t)byte);
			return;
		}
		hid_rx("rx ", (uint8_t)byte);
	}
	if (driver_cli_getc(hid_endpoint, ZHID_OP_GETC) != -ZOS_EAGAIN ||
	    driver_cli_poll(hid_endpoint, ZHID_OP_POLL) != 0) {
		hid_fail("typed", HID_FAIL_TYPED, 0x02u);
		return;
	}
	driver_cli_puts("hid: typed 09\r\n");

	/* Console pass: this image's declared driver set includes the
	 * console endpoint.  The replay queues the pair for the MCU when
	 * this row prints, so the console read drives the HID engine. */
	ace_hid_selftest_result = HID_FAIL_CONSOLE;
	if (driver_cli_endpoint(ZOS_CLASS_CONSOLE, &console_endpoint) == 0u) {
		hid_fail("console", HID_FAIL_CONSOLE, 0x01u);
		return;
	}
	driver_cli_puts("hid: console input\r\n");
	for (index = 0u; index < HID_PAIR_LEN; ++index) {
		if (hid_wait_byte(console_endpoint, ZCONSOLE_OP_GETC,
				  hid_console[index], HID_CONSOLE_SPINS) == 0u) {
			hid_fail("console", HID_FAIL_CONSOLE,
				 hid_console[index]);
			return;
		}
		hid_rx("con rx ", hid_console[index]);
	}
	driver_cli_puts("hid: console OK\r\n");

	/* Fault pass: the replay queues the clean pair behind one short
	 * CMD_HID_INPUT reply.  The driver must deliver nothing from the
	 * malformed reply; after the bounded penalty the unchanged pair
	 * arrives in FIFO order, which is the byte-for-byte atomicity the
	 * replay pins. */
	ace_hid_selftest_result = HID_FAIL_FAULT;
	driver_cli_puts("hid: fault pass\r\n");
	for (index = 0u; index < HID_PAIR_LEN; ++index) {
		if (hid_wait_byte(hid_endpoint, ZHID_OP_GETC,
				  hid_fault[index], HID_FAULT_SPINS) == 0u) {
			hid_fail("fault", HID_FAIL_FAULT, hid_fault[index]);
			return;
		}
		hid_rx("rx ", hid_fault[index]);
	}
	if (driver_cli_getc(hid_endpoint, ZHID_OP_GETC) != -ZOS_EAGAIN) {
		hid_fail("fault", HID_FAIL_FAULT, 0x01u);
		return;
	}
	driver_cli_puts("hid: fault OK\r\n");

	/* Malformed calls: the unknown operation is refused by the driver,
	 * the unissued handles by the nucleus before any dispatch. */
	ace_hid_selftest_result = HID_FAIL_REJECTS;
	driver_cli_frame(&frame, 0x7Fu, 0u, 0u);
	if (driver_cli_call(hid_endpoint, &frame) != -ZOS_EINVAL) {
		hid_fail("rejects", HID_FAIL_REJECTS, 0x01u);
		return;
	}
	driver_cli_frame(&frame, ZHID_OP_GETC, 0u, 0u);
	if (driver_cli_call(0u, &frame) != -ZOS_EINVAL) {
		hid_fail("rejects", HID_FAIL_REJECTS, 0x02u);
		return;
	}
	if (driver_cli_call((uint8_t)(hid_endpoint + 0x80u), &frame) !=
	    -ZOS_EINVAL) {
		hid_fail("rejects", HID_FAIL_REJECTS, 0x03u);
		return;
	}
	driver_cli_puts("hid: rejected calls OK\r\n");

	ace_hid_selftest_result = 0x00u;
	driver_cli_puts("hid selftest: OK\r\n");
}
#endif /* ZOS_HID_SELFTEST */
