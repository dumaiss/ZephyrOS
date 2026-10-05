/* ZephyrOS M9 duplicate-registration fixture (driver ABI v3, test only).
 *
 * Tries to register the provider name `probe`, which the resident m9_probe
 * fixture already owns.  `fs_register` returns -ZOS_EEXIST, so the init
 * returns non-zero, the loader rolls this module back, and the original
 * `probe` registration is left untouched.
 */

#include "zephyros/driver.h"
#include "zephyros/errno.h"
#include "zephyros/vfs.h"

static int dup_mount(void *provider_state, const char *source, void **fs)
{
	(void)provider_state;
	(void)source;
	(void)fs;
	return -ZOS_ENOTSUP;
}

static const struct zfs_ops dup_ops = {
	dup_mount, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const struct zfs_provider dup_provider = {
	"probe",
	&dup_ops,
	(void *)0
};

int m9_dup_init(const struct zdriver_api *api)
{
	/* -ZOS_EEXIST is non-zero, so the loader treats this as a failed
	 * init and rolls the (empty) registration set back. */
	return api->fs_register(&dup_provider);
}
