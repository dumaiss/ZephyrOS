/* ZephyrOS SIO0/B driver module entry (ZMOD `sio`).
 *
 * Production module: the `sio0.b` device (class, capabilities, polling
 * ops semantics) is linked at base zero and loaded into the bank-7 driver
 * arena by the M4 loader.  The module reaches the kernel only through the
 * passed `zdriver_api` (device_register / irq_claim / irq_release); it
 * contains no kernel symbol references.  Raw port I/O (sio_io.asm) and
 * the private state (sio0b_state, BSS) stay inside the module image.
 *
 * The IRQ self-test and init-failure variants are test artifacts: their
 * module entries live under tests/modules/ (sio_irq_module.c and
 * sio_fail_module.c) and are never compiled into this production module.
 *
 * The descriptor must be a static const object: stack-built descriptors
 * emit SDCC byte-fragmented relocations that ZMOD v1 cannot express and
 * the packer rejects (see docs/module-format.md).
 */

#include "zephyros/driver.h"

#include "sio.h"

static const struct zdevice_desc sio0b_desc = {
	ZDEV_SERIAL,
	ZCAP_CHAR_IN | ZCAP_CHAR_OUT | ZCAP_CONSOLE_IN | ZCAP_CONSOLE_OUT,
	"sio0.b",
	&sio0b_ops,
	(const void *)0,
	&sio0b_state
};

int sio_module_init(const struct zdriver_api *api)
{
	sio0b_hw_init();

	if (api->device_register(&sio0b_desc, (zdev_handle_t *)0) !=
	    ZDRIVER_OK) {
		return ZDRIVER_FAIL;
	}
	return ZDRIVER_OK;
}
