/* ZephyrOS M9 VFS kernel file API and the loader's byte source.
 *
 * Open/read/write/sync/close/stat operate on caller-provided `struct
 * vfs_file` storage: this package keeps no descriptor pool (package 27 adds
 * the process-visible one).  A path is normalized, its owning mount is
 * selected by the first component and the provider receives the
 * provider-relative path, so VFS never walks components and keeps no node
 * object.
 *
 * Operations a read-only provider leaves null report ZOS_EROFS here, which
 * is the single place the read-only rule is applied (architecture section
 * 32.12).
 */

#include "kernel/vfs/vfs.h"

static void vfs_fill_stat(struct zfs_stat *out, uint8_t type, uint32_t size)
{
	out->type = type;
	out->attributes = 0;
	out->reserved = 0;
	out->size = size;
	out->ctime = ZFS_TIME_UNKNOWN;
	out->mtime = ZFS_TIME_UNKNOWN;
}

int vfs_open(const char *path, uint16_t flags, struct vfs_file *file)
{
	const struct zfs_ops *ops;
	const char *rel;
	uint16_t index;
	void *handle = (void *)0;
	uint32_t size = 0;
	int result;

	if (file == (struct vfs_file *)0) {
		return -ZOS_EINVAL;
	}
	if ((flags & ~(uint16_t)(ZFS_O_READ | ZFS_O_WRITE | ZFS_O_CREATE |
				 ZFS_O_EXCL | ZFS_O_TRUNC | VFS_O_EXEC)) != 0) {
		return -ZOS_EINVAL;
	}
	/* TRUNC/CREATE/EXCL are write operations, and EXCL only means
	 * anything together with CREATE. */
	if ((flags & (uint16_t)(ZFS_O_TRUNC | ZFS_O_CREATE | ZFS_O_EXCL)) != 0 &&
	    (flags & ZFS_O_WRITE) == 0) {
		return -ZOS_EINVAL;
	}
	if ((flags & ZFS_O_EXCL) != 0 && (flags & ZFS_O_CREATE) == 0) {
		return -ZOS_EINVAL;
	}
	result = vfs_resolve_path(path, &index, &rel);
	if (result == VFS_RESOLVED_ROOT || result == VFS_RESOLVED_MOUNTPOINT) {
		return -ZOS_EISDIR; /* a directory is not a file to open */
	}
	if (result != VFS_RESOLVED_PROVIDER) {
		return result;
	}
	ops = vfs_mount_state[index].provider->ops;
	result = ops->open(vfs_mount_state[index].fs_state, rel, flags,
			   &handle, &size);
	if (result != 0) {
		return result;
	}
	file->mount = &vfs_mount_state[index];
	file->handle = handle;
	file->size = size;
	file->offset = 0;
	file->flags = flags;
	file->in_use = 1;
	return 0;
}

/* Positioned provider read shared by vfs_read() and the loader adapter.
 * `file` is validated here; the adapter passes an explicit offset because
 * the module/ZEX loaders read fixed container fields. */
static int vfs_file_read_at(struct vfs_file *file, uint32_t offset,
			    void *buffer, uint16_t length)
{
	const struct zfs_ops *ops;

	if (file == (struct vfs_file *)0 || file->in_use == 0 ||
	    file->mount == (struct vfs_mount_runtime *)0) {
		return -ZOS_EINVAL;
	}
	if (buffer == (void *)0 && length != 0) {
		return -ZOS_EINVAL;
	}
	if (offset >= file->size) {
		return 0; /* clean EOF */
	}
	if ((uint32_t)length > (file->size - offset)) {
		length = (uint16_t)(file->size - offset);
	}
	if (length == 0) {
		return 0;
	}
	ops = file->mount->provider->ops;
	return ops->read(file->mount->fs_state, file->handle, offset, buffer,
			 length);
}

int vfs_read(struct vfs_file *file, void *buffer, uint16_t length)
{
	int result = vfs_file_read_at(
		file, (file != (struct vfs_file *)0) ? file->offset : 0u,
		buffer, length);

	if (result > 0) {
		file->offset += (uint32_t)result;
	}
	return result;
}

/* A mutation pointer a read-only provider leaves null is ZOS_EROFS.  The
 * position and the cached size advance only after the provider succeeds
 * (architecture section 32.28). */
int vfs_write(struct vfs_file *file, const void *buffer, uint16_t length)
{
	const struct zfs_ops *ops;
	uint32_t end;
	int result;

	if (file == (struct vfs_file *)0 || file->in_use == 0 ||
	    file->mount == (struct vfs_mount_runtime *)0) {
		return -ZOS_EINVAL;
	}
	if (buffer == (const void *)0 && length != 0) {
		return -ZOS_EINVAL;
	}
	ops = file->mount->provider->ops;
	if (ops->write == (void *)0) {
		return -ZOS_EROFS;
	}
	result = ops->write(file->mount->fs_state, file->handle, file->offset,
			    buffer, length);
	if (result != 0) {
		return result;
	}
	end = file->offset + (uint32_t)length;
	file->offset = end;
	if (end > file->size) {
		file->size = end;
	}
	return 0;
}

int vfs_seek(struct vfs_file *file, int32_t offset, uint8_t whence,
	     uint32_t *position)
{
	int32_t base;
	int32_t result;

	if (file == (struct vfs_file *)0 || file->in_use == 0 ||
	    file->mount == (struct vfs_mount_runtime *)0) {
		return -ZOS_EINVAL;
	}
	if (whence == ZFS_SEEK_SET) {
		base = 0;
	} else if (whence == ZFS_SEEK_CUR) {
		base = (int32_t)file->offset;
	} else if (whence == ZFS_SEEK_END) {
		base = (int32_t)file->size;
	} else {
		return -ZOS_EINVAL;
	}
	result = base + offset;
	if (result < 0) {
		return -ZOS_EINVAL;
	}
	file->offset = (uint32_t)result;
	if (position != (uint32_t *)0) {
		*position = file->offset;
	}
	return 0;
}

int vfs_sync(struct vfs_file *file)
{
	const struct zfs_ops *ops;

	if (file == (struct vfs_file *)0 || file->in_use == 0 ||
	    file->mount == (struct vfs_mount_runtime *)0) {
		return -ZOS_EINVAL;
	}
	ops = file->mount->provider->ops;
	if (ops->sync == (void *)0) {
		return -ZOS_EROFS;
	}
	return ops->sync(file->mount->fs_state, file->handle);
}

int vfs_close(struct vfs_file *file)
{
	const struct zfs_ops *ops;
	int result;

	if (file == (struct vfs_file *)0 || file->in_use == 0 ||
	    file->mount == (struct vfs_mount_runtime *)0) {
		return -ZOS_EINVAL;
	}
	ops = file->mount->provider->ops;
	/* Flush before releasing the handle (architecture section 32.27);
	 * a provider without sync (read-only) has nothing to flush. */
	if (ops->sync != (void *)0) {
		(void)ops->sync(file->mount->fs_state, file->handle);
	}
	result = ops->close(file->mount->fs_state, file->handle);
	file->mount = (struct vfs_mount_runtime *)0;
	file->handle = (void *)0;
	file->size = 0;
	file->offset = 0;
	file->flags = 0;
	file->in_use = 0;
	return result;
}

int vfs_stat(const char *path, struct zfs_stat *out)
{
	const struct zfs_ops *ops;
	const char *rel;
	uint16_t index;
	int result;

	if (out == (struct zfs_stat *)0) {
		return -ZOS_EINVAL;
	}
	result = vfs_resolve_path(path, &index, &rel);
	if (result == VFS_RESOLVED_ROOT) {
		vfs_fill_stat(out, ZFS_TYPE_DIR, 0); /* `/` is owned by VFS */
		return 0;
	}
	if (result == VFS_RESOLVED_MOUNTPOINT) {
		/* VFS owns mountpoint metadata: it is a directory. */
		vfs_fill_stat(out, ZFS_TYPE_DIR, 0);
		return 0;
	}
	if (result != VFS_RESOLVED_PROVIDER) {
		return result;
	}
	ops = vfs_mount_state[index].provider->ops;
	return ops->stat(vfs_mount_state[index].fs_state, rel, out);
}

/* --- loader source adapter -------------------------------------------- */

static int vfs_source_read(const void *ctx, uint16_t offset, uint8_t *dst,
			   uint16_t length)
{
	return vfs_file_read_at((struct vfs_file *)ctx, (uint32_t)offset, dst,
				length);
}

int vfs_file_source(struct vfs_file *file, struct zmod_source *source)
{
	if (file == (struct vfs_file *)0 || source == (struct zmod_source *)0 ||
	    file->in_use == 0) {
		return -ZOS_EINVAL;
	}
	if (file->size > 0xFFFFu) {
		return -ZOS_ENOSPC;
	}
	source->read = vfs_source_read;
	source->ctx = file;
	source->size = (uint16_t)file->size;
	return 0;
}
