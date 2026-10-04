/* ZephyrOS M9 VFS path-based mutation entry points.
 *
 * These are the kernel half of the native filesystem mutation family
 * (`SYS_FS_MKDIR` and friends, wired by package 27).  They exist here so the
 * read-only rule has exactly one home: a provider that leaves a mutation
 * pointer null is read-only and VFS answers ZOS_EROFS (architecture section
 * 32.12).  Cross-mount rename is refused with ZOS_EXDEV and VFS never turns
 * a rename into a copy (section 32.31).
 *
 * `rename` is the only operation that needs two pathnames at once, so it
 * owns the second 128-byte scratch buffer (work/M9-REFERENCE.md).
 */

#include "kernel/vfs/vfs.h"

static char vfs_rename_target[ZOS_PATH_MAX];

/* Resolve a mutation target.  Returns the provider path in *rel (never
 * null), or a negative error.  Mutating the VFS root itself is refused
 * before any provider is consulted. */
static int vfs_mutation_path(const char *path, uint16_t *index,
			     const char **rel, int root_error)
{
	int result = vfs_resolve_path(path, index, rel);

	if (result == VFS_RESOLVED_ROOT) {
		return root_error;
	}
	if (result != VFS_RESOLVED_PROVIDER &&
	    result != VFS_RESOLVED_MOUNTPOINT) {
		return result;
	}
	return 0;
}

int vfs_mkdir(const char *path)
{
	const struct zfs_ops *ops;
	const char *rel;
	uint16_t index;
	int result = vfs_mutation_path(path, &index, &rel, -ZOS_EEXIST);

	if (result != 0) {
		return result;
	}
	ops = vfs_mount_state[index].provider->ops;
	if (ops->mkdir == (void *)0) {
		return -ZOS_EROFS;
	}
	return ops->mkdir(vfs_mount_state[index].fs_state, rel);
}

int vfs_rmdir(const char *path)
{
	const struct zfs_ops *ops;
	const char *rel;
	uint16_t index;
	int result = vfs_mutation_path(path, &index, &rel, -ZOS_EBUSY);

	if (result != 0) {
		return result;
	}
	ops = vfs_mount_state[index].provider->ops;
	if (ops->rmdir == (void *)0) {
		return -ZOS_EROFS;
	}
	return ops->rmdir(vfs_mount_state[index].fs_state, rel);
}

int vfs_unlink(const char *path)
{
	const struct zfs_ops *ops;
	const char *rel;
	uint16_t index;
	int result = vfs_mutation_path(path, &index, &rel, -ZOS_EISDIR);

	if (result != 0) {
		return result;
	}
	ops = vfs_mount_state[index].provider->ops;
	if (ops->unlink == (void *)0) {
		return -ZOS_EROFS;
	}
	return ops->unlink(vfs_mount_state[index].fs_state, rel);
}

int vfs_rename(const char *oldpath, const char *newpath)
{
	const struct zfs_ops *ops;
	const char *old_rel;
	const char *new_rel;
	uint16_t old_index;
	uint16_t new_index;
	int result;

	result = vfs_mutation_path(oldpath, &old_index, &old_rel, -ZOS_EBUSY);
	if (result != 0) {
		return result;
	}
	/* The source path lives in the shared scratch buffer, so the target
	 * goes through the operation-private second one. */
	result = vfs_path_normalize(newpath, vfs_rename_target, ZOS_PATH_MAX);
	if (result != 0) {
		return result;
	}
	result = vfs_mount_resolve(vfs_rename_target, &new_index, &new_rel);
	if (result != 0) {
		return result;
	}
	if (new_index != old_index) {
		return -ZOS_EXDEV; /* VFS never copies across filesystems */
	}
	ops = vfs_mount_state[old_index].provider->ops;
	if (ops->rename == (void *)0) {
		return -ZOS_EROFS;
	}
	return ops->rename(vfs_mount_state[old_index].fs_state, old_rel,
			   new_rel);
}
