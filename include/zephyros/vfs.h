/* ZephyrOS M9 filesystem-provider ABI.
 *
 * A filesystem implementation registers a `struct zfs_provider` with the
 * kernel VFS and exposes filesystem behavior through `struct zfs_ops`.
 * This header is module-facing (architecture section 32.11-32.12): it is
 * reachable by ZMOD driver code and must not pull in kernel registry,
 * memory-mapping or process interfaces.  Kernel-only VFS APIs live in
 * kernel/vfs/vfs.h and are never visible to modules.
 *
 * Handles (`fs`, `file`, `dir`) are opaque to VFS.  Providers never learn
 * their external mountpoint: VFS passes a provider-relative path that always
 * begins with '/'.  There is no node object and no `object_id`: VFS keeps no
 * cache and no object identity (architecture section 32.12).
 */

#ifndef ZEPHYROS_VFS_H
#define ZEPHYROS_VFS_H

#include "zephyros/types.h"
#include "zephyros/fs.h"

/* Filesystem operation table (architecture section 32.12).  Providers are
 * **path-based**: every operation receives a provider-relative path instead
 * of a node handle, so VFS keeps no node objects, no node lifetimes and no
 * per-component traversal state.
 *
 * Every path argument obeys the same contract:
 *   - absolute within the provider, beginning with `/` (`/` is the root);
 *   - already normalized by VFS: no `.`, `..`, empty or repeated
 *     components and no trailing `/`;
 *   - NUL-terminated and at most `ZOS_PATH_MAX - 1` bytes;
 *   - matched by the provider with ASCII case-insensitive semantics
 *     (architecture section 32.8); VFS folds only the first component.
 *
 * The 14 operations below are declared in architecture order.  A read-only
 * provider leaves the mutation pointers (write, sync, mkdir, rmdir, unlink,
 * rename) null and VFS turns a null mutation operation into ZOS_EROFS. */
struct zfs_ops {
	/* Instantiate the filesystem from an opaque source string.  On
	 * success stores the provider instance in *fs. */
	int (*mount)(void *provider_state, const char *source, void **fs);

	/* Open `path` and report the current size.  `flags` is the ZFS_O_*
	 * set; M9 uses ZFS_O_READ, ZFS_O_WRITE and ZFS_O_TRUNC.  Stores the
	 * opaque provider file handle in *file. */
	int (*open)(void *fs, const char *path, uint16_t flags, void **file,
		    uint32_t *size);

	int (*close)(void *fs, void *file);
	int (*read)(void *fs, void *file, uint32_t offset, void *buf,
		    uint16_t len);
	int (*write)(void *fs, void *file, uint32_t offset, const void *buf,
		     uint16_t len);
	int (*sync)(void *fs, void *file);

	int (*stat)(void *fs, const char *path, struct zfs_stat *out);

	int (*opendir)(void *fs, const char *path, void **dir);
	/* Writes one entry and returns 1, or returns 0 at end of stream,
	 * or a negative ZOS_E* value. */
	int (*readdir)(void *fs, void *dir, struct zfs_dirent *out);
	int (*closedir)(void *fs, void *dir);

	int (*mkdir)(void *fs, const char *path);
	int (*rmdir)(void *fs, const char *path);
	int (*unlink)(void *fs, const char *path);
	int (*rename)(void *fs, const char *oldpath, const char *newpath);
};

struct zfs_provider {
	const char *name;      /* registry name, folded for comparison */
	const struct zfs_ops *ops;
	void *provider_state;  /* opaque provider-global state         */
};

#define ZFS_OPS_COUNT 14u

#if defined(__SDCC)
_Static_assert(sizeof(struct zfs_ops) == 2u * ZFS_OPS_COUNT,
	       "zfs_ops must be 14 function pointers");

_Static_assert(sizeof(struct zfs_provider) == 6,
	       "zfs_provider must be 6 packed bytes");
_Static_assert(__builtin_offsetof(struct zfs_provider, ops) == 2,
	       "zfs_provider ops offset");
_Static_assert(__builtin_offsetof(struct zfs_provider, provider_state) == 4,
	       "zfs_provider provider_state offset");
#endif /* __SDCC */

#endif /* ZEPHYROS_VFS_H */
