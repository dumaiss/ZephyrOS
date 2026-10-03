/* ZephyrOS M8 HID acceptance selftest (test artifact).
 *
 * Runs from the late boot hook after the production-style boot: the SIO
 * fallback, the IOC transport and the HID keyboard module are loaded
 * from the ROMFS recovery store and the console is bound.  The HID
 * module registers only when it discovered the IOC class by
 * class/capability/interface.
 *
 * Control and outcome lines go through the direct early SIO.  The
 * typing and fallback phases deliberately use the bound console service
 * (console_write/console_getc), so the transcript proves the accepted
 * binding: HID input while output remains visible on SIO0/B.
 *
 * The scripted USB-keyboard pass is fixed (the MCU translation of the
 * pinned deployment): 'a', 'B', '3', Enter, Backspace, Ctrl-X and one
 * Arrow-Up (ESC [ A), nine bytes total.  Every received byte is echoed
 * as an `m8 hid: rx XX` line through the console output; the run then
 * forces the input back to the SIO fallback (omitting HID), discards
 * any stale host-sync residue left in the receiver by the smoke tool's
 * repeated banner nudge, and requires one host-sent serial byte through
 * the same service.
 *
 * The no-IOC image (ZOS_M8_HID_NOIOC) proves the discovery half of the
 * dependency contract: HID must be absent (the loader rolled its failed
 * dependency acquisition back) and the SIO fallback must carry the whole
 * console.  The malformed-IOC image (ZOS_M8_HID_BADIOC) proves the
 * interface-verification half: the fixture registers an IOC-class
 * device with a null command operation, HID must reject it without
 * calling through the null pointer, and both the fixture and the SIO
 * fallback must remain usable.
 */

#include "zephyros/console.h"
#include "zephyros/device.h"
#include "zephyros/hid.h"
#include "zephyros/kernel.h"

#include "m8_hid_selftest.h"

#if ZOS_M8_HID_SELFTEST || ZOS_M8_HID_NOIOC || ZOS_M8_HID_BADIOC

uint8_t m8_hid_selftest_result;

/* The MCU's translation of the scripted key pass: printable keys plus
 * Enter, Backspace, Ctrl-X and one VT100 Arrow-Up sequence.  These are
 * translated terminal bytes, not raw USB reports. */
static const uint8_t m8hid_expected[] = {
	0x61u, 0x42u, 0x33u, 0x0Du, 0x08u, 0x18u,
	0x1Bu, 0x5Bu, 0x41u
};
#define M8HID_EXPECTED_LEN 9u

/* Bounded wait per expected byte: long enough for a human to type the
 * next key after the prompt, short enough that a missing byte cannot
 * hang the acceptance image forever (the smoke tool's timeout is the
 * outer bound). */
#define M8HID_WAIT_SPINS 0x00600000u

static const char m8hid_digits[] = "0123456789ABCDEF";

static void m8hid_put_hex8(uint8_t value)
{
	early_console_putc(m8hid_digits[(value >> 4) & 0x0Fu]);
	early_console_putc(m8hid_digits[value & 0x0Fu]);
}

static void m8hid_fail(uint8_t code, uint8_t detail)
{
	static const char prefix[] = "m8 hid selftest: FAIL 0x";
	static const char detail_prefix[] = " detail 0x";

	m8_hid_selftest_result = code;
	early_console_puts(prefix);
	m8hid_put_hex8(code);
	early_console_puts(detail_prefix);
	m8hid_put_hex8(detail);
	early_console_puts("\r\n");
}

/* Progress/echo through the bound console service: this is what proves
 * HID input plus visible SIO output (and, later, the forced SIO input)
 * rather than the independent early-console path. */
static void m8hid_console_puts(const char *text)
{
	uint16_t len = 0u;

	while (text[len] != '\0') {
		++len;
	}
	(void)console_write(text, len);
}

static void m8hid_console_rx(uint8_t value)
{
	char line[20];
	static const char prefix[] = "m8 hid: rx ";
	char *out = line;
	const char *scan;

	for (scan = prefix; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	*out++ = m8hid_digits[(value >> 4) & 0x0Fu];
	*out++ = m8hid_digits[value & 0x0Fu];
	*out++ = '\r';
	*out++ = '\n';
	(void)console_write(line, (uint16_t)(out - line));
}

/* One nonblocking console byte or a bounded-timeout error. */
static int m8hid_wait_byte(void)
{
	uint32_t spin = M8HID_WAIT_SPINS;

	while (spin != 0u) {
		int ch = console_getc();

		if (ch >= 0) {
			return ch;
		}
		if (ch != ZDEV_EAGAIN) {
			return ch;
		}
		--spin;
	}
	return ZDEV_ETIMEDOUT;
}

/* Capability dispatch for the reserved gamepad shape.  The operation
 * must be reported as ZDEV_ENOTSUP from the capability bit (and the
 * null method), never inferred from the device name. */
static int m8hid_gamepad(const struct zdevice *dev, uint8_t index,
			 struct zhid_gamepad_state *out)
{
	const struct zhid_ops *ops;

	if ((dev->capabilities & ZCAP_HID_GAMEPAD) == 0u) {
		return ZDEV_ENOTSUP;
	}
	ops = (const struct zhid_ops *)dev->class_ops;
	if (ops == (const struct zhid_ops *)0 || ops->read_gamepad == 0) {
		return ZDEV_ENOTSUP;
	}
	return ops->read_gamepad(dev->state, index, out);
}

/* Forced SIO fallback: bind the SIO input explicitly (omitting HID),
 * require one host-sent byte through the same console service, and
 * report each step.  Returns 1 on success. */
static uint8_t m8hid_sio_phase(void)
{
	zdev_handle_t sio;
	int ch;
	uint8_t flush;

	sio = device_find(ZDEV_SERIAL, ZCAP_CONSOLE_IN | ZCAP_CONSOLE_OUT,
			  0u);
	if (sio == ZDEV_HANDLE_INVALID) {
		m8hid_fail(ZOS_M8HID_FAIL_FALLBACK_BIND, 0x01u);
		return 0u;
	}
	if (console_bind(sio, console_output_handle()) != ZDEV_OK ||
	    console_input_handle() != sio) {
		m8hid_fail(ZOS_M8HID_FAIL_FALLBACK_BIND, 0x02u);
		return 0u;
	}

	/* The smoke tool repeats the host sync sequence (AAh 55h) until
	 * the banner is visible, so a pair sent while the boot log was
	 * being captured can still sit in the receiver.  Discard that
	 * documented pre-boot residue before inviting the fallback byte:
	 * the assertion must test the invited byte, not a stale sync
	 * byte.  Any other byte before the invitation is still a
	 * failure. */
	for (flush = 0u; flush < 8u; ++flush) {
		ch = console_getc();
		if (ch < 0) {
			break;
		}
		if ((uint8_t)ch != 0xAAu && (uint8_t)ch != 0x55u) {
			m8hid_fail(ZOS_M8HID_FAIL_FALLBACK, (uint8_t)ch);
			return 0u;
		}
	}

	m8hid_console_puts("m8 hid: sio fallback, send R\r\n");
	ch = m8hid_wait_byte();
	if (ch != (int)'R') {
		m8hid_fail(ZOS_M8HID_FAIL_FALLBACK,
			   ch < 0 ? 0x01u : (uint8_t)ch);
		return 0u;
	}
	m8hid_console_puts("m8 hid: sio fallback OK\r\n");
	return 1u;
}

void zos_run_m8_hid_selftest(void)
{
	m8_hid_selftest_result = ZOS_M8HID_FAIL_BOOT;

#if ZOS_M8_HID_NOIOC
	/* Dependency-failure image: HID must not exist.  Its init failed
	 * on the missing IOC class and the loader rolled the (empty)
	 * registration set back. */
	if (device_find(ZDEV_HID, ZCAP_HID_TEXT, 0u) !=
	    ZDEV_HANDLE_INVALID) {
		m8hid_fail(ZOS_M8HID_FAIL_DEPENDENCY, 0x01u);
		return;
	}
	early_console_puts("m8 hid: hid absent after rollback\r\n");
	if (!m8hid_sio_phase()) {
		return;
	}
	m8_hid_selftest_result = ZOS_M8HID_OK;
	early_console_puts("m8 hid selftest: OK\r\n");
#elif ZOS_M8_HID_BADIOC
	/* Malformed-dependency image: the fixture's IOC-class device (with
	 * a null command operation) must have been found and rejected by
	 * HID's interface verification; HID must be rolled back, and the
	 * fixture plus the SIO fallback must be untouched. */
	if (device_find(ZDEV_HID, ZCAP_HID_TEXT, 0u) !=
	    ZDEV_HANDLE_INVALID) {
		m8hid_fail(ZOS_M8HID_FAIL_DEPENDENCY, 0x02u);
		return;
	}
	if (device_find(ZDEV_IOC, ZCAP_IOC_COMMAND | ZCAP_IOC_STATUS, 0u) ==
	    ZDEV_HANDLE_INVALID) {
		m8hid_fail(ZOS_M8HID_FAIL_DEPENDENCY, 0x03u);
		return;
	}
	early_console_puts("m8 hid: malformed ioc rejected\r\n");
	if (!m8hid_sio_phase()) {
		return;
	}
	m8_hid_selftest_result = ZOS_M8HID_OK;
	early_console_puts("m8 hid selftest: OK\r\n");
#else
	{
		zdev_handle_t handle;
		const struct zdevice *dev;
		const void *class_ops = (const void *)0;
		void *state = (void *)0;
		const struct zhid_ops *ops;
		struct zhid_gamepad_state pad;
		uint8_t i;

		/* Exact class/capability discovery: the keyboard must be
		 * found by its class and the complete required set, and the
		 * reserved capabilities must be absent, not inferred. */
		handle = device_find(ZDEV_HID, ZCAP_HID_TEXT |
						     ZCAP_CHAR_IN |
						     ZCAP_CONSOLE_IN, 0u);
		if (handle == ZDEV_HANDLE_INVALID) {
			m8hid_fail(ZOS_M8HID_FAIL_BOOT, 0x01u);
			return;
		}
		dev = device_get(handle);
		if (dev == (const struct zdevice *)0) {
			m8hid_fail(ZOS_M8HID_FAIL_BOOT, 0x02u);
			return;
		}
		if (dev->capabilities != (ZCAP_HID_TEXT | ZCAP_CHAR_IN |
					  ZCAP_CONSOLE_IN)) {
			m8hid_fail(ZOS_M8HID_FAIL_SHAPE, 0x01u);
			return;
		}
		if (device_find(ZDEV_HID, ZCAP_HID_GAMEPAD, 0u) !=
			    ZDEV_HANDLE_INVALID ||
		    device_find(ZDEV_HID, ZCAP_HID_KEY_EVENTS, 0u) !=
			    ZDEV_HANDLE_INVALID ||
		    device_find(ZDEV_HID, ZCAP_CONSOLE_OUT, 0u) !=
			    ZDEV_HANDLE_INVALID ||
		    device_find(ZDEV_HID, ZCAP_CHAR_OUT, 0u) !=
			    ZDEV_HANDLE_INVALID) {
			m8hid_fail(ZOS_M8HID_FAIL_SHAPE, 0x02u);
			return;
		}
		if (dev->char_ops == (const struct zchar_ops *)0 ||
		    dev->char_ops->poll == 0 || dev->char_ops->getc == 0 ||
		    dev->char_ops->putc != 0 || dev->char_ops->write != 0) {
			m8hid_fail(ZOS_M8HID_FAIL_SHAPE, 0x03u);
			return;
		}
		if (device_interface(handle, ZDEV_HID, &class_ops, &state) !=
		    ZDEV_OK) {
			m8hid_fail(ZOS_M8HID_FAIL_SHAPE, 0x04u);
			return;
		}
		ops = (const struct zhid_ops *)class_ops;
		if (ops == (const struct zhid_ops *)0 ||
		    ops->read_text == 0 || ops->read_gamepad != 0) {
			m8hid_fail(ZOS_M8HID_FAIL_SHAPE, 0x05u);
			return;
		}

		/* Absent capability dispatch: ZDEV_ENOTSUP, not a call
		 * through the null method and not a name inference. */
		if (m8hid_gamepad(dev, 0u, &pad) != ZDEV_ENOTSUP) {
			m8hid_fail(ZOS_M8HID_FAIL_UNSUPPORTED, 0x01u);
			return;
		}

		/* Binding policy: HID input preferred, output on the SIO
		 * fallback at this stage. */
		if (console_input_handle() != handle ||
		    console_output_handle() == ZDEV_HANDLE_INVALID) {
			m8hid_fail(ZOS_M8HID_FAIL_BIND, 0x01u);
			return;
		}
		early_console_puts("m8 hid: keyboard0 OK\r\n");

		/* Scripted USB-keyboard pass through the console service. */
		m8hid_console_puts("m8 hid: ready for input\r\n");
		for (i = 0u; i < M8HID_EXPECTED_LEN; ++i) {
			int ch = m8hid_wait_byte();

			if (ch < 0 || (uint8_t)ch != m8hid_expected[i]) {
				m8hid_fail(ZOS_M8HID_FAIL_TYPED,
					   ch < 0 ? 0x01u : (uint8_t)ch);
				return;
			}
			m8hid_console_rx((uint8_t)ch);
		}
		m8hid_console_puts("m8 hid: typed pass OK\r\n");

		/* Forced SIO input fallback: HID omitted. */
		if (!m8hid_sio_phase()) {
			return;
		}
		m8_hid_selftest_result = ZOS_M8HID_OK;
		early_console_puts("m8 hid selftest: OK\r\n");
	}
#endif /* ZOS_M8_HID_NOIOC */
}

#endif /* ZOS_M8_HID_SELFTEST || ZOS_M8_HID_NOIOC || ZOS_M8_HID_BADIOC */
