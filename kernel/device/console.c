/* ZephyrOS console service (M8 binding policy).
 *
 * Binds separate output and input device handles and forwards operations
 * through the device's generic character interface (`struct zchar_ops`).
 * The service never compares driver or instance names; selection is by
 * class plus generic console capability.
 *
 * Kernel panic and early boot deliberately do not use this service; they
 * stay on the common-resident early console.
 */

#include "zephyros/console.h"

#include "kernel/device/registry.h"

zdev_handle_t console_in_handle;
zdev_handle_t console_out_handle;

zdev_handle_t console_input_handle(void)
{
	return console_in_handle;
}

zdev_handle_t console_output_handle(void)
{
	return console_out_handle;
}

void console_unbind(void)
{
	console_in_handle = ZDEV_HANDLE_INVALID;
	console_out_handle = ZDEV_HANDLE_INVALID;
}

static int console_handle_has_capability(zdev_handle_t handle,
					 zdev_caps_t cap)
{
	const struct zdevice *dev = device_get(handle);

	if (dev == (const struct zdevice *)0) {
		return ZDEV_EINVAL;
	}
	return (dev->capabilities & cap) == cap ? ZDEV_OK : ZDEV_EINVAL;
}

int console_bind(zdev_handle_t input, zdev_handle_t output)
{
	if (input != ZDEV_HANDLE_INVALID &&
	    console_handle_has_capability(input, ZCAP_CONSOLE_IN) != ZDEV_OK) {
		return ZDEV_EINVAL;
	}
	if (output != ZDEV_HANDLE_INVALID &&
	    console_handle_has_capability(output, ZCAP_CONSOLE_OUT) != ZDEV_OK) {
		return ZDEV_EINVAL;
	}
	console_in_handle = input;
	console_out_handle = output;
	return ZDEV_OK;
}

/* Preferred input: a HID console-in device; preferred output: a VIDEO
 * console-out device.  Each direction falls back independently to any
 * registered device carrying the matching generic console capability,
 * so one missing direction never clears a valid other direction.
 * Returns ZDEV_OK only when both directions are bound. */
int console_bind_defaults(void)
{
	zdev_handle_t input = device_find(ZDEV_HID, ZCAP_CONSOLE_IN, 0);
	zdev_handle_t output = device_find(ZDEV_VIDEO, ZCAP_CONSOLE_OUT, 0);

	if (input == ZDEV_HANDLE_INVALID) {
		input = device_find(ZDEV_CLASS_ANY, ZCAP_CONSOLE_IN, 0);
	}
	if (output == ZDEV_HANDLE_INVALID) {
		output = device_find(ZDEV_CLASS_ANY, ZCAP_CONSOLE_OUT, 0);
	}
	if (input != ZDEV_HANDLE_INVALID &&
	    console_handle_has_capability(input, ZCAP_CONSOLE_IN) != ZDEV_OK) {
		input = ZDEV_HANDLE_INVALID;
	}
	if (output != ZDEV_HANDLE_INVALID &&
	    console_handle_has_capability(output, ZCAP_CONSOLE_OUT) != ZDEV_OK) {
		output = ZDEV_HANDLE_INVALID;
	}

	console_in_handle = input;
	console_out_handle = output;

	if (input == ZDEV_HANDLE_INVALID || output == ZDEV_HANDLE_INVALID) {
		return ZDEV_ENODEV;
	}
	return ZDEV_OK;
}

int console_putc(char ch)
{
	const struct zdevice *dev = device_get(console_out_handle);

	if (dev == (const struct zdevice *)0) {
		return ZDEV_ENODEV;
	}
	if (dev->char_ops == (const struct zchar_ops *)0 ||
	    dev->char_ops->putc == 0) {
		return ZDEV_ENOTSUP;
	}
	return dev->char_ops->putc(dev->state, ch);
}

int console_write(const char *buf, uint16_t len)
{
	const struct zdevice *dev = device_get(console_out_handle);

	if (dev == (const struct zdevice *)0) {
		return ZDEV_ENODEV;
	}
	if (dev->char_ops == (const struct zchar_ops *)0 ||
	    dev->char_ops->write == 0) {
		return ZDEV_ENOTSUP;
	}
	return dev->char_ops->write(dev->state, buf, len);
}

int console_poll(void)
{
	const struct zdevice *dev = device_get(console_in_handle);

	if (dev == (const struct zdevice *)0) {
		return ZDEV_ENODEV;
	}
	if (dev->char_ops == (const struct zchar_ops *)0 ||
	    dev->char_ops->poll == 0) {
		return ZDEV_ENOTSUP;
	}
	return dev->char_ops->poll(dev->state);
}

int console_getc(void)
{
	const struct zdevice *dev = device_get(console_in_handle);

	if (dev == (const struct zdevice *)0) {
		return ZDEV_ENODEV;
	}
	if (dev->char_ops == (const struct zchar_ops *)0 ||
	    dev->char_ops->getc == 0) {
		return ZDEV_ENOTSUP;
	}
	return dev->char_ops->getc(dev->state);
}
