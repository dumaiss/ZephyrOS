/* ZephyrOS M8 IOC command-lane acceptance selftest (test artifact).
 *
 * Runs from the late boot hook after the production-style boot: the SIO
 * fallback and the IOC command lane are loaded from the ROMFS recovery
 * store, the console is bound, and `ioc.0` is registered only because
 * its CMD_LINK_SYNC bring-up produced a CRC-verified reply.
 *
 * The test reports through the direct early SIO (independent of the
 * registry and the console service), performs repeated PING exchanges
 * with an echo pattern, samples the semantic HID status, and records a
 * result code for the host replay.  It must not copy the historical
 * transport-document banner (firmware level 23 / host level 07h): the
 * pinned deployment answers 74 (4Ah) and this host implements contract
 * level 08h.
 */

#include "zephyros/device.h"
#include "zephyros/ioc.h"
#include "zephyros/kernel.h"

#include "ioc_proto.h"
#include "m8_ioc_selftest.h"

#if ZOS_M8_IOC_COMMAND_SELFTEST

uint8_t m8_ioc_selftest_result;

static const char m8ioc_digits[] = "0123456789ABCDEF";

#define M8IOC_PINGS 4u
#define M8IOC_STATUS_SAMPLES 8u

static void m8ioc_put_hex8(uint8_t value)
{
	early_console_putc(m8ioc_digits[(value >> 4) & 0x0Fu]);
	early_console_putc(m8ioc_digits[value & 0x0Fu]);
}

static void m8ioc_fail(uint8_t code, uint8_t detail)
{
	static const char prefix[] = "m8 ioc selftest: FAIL 0x";
	static const char detail_prefix[] = " detail 0x";

	m8_ioc_selftest_result = code;
	early_console_puts(prefix);
	m8ioc_put_hex8(code);
	early_console_puts(detail_prefix);
	m8ioc_put_hex8(detail);
	early_console_puts("\r\n");
}

void zos_run_m8_ioc_selftest(void)
{
	const struct zioc_ops *ops;
	const void *class_ops;
	void *state;
	zdev_handle_t handle;
	struct zioc_frame request;
	struct zioc_frame reply;
	uint8_t level = 0u;
	uint8_t pass;
	uint8_t i;
	int result;
	uint16_t bits;

	m8_ioc_selftest_result = ZOS_M8IOC_FAIL_BOOT;

	/* The implemented host contract is the pinned deployed host level:
	 * this is an identifier the diagnostic prints and the smoke tool
	 * checks - not a firmware request. */
	early_console_puts("m8 ioc: transport level ");
	m8ioc_put_hex8(ZIOC_TRANSPORT_LEVEL);
	early_console_puts("\r\n");

	/* Discovery is by class plus capabilities; names are diagnostic
	 * only.  The bulk capabilities arrive with the next package. */
	handle = device_find(ZDEV_IOC, ZCAP_IOC_COMMAND | ZCAP_IOC_STATUS, 0u);
	if (handle == ZDEV_HANDLE_INVALID) {
		m8ioc_fail(ZOS_M8IOC_FAIL_BOOT, 0x01u);
		return;
	}
	if (device_interface(handle, ZDEV_IOC, &class_ops, &state) !=
	    ZDEV_OK) {
		m8ioc_fail(ZOS_M8IOC_FAIL_BOOT, 0x02u);
		return;
	}
	ops = (const struct zioc_ops *)class_ops;
	if (ops == (const struct zioc_ops *)0 || ops->command == 0 ||
	    ops->status == 0 || ops->bulk_read != 0 || ops->bulk_write != 0) {
		m8ioc_fail(ZOS_M8IOC_FAIL_BOOT, 0x03u);
		return;
	}

	/* Repeated PING: identical echo pattern every time, and the
	 * persistent link must survive without re-initialisation. */
	for (pass = 0; pass < M8IOC_PINGS; ++pass) {
		request.type = IOC_CMD_PING;
		request.status = 0u;
		request.length = IOC_PING_ECHO_BYTES;
		for (i = 0; i < IOC_PING_ECHO_BYTES; ++i) {
			request.data[i] = (uint8_t)(i ^ 0x5Au);
		}

		result = ops->command(state, &request, &reply);
		if (result != ZIOC_OK) {
			m8ioc_fail(ZOS_M8IOC_FAIL_PING, (uint8_t)(0x10u + pass));
			return;
		}
		if (reply.type != IOC_RSP_PING ||
		    reply.length != IOC_PING_DATA_LEN) {
			m8ioc_fail(ZOS_M8IOC_FAIL_PING,
				   (uint8_t)(0x20u + pass));
			return;
		}
		if (reply.status != 0u) {
			m8ioc_fail(ZOS_M8IOC_FAIL_PING,
				   (uint8_t)(0x30u + pass));
			return;
		}
		for (i = 0; i < IOC_PING_ECHO_BYTES; ++i) {
			if (reply.data[i] != request.data[i]) {
				m8ioc_fail(ZOS_M8IOC_FAIL_ECHO, i);
				return;
			}
		}

		/* Controller firmware level: printed once, then validated
		 * on every reply. */
		if (pass == 0u) {
			level = reply.data[IOC_PING_DATA_LEVEL];
			early_console_puts("m8 ioc: controller level ");
			m8ioc_put_hex8(level);
			early_console_puts("\r\n");
		}
		if (reply.data[IOC_PING_DATA_LEVEL] != level) {
			m8ioc_fail(ZOS_M8IOC_FAIL_REPEAT, pass);
			return;
		}

		/* The MCU must already report the command lane established:
		 * this PING is itself a CRC-verified exchange after the
		 * LINK_SYNC bring-up. */
		if ((reply.data[IOC_PING_DATA_POWER] &
		     IOC_PING_LINK_SYNCED) == 0u) {
			m8ioc_fail(ZOS_M8IOC_FAIL_PING,
				   (uint8_t)(0x40u + pass));
			return;
		}
	}

	if (level != IOC_FW_LEVEL_PINNED) {
		m8ioc_fail(ZOS_M8IOC_FAIL_LEVEL, level);
		return;
	}
	early_console_puts("m8 ioc: ping x4 OK\r\n");

	/* Semantic status only: the returned mask may carry nothing but
	 * ZIOC_STATUS_HID_PENDING (and the doorbell varies with whatever
	 * the controller has queued, so both values are legal). */
	for (i = 0; i < M8IOC_STATUS_SAMPLES; ++i) {
		bits = ops->status(state);
		if ((bits & (uint16_t)~ZIOC_STATUS_HID_PENDING) != 0u) {
			m8ioc_fail(ZOS_M8IOC_FAIL_STATUS, (uint8_t)bits);
			return;
		}
	}
	early_console_puts("m8 ioc: hid status ");
	m8ioc_put_hex8((uint8_t)(bits & 0xFFu));
	early_console_puts("\r\n");

	m8_ioc_selftest_result = ZOS_M8IOC_OK;
	early_console_puts("m8 ioc selftest: OK\r\n");
}

#endif /* ZOS_M8_IOC_COMMAND_SELFTEST */
