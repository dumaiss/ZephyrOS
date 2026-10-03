/* ZephyrOS M3 target-test operation tables (test artifact).
 *
 * m3_fixture_ops gives the registry tests a valid ops table;
 * m3_fixture_caponly_ops has all function members null so the console
 * service "operation unsupported" errors can be exercised.  The static
 * driver descriptors that used to live here were retired with the M3
 * static-driver table.
 */

#include "zephyros/device.h"

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

const struct zchar_ops m3_fixture_ops = {
	m3_fixture_putc,
	m3_fixture_write,
	m3_fixture_poll,
	m3_fixture_getc
};

const struct zchar_ops m3_fixture_caponly_ops = {
	0, 0, 0, 0
};
