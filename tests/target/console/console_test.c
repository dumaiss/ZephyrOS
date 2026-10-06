/* ZephyrOS console acceptance selftest (test artifact).
 *
 * Runs from the late boot hook after the production-style boot: SIO +
 * IOC + HID + the V9958 video core (with its 85x26 output terminal
 * renderer behind zchar_ops) are loaded from the recovery store and the
 * console service has bound the default pair.  Control and outcome
 * lines go through the direct early SIO so the serial transcript is
 * independent of the bound console; the visible renderer output is
 * decoded from the V9958 command trace by tests/test_console_sim.py and
 * observed by the operator on hardware.
 *
 * The unchanged M7 `monitor.zex` launcher runs after this test, so the
 * normal monitor session (help, R, dump, modify/readback, rejections,
 * two Q relaunch cycles) executes through this same HID-input /
 * V9958-output binding.
 */

#include "zephyros/console.h"
#include "zephyros/device.h"
#include "zephyros/hid.h"
#include "zephyros/kernel.h"
#include "zephyros/video.h"

#include "kernel/device/registry.h"
#include "console_selftest.h"

#if ZOS_CONSOLE_SELFTEST

uint8_t ace_console_selftest_result;

static const char console_digits[] = "0123456789ABCDEF";

static void console_fail(uint8_t code, uint8_t detail)
{
	static const char prefix[] = "console selftest: FAIL 0x";
	static const char detail_prefix[] = " detail 0x";

	ace_console_selftest_result = code;
	early_console_puts(prefix);
	early_console_putc(console_digits[(code >> 4) & 0x0Fu]);
	early_console_putc(console_digits[code & 0x0Fu]);
	early_console_puts(detail_prefix);
	early_console_putc(console_digits[(detail >> 4) & 0x0Fu]);
	early_console_putc(console_digits[detail & 0x0Fu]);
	early_console_puts("\r\n");
}

/* Bounded instance-name compare/copy over the registry-owned string. */
static uint8_t console_name_is(const char *name, const char *want)
{
	while (*want != '\0') {
		if (*name != *want) {
			return 0u;
		}
		++name;
		++want;
	}
	return (*name == '\0') ? 1u : 0u;
}

static void console_put_name(const char *name)
{
	uint8_t i;

	for (i = 0u; i < 20u && name[i] != '\0'; ++i) {
		early_console_putc(name[i]);
	}
}

static uint8_t console_class_is(zdev_handle_t handle, uint8_t want)
{
	const struct zdevice *dev = device_get(handle);

	if (dev == (const struct zdevice *)0) {
		return 0u;
	}
	return dev->class_id == want ? 1u : 0u;
}

static void console_run(void)
{
	zdev_handle_t in;
	zdev_handle_t out;
	const struct zdevice *dev;
	const struct zdevice *dev_in;
	const struct zchar_ops *out_ops;
	const void *class_ops = (const void *)0;
	void *state = (void *)0;
	const struct zvideo_ops *video_ops;
	static const char visible[] = "console: renderer visible\r\n";
	static const char sgr[] =
		"console: reverse \x1b[7mSGR\x1b[27m normal\r\n";
	static const char report_in[] = "console: input ";
	static const char report_out[] = "console: output ";
	static const char report_class[] = " class 0";
	int rc;

	/* Class + capability discovery: the preferred pair must be a HID
	 * console input and a VIDEO console output. */
	in = device_find(ZDEV_HID, ZCAP_HID_TEXT | ZCAP_CONSOLE_IN, 0u);
	out = device_find(ZDEV_VIDEO, ZCAP_VIDEO_RESET | ZCAP_CONSOLE_OUT,
			  0u);
	if (in == ZDEV_HANDLE_INVALID || out == ZDEV_HANDLE_INVALID) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_BOOT, 0x01u);
		return;
	}
	dev = device_get(out);
	dev_in = device_get(in);
	if (dev == (const struct zdevice *)0 ||
	    dev_in == (const struct zdevice *)0) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_BOOT, 0x02u);
		return;
	}
	if (console_name_is(dev->name, "v9958.0") == 0u ||
	    console_name_is(dev_in->name, "hid.keyboard0") == 0u) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_SHAPE, 0x01u);
		return;
	}

	/* The renderer device shape: the seven VIDEO capabilities plus the
	 * generic character/console output bits and an output-only table. */
	if ((dev->capabilities & (ZCAP_VIDEO_RESET | ZCAP_VIDEO_PRESENT |
				 ZCAP_CHAR_OUT | ZCAP_CONSOLE_OUT)) !=
	    (ZCAP_VIDEO_RESET | ZCAP_VIDEO_PRESENT |
	     ZCAP_CHAR_OUT | ZCAP_CONSOLE_OUT)) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_SHAPE, 0x02u);
		return;
	}
	out_ops = dev->char_ops;
	if (out_ops == (const struct zchar_ops *)0 ||
	    out_ops->putc == 0 || out_ops->write == 0) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_SHAPE, 0x03u);
		return;
	}
	if (out_ops->poll != 0 || out_ops->getc != 0) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_SHAPE, 0x04u);
		return;
	}
	if (device_interface(out, ZDEV_VIDEO, &class_ops, &state) !=
	    ZDEV_OK ||
	    class_ops == (const void *)0 || state == (void *)0) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_SHAPE, 0x05u);
		return;
	}
	video_ops = (const struct zvideo_ops *)class_ops;
	if (video_ops->send == 0) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_SHAPE, 0x06u);
		return;
	}

	/* Default binding: exactly the discovered preferred handles. */
	if (console_input_handle() != in ||
	    console_output_handle() != out) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_BIND, 0x01u);
		return;
	}

	/* Report the selected classes/instances on the early SIO. */
	early_console_puts(report_in);
	console_put_name(dev_in->name);
	early_console_puts(report_class);
	early_console_putc(console_digits[dev_in->class_id & 0x0Fu]);
	early_console_puts("\r\n");
	early_console_puts(report_out);
	console_put_name(dev->name);
	early_console_puts(report_class);
	early_console_putc(console_digits[dev->class_id & 0x0Fu]);
	early_console_puts("\r\n");

	/* A full reset must leave the console renderer usable. */
	if (video_ops->send(state, ZVIDEO_RESET, (const void *)0, 0u) !=
	    ZDEV_OK) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_OPS, 0x01u);
		return;
	}

	/* Console service through the renderer: exact counts, printable
	 * text with CR/LF, single-byte putc, SGR reverse consumption. */
	rc = console_write(visible, (uint16_t)(sizeof(visible) - 1u));
	if (rc != (int)(sizeof(visible) - 1u)) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_OPS, 0x02u);
		return;
	}
	rc = console_write(sgr, (uint16_t)(sizeof(sgr) - 1u));
	if (rc != (int)(sizeof(sgr) - 1u)) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_OPS, 0x03u);
		return;
	}
	if (console_putc('m') != ZDEV_OK ||
	    console_putc('8') != ZDEV_OK ||
	    console_putc('\r') != ZDEV_OK ||
	    console_putc('\n') != ZDEV_OK) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_OPS, 0x04u);
		return;
	}

	/* Inject VIDEO-operation errors: exact codes, no state damage. */
	if (video_ops->send(state, ZVIDEO_PRESENT, (const void *)0, 5u) !=
	    ZDEV_EINVAL) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_OPS, 0x05u);
		return;
	}
	if (video_ops->send(state, 0x40u, (const void *)0, 0u) !=
	    ZDEV_ENOTSUP) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_OPS, 0x06u);
		return;
	}
	if (video_ops->send((void *)0, ZVIDEO_PRESENT, (const void *)0,
			    0u) != ZDEV_ENODEV) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_OPS, 0x07u);
		return;
	}

	/* The other console direction and the whole registry must be
	 * intact after the injected errors and the reset. */
	if (console_input_handle() != in ||
	    console_output_handle() != out) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_DEVICES, 0x01u);
		return;
	}
	if (console_class_is(in, ZDEV_HID) == 0u ||
	    console_class_is(out, ZDEV_VIDEO) == 0u ||
	    device_find(ZDEV_SERIAL, ZCAP_CONSOLE_IN | ZCAP_CONSOLE_OUT,
			0u) == ZDEV_HANDLE_INVALID ||
	    device_find(ZDEV_IOC, ZCAP_IOC_COMMAND, 0u) ==
		    ZDEV_HANDLE_INVALID) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_DEVICES, 0x02u);
		return;
	}

	/* One more visible line through the service after the errors. */
	rc = console_write(visible, (uint16_t)(sizeof(visible) - 1u));
	if (rc != (int)(sizeof(visible) - 1u)) {
		console_fail(ZOS_CONSOLESELFTEST_FAIL_OPS, 0x08u);
		return;
	}

	ace_console_selftest_result = ZOS_CONSOLESELFTEST_OK;
	early_console_puts("console selftest: OK\r\n");
}

void zos_run_console_selftest(void)
{
	ace_console_selftest_result = ZOS_CONSOLESELFTEST_FAIL_BOOT;
	console_run();
}

#endif /* ZOS_CONSOLE_SELFTEST */
