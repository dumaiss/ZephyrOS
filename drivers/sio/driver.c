/* ZephyrOS M3 static SIO0/B driver descriptor (bank 7).
 *
 * Registers exactly one device instance named `sio0.b` and advertises the
 * serial character and console capabilities.  The device is selected by
 * capability, never by this name.
 */

#include "zephyros/driver.h"

#include "sio.h"

static int sio_driver_init(const struct zdriver_api *api)
{
	struct zdevice_desc desc;

	/* Explicitly re-establish the documented 115200 8N1 state; the
	 * sequence is idempotent with respect to early-console setup. */
	sio0b_hw_init();

	desc.class_id = ZDEV_SERIAL;
	desc.capabilities = ZCAP_CHAR_IN | ZCAP_CHAR_OUT |
			    ZCAP_CONSOLE_IN | ZCAP_CONSOLE_OUT;
	desc.name = "sio0.b";
	desc.ops = &sio0b_ops;
	desc.state = &sio0b_state;

	return api->device_register(&desc, (zdev_handle_t *)0);
}

const struct zdriver sio_driver = {
	"sio",
	ZDRIVER_ABI_VERSION,
	sio_driver_init
};
