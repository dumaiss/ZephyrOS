/* ZephyrOS IOC driver module entry (ZMOD `ioc`).
 *
 * Production module: it programs SIO1/B once, performs the bounded
 * CMD_LINK_SYNC bring-up, and only then registers the `ioc.0` device
 * with the command and semantic-status capabilities.  Bulk read/write
 * arrive with the next package; their capability bits are absent and
 * their ops pointers are null.
 *
 * The module reaches the kernel only through the passed `zdriver_api`;
 * it contains no kernel symbol references.  Port I/O and the streaming
 * loops live in ioc_io.asm; framing/CRC/validation/state in ioc.c.
 *
 * A failed bring-up returns ZDRIVER_FAIL: the loader rolls back the
 * (empty) registration set, no device is published, and the already
 * loaded SIO fallback console is unaffected.
 *
 * The descriptor must be a static const object: stack-built descriptors
 * emit SDCC byte-fragmented relocations that ZMOD v1 cannot express and
 * the packer rejects (see docs/module-format.md).
 */

#include "zephyros/driver.h"
#include "zephyros/platform.h"

#include "ioc_driver.h"

static const struct zdevice_desc ioc0_desc = {
	ZDEV_IOC,
	ZCAP_IOC_COMMAND | ZCAP_IOC_STATUS,
	"ioc.0",
	(const struct zchar_ops *)0,
	(const void *)&ioc_ops,
	(void *)&ioc_state
};

int ioc_module_init(const struct zdriver_api *api)
{
	ioc_hw_init();

	if (ioc_link_bringup() != ZIOC_OK) {
		return ZDRIVER_FAIL;
	}

	if (api->device_register(&ioc0_desc, (zdev_handle_t *)0) !=
	    ZDRIVER_OK) {
		return ZDRIVER_FAIL;
	}
	return ZDRIVER_OK;
}
