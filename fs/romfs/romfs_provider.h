/* ZephyrOS M9 ROMFS filesystem-provider registration (kernel-private).
 *
 * The provider is linked into the kernel image (it is not a ZMOD) so it is
 * available during VFS bootstrap, before any module loads.  Only
 * kernel/vfs and the built-in registration table reference this symbol.
 */

#ifndef ZEPHYROS_FS_ROMFS_PROVIDER_H
#define ZEPHYROS_FS_ROMFS_PROVIDER_H

#include "zephyros/vfs.h"

/* Registered by kernel/vfs during bootstrap; source must be "recovery". */
extern const struct zfs_provider romfs_provider;

#endif /* ZEPHYROS_FS_ROMFS_PROVIDER_H */
