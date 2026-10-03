/* ZephyrOS M4 loader test fixture: failed initialization.
 *
 * Registers a device and claims IRQ_CTC1 through the driver API, then
 * deliberately returns failure.  The loader must invoke no fini (none is
 * declared), roll back the device registration and the IRQ claim, and
 * zero/release the allocation so a later load can reuse the granule.
 */

#include "zephyros/device.h"
#include "zephyros/driver.h"
#include "zephyros/irq.h"

struct m4_fail_state {
	uint16_t marker;
};

struct m4_fail_state m4_fail_state;

static int m4_fail_noop(void *state, char ch)
{
	(void)state;
	(void)ch;
	return ZDEV_OK;
}

static const struct zchar_ops m4_fail_ops = {
	m4_fail_noop,
	(int (*)(void *, const char *, uint16_t))0,
	(int (*)(void *))0,
	(int (*)(void *))0
};

static const struct zdevice_desc m4_fail_desc = {
	ZDEV_OTHER,
	0,
	"m4.fail",
	&m4_fail_ops,
	(const void *)0,
	&m4_fail_state
};

static void m4_fail_irq(void *context)
{
	(void)context;
}

int m4_fail_init(const struct zdriver_api *api)
{
	if (api->device_register(&m4_fail_desc, (zdev_handle_t *)0) !=
	    ZDRIVER_OK) {
		return ZDRIVER_FAIL;
	}
	if (api->irq_claim(IRQ_CTC1, m4_fail_irq, (void *)0) != IRQ_OK) {
		return ZDRIVER_FAIL;
	}
	return ZDRIVER_FAIL;
}
