/* ZephyrOS M9 provider-rollback fixture (driver ABI v3, test only).
 *
 * Registers the fresh `failfs` provider and then fails its init.  The
 * loader must roll the registration back, so `vfs_find_provider("failfs")`
 * is null afterwards; a registration leak would make it visible.
 */

#include "zephyros/driver.h"
#include "zephyros/errno.h"
#include "zephyros/vfs.h"

static int fail_mount(void *provider_state, const char *source, void **fs)
{
	(void)provider_state;
	(void)source;
	(void)fs;
	return -ZOS_ENOTSUP;
}

static const struct zfs_ops fail_ops = {
	fail_mount, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const struct zfs_provider fail_provider = {
	"failfs",
	&fail_ops,
	(void *)0
};

int m9_fail_init(const struct zdriver_api *api)
{
	(void)api->fs_register(&fail_provider);
	return ZDRIVER_FAIL;
}
