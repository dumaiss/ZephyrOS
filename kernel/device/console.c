/* ZephyrOS M3 console service.
 *
 * Binds separate output and input device handles and forwards operations
 * to the registered device.  Binding is capability-based: the service
 * never compares driver or instance names.  Operations fail cleanly when
 * unbound (ZDEV_ENODEV) or when the bound device does not implement the
 * operation (ZDEV_ENOTSUP).
 *
 * Kernel panic and early boot deliberately do not use this service; they
 * stay on the common-resident early console.
 */

#include "zephyros/console.h"

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

static int console_handle_has_capability(zdev_handle_t handle, uint16_t cap)
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

int console_bind_defaults(void)
{
	zdev_handle_t input = device_find_capability(ZCAP_CONSOLE_IN, 0);
	zdev_handle_t output = device_find_capability(ZCAP_CONSOLE_OUT, 0);

	if (input == ZDEV_HANDLE_INVALID || output == ZDEV_HANDLE_INVALID) {
		console_unbind();
		return ZDEV_ENODEV;
	}
	return console_bind(input, output);
}

int console_putc(char ch)
{
	const struct zdevice *dev = device_get(console_out_handle);

	if (dev == (const struct zdevice *)0) {
		return ZDEV_ENODEV;
	}
	if (dev->ops == (const struct zdevice_ops *)0 || dev->ops->putc == 0) {
		return ZDEV_ENOTSUP;
	}
	return dev->ops->putc(dev->state, ch);
}

int console_write(const char *buf, uint16_t len)
{
	const struct zdevice *dev = device_get(console_out_handle);

	if (dev == (const struct zdevice *)0) {
		return ZDEV_ENODEV;
	}
	if (dev->ops == (const struct zdevice_ops *)0 || dev->ops->write == 0) {
		return ZDEV_ENOTSUP;
	}
	return dev->ops->write(dev->state, buf, len);
}

int console_poll(void)
{
	const struct zdevice *dev = device_get(console_in_handle);

	if (dev == (const struct zdevice *)0) {
		return ZDEV_ENODEV;
	}
	if (dev->ops == (const struct zdevice_ops *)0 || dev->ops->poll == 0) {
		return ZDEV_ENOTSUP;
	}
	return dev->ops->poll(dev->state);
}

int console_getc(void)
{
	const struct zdevice *dev = device_get(console_in_handle);

	if (dev == (const struct zdevice *)0) {
		return ZDEV_ENODEV;
	}
	if (dev->ops == (const struct zdevice_ops *)0 || dev->ops->getc == 0) {
		return ZDEV_ENOTSUP;
	}
	return dev->ops->getc(dev->state);
}
