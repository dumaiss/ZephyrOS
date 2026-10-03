/* Test-only static driver fixtures (ZOS_M3_SELFTEST build only).
 *
 *   1. m3_partial_driver registers two devices and then deliberately
 *      fails.  driver_init_static() must roll back exactly those two
 *      entries (checkpoint/rollback transaction) and keep earlier
 *      devices intact.
 *   2. m3_bad_abi_driver advertises ABI version 2.  It must be rejected
 *      before its init runs, so its device never appears.
 *   3. m3_fixture_ops / m3_fixture_caponly_ops give the registry tests a
 *      valid ops table and one whose function members are all null (to
 *      exercise the console service "operation unsupported" errors).
 */

#include "zephyros/driver.h"

static int m3_fixture_putc(void *state, char ch)
{
	(void)state;
	(void)ch;
	return ZDEV_OK;
}

static int m3_fixture_write(void *state, const char *buf, uint16_t len)
{
	(void)state;
	(void)buf;
	return (int)len;
}

static int m3_fixture_poll(void *state)
{
	(void)state;
	return 0;
}

static int m3_fixture_getc(void *state)
{
	(void)state;
	return ZDEV_EAGAIN;
}

const struct zdevice_ops m3_fixture_ops = {
	m3_fixture_putc,
	m3_fixture_write,
	m3_fixture_poll,
	m3_fixture_getc
};

const struct zdevice_ops m3_fixture_caponly_ops = {
	0, 0, 0, 0
};

/* --- partial-init rollback fixture ------------------------------------- */

static int m3_partial_driver_init(const struct zdriver_api *api)
{
	struct zdevice_desc desc;

	desc.class_id = ZDEV_OTHER;
	desc.capabilities = 0;
	desc.ops = &m3_fixture_ops;
	desc.state = (void *)0;

	desc.name = "test.partial.a";
	if (api->device_register(&desc, (zdev_handle_t *)0) != ZDEV_OK) {
		return ZDRIVER_FAIL;
	}
	desc.name = "test.partial.b";
	if (api->device_register(&desc, (zdev_handle_t *)0) != ZDEV_OK) {
		return ZDRIVER_FAIL;
	}
	/* Both registrations succeeded; fail anyway so the kernel must
	 * roll the attempt back. */
	return ZDRIVER_FAIL;
}

const struct zdriver m3_partial_driver = {
	"test.partial",
	ZDRIVER_ABI_VERSION,
	m3_partial_driver_init
};

/* --- bad-ABI fixture ---------------------------------------------------- */

static uint8_t m3_bad_abi_init_run;

static int m3_bad_abi_driver_init(const struct zdriver_api *api)
{
	struct zdevice_desc desc;

	m3_bad_abi_init_run = 1;
	desc.class_id = ZDEV_OTHER;
	desc.capabilities = 0;
	desc.name = "test.badabi";
	desc.ops = &m3_fixture_ops;
	desc.state = (void *)0;
	return api->device_register(&desc, (zdev_handle_t *)0) == ZDEV_OK
		       ? ZDRIVER_OK
		       : ZDRIVER_FAIL;
}

uint8_t m3_bad_abi_init_called(void)
{
	return m3_bad_abi_init_run;
}

const struct zdriver m3_bad_abi_driver = {
	"test.badabi",
	2u, /* deliberately not ZDRIVER_ABI_VERSION */
	m3_bad_abi_driver_init
};
