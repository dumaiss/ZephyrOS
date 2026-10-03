/* ZephyrOS SIO init-failure ZMOD entry (test artifact).
 *
 * Module entry for the `sio-fail` rollback image.  It registers the real
 * `sio0.b` device and claims CTC1 through the module API, then returns
 * failure so the loader transaction rollback can be verified: no device,
 * no IRQ claim, no descriptor, and no granule may survive.
 *
 * The production module (drivers/sio/sio_module.c) contains none of this
 * logic; this entry is packed only into build/modules/sio-fail.zmod.
 */

#include "zephyros/driver.h"
#include "zephyros/platform.h"

#include "sio.h"

static const struct zdevice_desc sio0b_desc = {
	ZDEV_SERIAL,
	ZCAP_CHAR_IN | ZCAP_CHAR_OUT | ZCAP_CONSOLE_IN | ZCAP_CONSOLE_OUT,
	"sio0.b",
	&sio0b_ops,
	(const void *)0,
	&sio0b_state
};

static void sio_fail_irq_handler(void *context)
{
	(void)context;
}

int sio_fail_module_init(const struct zdriver_api *api)
{
	sio0b_hw_init();

	if (api->device_register(&sio0b_desc, (zdev_handle_t *)0) !=
	    ZDRIVER_OK) {
		return ZDRIVER_FAIL;
	}
	if (api->irq_claim(IRQ_CTC1, sio_fail_irq_handler, (void *)0) !=
	    IRQ_OK) {
		return ZDRIVER_FAIL;
	}
	/* Registration and claim both succeeded; fail anyway so the kernel
	 * must roll the whole attempt back. */
	return ZDRIVER_FAIL;
}
