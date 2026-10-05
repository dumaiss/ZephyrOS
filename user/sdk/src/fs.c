/* ZephyrOS libzos: native filesystem wrappers.
 *
 * Owns the shared low-window argument block and builds every FS_* block
 * before calling the single gate stub (fsgate.asm).  Local validation is
 * limited to the documented path-length rule; window, descriptor and path
 * semantics belong to the kernel.
 */

#include "zos.h"
#include "zos_internal.h"

/* NUL-terminated length in 1..(ZOS_PATH_MAX-1), or 0xFFFF for
 * null/empty/over-long. */
static unsigned int zfs_len(const char *path)
{
	unsigned int n = 0;

	if (path == (const char *)0) {
		return 0xFFFFu;
	}
	while (path[n] != '\0') {
		if (n >= (unsigned int)(ZOS_PATH_MAX - 1u)) {
			return 0xFFFFu;
		}
		++n;
	}
	if (n == 0u) {
		return 0xFFFFu;
	}
	return n;
}

static int zfs_call(unsigned char number, void *block)
{
	struct zos_fs_call_req req;

	req.number = number;
	req.block = block;
	return zos_fs_call(&req);
}

static int zfs_path_call(unsigned char number, const char *path)
{
	unsigned int n = zfs_len(path);

	if (n == 0xFFFFu) {
		return -ZOS_EINVAL;
	}
	zos_scratch.path.path = (unsigned int)path;
	zos_scratch.path.length = n;
	return zfs_call(number, &zos_scratch.path);
}

static int zfs_fd_call(unsigned char number, unsigned int fd)
{
	zos_scratch.fd.fd = (unsigned char)fd;
	return zfs_call(number, &zos_scratch.fd);
}

int zos_open(const char *path, unsigned char flags)
{
	unsigned int n = zfs_len(path);

	if (n == 0xFFFFu) {
		return -ZOS_EINVAL;
	}
	zos_scratch.open.path = (unsigned int)path;
	zos_scratch.open.length = n;
	zos_scratch.open.flags = flags;
	return zfs_call(ZOS_SYSCALL_FS_OPEN, &zos_scratch.open);
}

int zos_close(unsigned int fd)
{
	return zfs_fd_call(ZOS_SYSCALL_FS_CLOSE, fd);
}

int zos_read(unsigned int fd, void *buffer, unsigned int length)
{
	zos_scratch.io.fd = (unsigned char)fd;
	zos_scratch.io.buffer = (unsigned int)buffer;
	zos_scratch.io.length = length;
	return zfs_call(ZOS_SYSCALL_FS_READ, &zos_scratch.io);
}

int zos_write(unsigned int fd, const void *buffer, unsigned int length)
{
	zos_scratch.io.fd = (unsigned char)fd;
	zos_scratch.io.buffer = (unsigned int)buffer;
	zos_scratch.io.length = length;
	return zfs_call(ZOS_SYSCALL_FS_WRITE, &zos_scratch.io);
}

long zos_seek(unsigned int fd, long offset, unsigned char whence)
{
	int result;

	zos_scratch.seek.fd = (unsigned char)fd;
	zos_scratch.seek.whence = whence;
	zos_scratch.seek.offset = offset;
	result = zfs_call(ZOS_SYSCALL_FS_SEEK, &zos_scratch.seek);
	if (result < 0) {
		return (long)result;
	}
	return zos_scratch.seek.offset;
}

int zos_stat(const char *path, struct zfs_stat *out)
{
	unsigned int n = zfs_len(path);

	if (n == 0xFFFFu) {
		return -ZOS_EINVAL;
	}
	zos_scratch.stat.path = (unsigned int)path;
	zos_scratch.stat.length = n;
	zos_scratch.stat.out = (unsigned int)out;
	return zfs_call(ZOS_SYSCALL_FS_STAT, &zos_scratch.stat);
}

int zos_opendir(const char *path)
{
	return zfs_path_call(ZOS_SYSCALL_FS_OPENDIR, path);
}

int zos_readdir(unsigned int fd, struct zfs_dirent *out)
{
	zos_scratch.readdir.fd = (unsigned char)fd;
	zos_scratch.readdir.out = (unsigned int)out;
	return zfs_call(ZOS_SYSCALL_FS_READDIR, &zos_scratch.readdir);
}

int zos_closedir(unsigned int fd)
{
	return zfs_fd_call(ZOS_SYSCALL_FS_CLOSEDIR, fd);
}

int zos_mkdir(const char *path)
{
	return zfs_path_call(ZOS_SYSCALL_FS_MKDIR, path);
}

int zos_rmdir(const char *path)
{
	return zfs_path_call(ZOS_SYSCALL_FS_RMDIR, path);
}

int zos_unlink(const char *path)
{
	return zfs_path_call(ZOS_SYSCALL_FS_UNLINK, path);
}

int zos_rename(const char *oldpath, const char *newpath)
{
	unsigned int from_len = zfs_len(oldpath);
	unsigned int to_len = zfs_len(newpath);

	if (from_len == 0xFFFFu || to_len == 0xFFFFu) {
		return -ZOS_EINVAL;
	}
	zos_scratch.rename.from = (unsigned int)oldpath;
	zos_scratch.rename.from_length = from_len;
	zos_scratch.rename.to = (unsigned int)newpath;
	zos_scratch.rename.to_length = to_len;
	return zfs_call(ZOS_SYSCALL_FS_RENAME, &zos_scratch.rename);
}

int zos_chdir(const char *path)
{
	return zfs_path_call(ZOS_SYSCALL_FS_CHDIR, path);
}

int zos_getcwd(char *buffer, unsigned int length)
{
	zos_scratch.getcwd.buffer = (unsigned int)buffer;
	zos_scratch.getcwd.length = length;
	return zfs_call(ZOS_SYSCALL_FS_GETCWD, &zos_scratch.getcwd);
}

int zos_sync(unsigned int fd)
{
	return zfs_fd_call(ZOS_SYSCALL_FS_SYNC, fd);
}
