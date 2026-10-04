/* ZephyrOS HID keyboard/text driver module entry (ZMOD `hid`).
 *
 * Production module: it discovers the IOC class through the passed
 * `zdriver_api` only (class + capability + interface; no name lookup, no
 * imported IOC-module symbol), stores the returned class operations and
 * opaque instance state, and registers the single input-only
 * `hid.keyboard0` device.
 *
 * The device advertises exactly ZCAP_HID_TEXT | ZCAP_CHAR_IN |
 * ZCAP_CONSOLE_IN: translated text input plus the generic character
 * input operations the console service binds.  It has no output
 * methods and no output capabilities.  ZCAP_HID_KEY_EVENTS and
 * ZCAP_HID_GAMEPAD are reserved and absent, and `read_gamepad` stays
 * null; a consumer dispatching an unsupported capability must report
 * ZDEV_ENOTSUP instead of inferring support from the name.
 *
 * A missing or malformed IOC dependency returns ZDRIVER_FAIL: the
 * loader rolls back the (empty) registration set, no device is
 * published, and the already loaded SIO fallback and IOC transport are
 * untouched.
 *
 * The descriptor must be a static const object: stack-built descriptors
 * emit SDCC byte-fragmented relocations that ZMOD v1 cannot express and
 * the packer rejects (see docs/module-format.md).
 */

#include "zephyros/driver.h"

#include "hid_driver.h"

static const struct zdevice_desc hid_keyboard0_desc = {
	ZDEV_HID,
	ZCAP_HID_TEXT | ZCAP_CHAR_IN | ZCAP_CONSOLE_IN,
	"hid.keyboard0",
	&hid_char_ops,
	(const void *)&hid_ops,
	(void *)&hid_state
};

int hid_module_init(const struct zdriver_api *api)
{
	if (hid_driver_init(api) != ZDRIVER_OK) {
		return ZDRIVER_FAIL;
	}

	if (api->device_register(&hid_keyboard0_desc, (zdev_handle_t *)0) !=
	    ZDRIVER_OK) {
		return ZDRIVER_FAIL;
	}
	return ZDRIVER_OK;
}
