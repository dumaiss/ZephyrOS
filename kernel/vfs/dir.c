/* ZephyrOS M9 VFS directory API.
 *
 * `readdir("/")` is a VFS operation: the visible root merges the configured
 * root-level mountpoints with the entries of the filesystem attached at `/`
 * (architecture section 32.7).  It first yields each mounted root-level
 * mountpoint as a DIR entry, then the `/` provider's entries, skipping any
 * whose name equals a mountpoint (case-insensitive) so the mountpoint
 * shadows the backing entry.  With no `/` mounted the mountpoint list alone
 * is still a complete root enumeration (section 32.32.6).
 *
 * A provider directory is opened with its provider-relative path, so a
 * mountpoint such as `/SYSTEM` enumerates through `/` inside its own
 * filesystem.
 */

#include "kernel/vfs/vfs.h"

/* True when `name` (from the `/` filesystem) is shadowed by a mounted
 * root-level mountpoint; comparison is ASCII case-insensitive. */
static uint8_t vfs_root_name_is_mountpoint(const char *name)
{
	uint16_t i;

	for (i = 0; i < vfs_mount_count; ++i) {
		if (vfs_mount_state[i].state != VFS_MOUNT_MOUNTED ||
		    !vfs_mount_is_root_level(i) || vfs_mount_is_root(i)) {
			continue;
		}
		if (vfs_name_equal(name, &vfs_mount_table[i].path[1])) {
			return 1;
		}
	}
	return 0;
}

static void vfs_fill_mount_dirent(uint16_t mount_index,
				  struct zfs_dirent *out)
{
	const char *name = &vfs_mount_table[mount_index].path[1];
	uint16_t n = 0;

	out->type = ZFS_TYPE_DIR;
	out->attributes = 0;
	out->size = 0;
	out->ctime = ZFS_TIME_UNKNOWN;
	out->mtime = ZFS_TIME_UNKNOWN;
	while (name[n] != '\0' && n + 1u < ZFS_NAME_MAX) {
		out->name[n] = name[n];
		++n;
	}
	out->name[n] = '\0';
	out->name_length = (uint16_t)(n + 1u);
}

int vfs_opendir(const char *path, struct vfs_dir *dir)
{
	const struct zfs_ops *ops;
	const char *rel;
	uint16_t index;
	uint16_t i;
	void *handle = (void *)0;
	int result;

	if (dir == (struct vfs_dir *)0) {
		return -ZOS_EINVAL;
	}
	result = vfs_resolve_path(path, &index, &rel);
	if (result == VFS_RESOLVED_ROOT) {
		void *root_handle = (void *)0;

		dir->mount = (struct vfs_mount_runtime *)0;
		dir->handle = (void *)0;
		dir->index = 0;
		dir->kind = VFS_DIR_KIND_ROOT;
		dir->merge = 0;
		dir->in_use = 1;
		/* Open the `/` filesystem's own root for the merge half.
		 * It is best-effort: with no `/` mounted (or a provider
		 * that cannot open its root) the mountpoint list is still
		 * a valid root enumeration. */
		for (i = 0; i < vfs_mount_count; ++i) {
			if (vfs_mount_state[i].state != VFS_MOUNT_MOUNTED ||
			    !vfs_mount_is_root(i)) {
				continue;
			}
			if (vfs_mount_state[i].provider->ops->opendir(
				    vfs_mount_state[i].fs_state, "/",
				    &root_handle) == 0) {
				dir->mount = &vfs_mount_state[i];
				dir->handle = root_handle;
			}
			break;
		}
		return 0;
	}
	if (result != VFS_RESOLVED_PROVIDER &&
	    result != VFS_RESOLVED_MOUNTPOINT) {
		return result;
	}
	ops = vfs_mount_state[index].provider->ops;
	result = ops->opendir(vfs_mount_state[index].fs_state, rel, &handle);
	if (result != 0) {
		return result;
	}
	dir->mount = &vfs_mount_state[index];
	dir->handle = handle;
	dir->index = 0;
	dir->kind = VFS_DIR_KIND_PROVIDER;
	dir->merge = 0;
	dir->in_use = 1;
	return 0;
}

int vfs_readdir(struct vfs_dir *dir, struct zfs_dirent *out)
{
	if (dir == (struct vfs_dir *)0 || dir->in_use == 0 ||
	    out == (struct zfs_dirent *)0) {
		return -ZOS_EINVAL;
	}
	if (dir->kind == VFS_DIR_KIND_ROOT) {
		if (dir->merge == 0) {
			while (dir->index < vfs_mount_count) {
				uint16_t i = dir->index;

				dir->index = (uint16_t)(i + 1u);
				if (vfs_mount_state[i].state ==
					    VFS_MOUNT_MOUNTED &&
				    vfs_mount_is_root_level(i) &&
				    !vfs_mount_is_root(i)) {
					vfs_fill_mount_dirent(i, out);
					return 1;
				}
			}
			dir->merge = 1;
		}
		if (dir->mount == (struct vfs_mount_runtime *)0 ||
		    dir->handle == (void *)0) {
			return 0; /* no `/` filesystem: mountpoints only */
		}
		for (;;) {
			int result = dir->mount->provider->ops->readdir(
				dir->mount->fs_state, dir->handle, out);

			if (result != 1) {
				return result; /* 0 at end, or negative */
			}
			if (!vfs_root_name_is_mountpoint(out->name)) {
				return 1;
			}
		}
	}
	if (dir->kind == VFS_DIR_KIND_PROVIDER) {
		return dir->mount->provider->ops->readdir(
			dir->mount->fs_state, dir->handle, out);
	}
	return -ZOS_EINVAL;
}

int vfs_closedir(struct vfs_dir *dir)
{
	int result = 0;

	if (dir == (struct vfs_dir *)0 || dir->in_use == 0) {
		return -ZOS_EINVAL;
	}
	if (dir->mount != (struct vfs_mount_runtime *)0 &&
	    dir->handle != (void *)0) {
		result = dir->mount->provider->ops->closedir(
			dir->mount->fs_state, dir->handle);
	}
	dir->mount = (struct vfs_mount_runtime *)0;
	dir->handle = (void *)0;
	dir->kind = VFS_DIR_KIND_NONE;
	dir->merge = 0;
	dir->in_use = 0;
	return result;
}
