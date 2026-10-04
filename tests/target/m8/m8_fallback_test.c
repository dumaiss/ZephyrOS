/* ZephyrOS M8 fallback-matrix selftest (test artifact).
 *
 * Shared by the package-20 failure-variant images.  Every variant boots
 * the production load sequence (sio.zmod, ioc.zmod, hid.zmod,
 * v9958.zmod from ROMFS, one final console_bind_defaults()) with one
 * deliberate failure injected through its ROMFS manifest, and this
 * target reports exactly what the surviving stack provides:
 *
 *   - the bound console input and output instances and classes;
 *   - the number of registered devices;
 *   - when the bound input is the serial fallback, one byte invited on
 *     the link, read back through the console service and echoed;
 *   - a marker written through the bound console output (serial-visible
 *     only when that output is the SIO device; on V9958-bound output it
 *     is confirmed on the display by the operator and decoded from the
 *     V9958 trace by the host replay);
 *   - the invariant that the bound devices stayed registered.
 *
 * The expected binding per variant is asserted by
 * tests/test_m8_fallback.py and tools/serial_smoke.py -- never here, so
 * the image cannot claim a direction it did not observe.
 */

#include "zephyros/console.h"
#include "zephyros/device.h"
#include "zephyros/kernel.h"

#include "kernel/device/registry.h"
#include "m8_fallback_selftest.h"

#if ZOS_M8_FALLBACK_SELFTEST

uint8_t m8_fallback_selftest_result;

static const char m8fb_digits[] = "0123456789ABCDEF";

/* Bounded busy wait unit for the invited-byte phase (~16 ms at 10 MHz).
 * The host sends the byte as soon as the invitation appears, so the
 * wait normally consumes it on the first poll. */
static void m8fb_pace(void)
{
	uint16_t i;

	for (i = 0u; i < 0x1FFFu; ++i) {
	}
}

static void m8fb_fail(uint8_t code)
{
	static const char prefix[] = "m8 fallback selftest: FAIL 0x";

	m8_fallback_selftest_result = code;
	early_console_puts(prefix);
	early_console_putc(m8fb_digits[(code >> 4) & 0x0Fu]);
	early_console_putc(m8fb_digits[code & 0x0Fu]);
	early_console_puts("\r\n");
}

static void m8fb_put_hex8(uint8_t value)
{
	early_console_putc(m8fb_digits[(value >> 4) & 0x0Fu]);
	early_console_putc(m8fb_digits[value & 0x0Fu]);
}

static void m8fb_put_dec(uint8_t value)
{
	uint8_t tens = 0u;

	/* Subtraction loop: the freestanding build links no division
	 * helper, matching the kernel's own decimal emitter. */
	while (value >= 10u) {
		value = (uint8_t)(value - 10u);
		++tens;
	}
	if (tens != 0u) {
		early_console_putc((char)('0' + tens));
	}
	early_console_putc((char)('0' + value));
}

static void m8fb_put_name(const char *name)
{
	uint8_t i;

	for (i = 0u; i < 20u && name[i] != '\0'; ++i) {
		early_console_putc(name[i]);
	}
}

static uint8_t m8fb_device_count(void)
{
	uint8_t count = 0u;
	zdev_handle_t handle;

	for (handle = 1u; handle <= ZDEV_MAX_DEVICES; ++handle) {
		if (device_get(handle) != (const struct zdevice *)0) {
			++count;
		}
	}
	return count;
}

void zos_run_m8_fallback_selftest(void)
{
	zdev_handle_t in = console_input_handle();
	zdev_handle_t out = console_output_handle();
	const struct zdevice *dev_in = device_get(in);
	const struct zdevice *dev_out = device_get(out);
	static const char marker[] = "m8 fallback: console output\r\n";
	uint8_t outer;
	int result;

	m8_fallback_selftest_result = ZOS_M8FB_FAIL_BOOT;

	if (in == ZDEV_HANDLE_INVALID || out == ZDEV_HANDLE_INVALID ||
	    dev_in == (const struct zdevice *)0 ||
	    dev_out == (const struct zdevice *)0) {
		m8fb_fail(ZOS_M8FB_FAIL_BOOT);
		return;
	}

	early_console_puts("m8 fallback: input ");
	m8fb_put_name(dev_in->name);
	early_console_puts(" class ");
	m8fb_put_hex8(dev_in->class_id);
	early_console_puts("\r\n");
	early_console_puts("m8 fallback: output ");
	m8fb_put_name(dev_out->name);
	early_console_puts(" class ");
	m8fb_put_hex8(dev_out->class_id);
	early_console_puts("\r\n");
	early_console_puts("m8 fallback: devices ");
	m8fb_put_dec(m8fb_device_count());
	early_console_puts("\r\n");

	/* When the bound input is the serial fallback, invite exactly one
	 * byte, read it through the console service (the same path the
	 * monitor uses), and echo it on the early SIO.  A byte that never
	 * arrives is reported as such, never as a pass. */
	if (dev_in->class_id == ZDEV_SERIAL) {
		early_console_puts("m8 fallback: send input\r\n");
		result = ZDEV_EAGAIN;
		for (outer = 0u; outer < 128u; ++outer) {
			result = console_getc();
			if (result >= 0) {
				break;
			}
			m8fb_pace();
		}
		if (result >= 0) {
			early_console_puts("m8 fallback: input OK 0x");
			m8fb_put_hex8((uint8_t)result);
			early_console_puts("\r\n");
		} else {
			early_console_puts("m8 fallback: input none\r\n");
		}
	}

	/* One marker through the bound console output.  If the output is
	 * the SIO device it appears in this serial transcript; if it is
	 * the V9958 the operator confirms it on the display and the host
	 * replay decodes it from the V9958 trace. */
	result = console_write(marker, (uint16_t)(sizeof(marker) - 1u));
	if (result != (int)(sizeof(marker) - 1u)) {
		m8fb_fail(ZOS_M8FB_FAIL_CONSOLE);
		return;
	}
	early_console_puts("m8 fallback: console write OK\r\n");

	/* The bound devices must have stayed registered through the
	 * rejected/missing-module load path. */
	if (device_get(in) != dev_in || device_get(out) != dev_out ||
	    dev_in->class_id != device_get(in)->class_id ||
	    dev_out->class_id != device_get(out)->class_id) {
		m8fb_fail(ZOS_M8FB_FAIL_BOOT);
		return;
	}

	m8_fallback_selftest_result = ZOS_M8FB_OK;
	early_console_puts("m8 fallback selftest: OK\r\n");
}

#endif /* ZOS_M8_FALLBACK_SELFTEST */
