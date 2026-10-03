/* ZephyrOS HID dependency-failure fixture (test artifact).
 *
 * Packed as a ZMOD and mapped in place of the real IOC module in the
 * badioc acceptance image's ROMFS (`ioc.zmod` entry -> this module), so
 * the boot path is unchanged: the kernel loads "ioc", then "hid".
 *
 * The fixture registers a ZDEV_IOC device that advertises the IOC
 * command+status capabilities but whose class interface is malformed:
 * `command` is null.  HID must find the device by class/capability
 * alone, verify the interface it receives, reject the null operation
 * instead of calling it, and roll back its (empty) registration set.
 * The fixture device and the already loaded SIO fallback must remain
 * registered and usable.
 *
 * Test artifact only: never part of a production image or manifest.
 */

#include "zephyros/device.h"
#include "zephyros/driver.h"
#include "zephyros/ioc.h"

struct hid_fake_ioc_state {
	uint8_t tag;
};

struct hid_fake_ioc_state hid_fake_ioc_state;

static uint16_t hid_fake_ioc_status(void *state)
{
	(void)state;
	return 0u;
}

/* Malformed on purpose: the capability claims the command operation
 * while the method is absent. */
const struct zioc_ops hid_fake_ioc_ops = {
	(int (*)(void *, const struct zioc_frame *,
		 struct zioc_frame *))0,
	(int (*)(void *, void *, uint16_t))0,
	(int (*)(void *, const void *, uint16_t))0,
	hid_fake_ioc_status
};

static const struct zdevice_desc hid_fake_ioc_desc = {
	ZDEV_IOC,
	ZCAP_IOC_COMMAND | ZCAP_IOC_STATUS,
	"fake.ioc",
	(const struct zchar_ops *)0,
	(const void *)&hid_fake_ioc_ops,
	(void *)&hid_fake_ioc_state
};

int hid_fake_ioc_init(const struct zdriver_api *api)
{
	if (api->device_register(&hid_fake_ioc_desc, (zdev_handle_t *)0) !=
	    ZDRIVER_OK) {
		return ZDRIVER_FAIL;
	}
	return ZDRIVER_OK;
}
