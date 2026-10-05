/* ZephyrOS M9 provider-registration fixture (driver ABI v3, test only).
 *
 * Registers the `probe` provider during the module-loading phase and stays
 * resident, so the target acceptance test can prove that a module-registered
 * provider is visible to VFS and that a duplicate registration is rejected.
 */

#include "zephyros/driver.h"
#include "zephyros/errno.h"
#include "zephyros/vfs.h"

static int probe_mount(void *provider_state, const char *source, void **fs)
{
	(void)provider_state;
	(void)source;
	(void)fs;
	return -ZOS_ENOTSUP;
}

static const struct zfs_ops probe_ops = {
	probe_mount, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const struct zfs_provider probe_provider = {
	"probe",
	&probe_ops,
	(void *)0
};

int m9_probe_init(const struct zdriver_api *api)
{
	return api->fs_register(&probe_provider);
}
