/* ZephyrOS M9 native filesystem runtime: C wrappers (package 27).
 *
 * Owns the shared low-window argument block (`zos_fs_scratch`, defined in
 * zex_fs_runtime.asm) and builds every `FS_*` block before calling the
 * single gate stub.  All local validation is limited to the documented
 * path-length rule; window, descriptor and path semantics belong to the
 * kernel.
 */

#include "zephyros/fs.h"
#include "zephyros/syscall.h"

#include "zex_runtime.h"
#include "zex_fs_runtime.h"

#define ZOS_FS_OPEN     0x10u
#define ZOS_FS_CLOSE    0x11u
#define ZOS_FS_READ     0x12u
#define ZOS_FS_WRITE    0x13u
#define ZOS_FS_SEEK     0x14u
#define ZOS_FS_STAT     0x15u
#define ZOS_FS_OPENDIR  0x16u
#define ZOS_FS_READDIR  0x17u
#define ZOS_FS_CLOSEDIR 0x18u
#define ZOS_FS_MKDIR    0x19u
#define ZOS_FS_RMDIR    0x1Au
#define ZOS_FS_UNLINK   0x1Bu
#define ZOS_FS_RENAME   0x1Cu
#define ZOS_FS_CHDIR    0x1Du
#define ZOS_FS_GETCWD   0x1Eu
#define ZOS_FS_SYNC     0x1Fu

/* One overlay for every argument block; the largest is the rename block. */
union zos_fs_block {
	struct zfs_path_args    path;
	struct zfs_open_args    open;
	struct zfs_io_args      io;
	struct zfs_seek_args    seek;
	struct zfs_stat_args    stat;
	struct zfs_readdir_args readdir;
	struct zfs_getcwd_args  getcwd;
	struct zfs_rename_args  rename;
	struct zfs_fd_args      fd;
};

extern union zos_fs_block zos_fs_scratch;

/* NUL-terminated length in 1..127, or 0xFFFF for null/empty/over-long. */
static unsigned int zfs_len(const char *path)
{
	unsigned int n = 0;

	if (path == (const char *)0) {
		return 0xFFFFu;
	}
	while (path[n] != '\0') {
		if (n >= (unsigned int)ZEX_PATH_MAX) {
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
		return -ZOS_FS_EINVAL;
	}
	zos_fs_scratch.path.path = (unsigned int)path;
	zos_fs_scratch.path.length = n;
	return zfs_call(number, &zos_fs_scratch.path);
}

static int zfs_fd_call(unsigned char number, unsigned int fd)
{
	zos_fs_scratch.fd.fd = (unsigned char)fd;
	return zfs_call(number, &zos_fs_scratch.fd);
}

int zos_open(const char *path, unsigned char flags)
{
	unsigned int n = zfs_len(path);

	if (n == 0xFFFFu) {
		return -ZOS_FS_EINVAL;
	}
	zos_fs_scratch.open.path = (unsigned int)path;
	zos_fs_scratch.open.length = n;
	zos_fs_scratch.open.flags = flags;
	return zfs_call(ZOS_FS_OPEN, &zos_fs_scratch.open);
}

int zos_close(unsigned int fd)
{
	return zfs_fd_call(ZOS_FS_CLOSE, fd);
}

int zos_read(unsigned int fd, void *buffer, unsigned int length)
{
	zos_fs_scratch.io.fd = (unsigned char)fd;
	zos_fs_scratch.io.buffer = (unsigned int)buffer;
	zos_fs_scratch.io.length = length;
	return zfs_call(ZOS_FS_READ, &zos_fs_scratch.io);
}

int zos_write(unsigned int fd, const void *buffer, unsigned int length)
{
	zos_fs_scratch.io.fd = (unsigned char)fd;
	zos_fs_scratch.io.buffer = (unsigned int)buffer;
	zos_fs_scratch.io.length = length;
	return zfs_call(ZOS_FS_WRITE, &zos_fs_scratch.io);
}

long zos_seek(unsigned int fd, long offset, unsigned char whence)
{
	int result;

	zos_fs_scratch.seek.fd = (unsigned char)fd;
	zos_fs_scratch.seek.whence = whence;
	zos_fs_scratch.seek.offset = offset;
	result = zfs_call(ZOS_FS_SEEK, &zos_fs_scratch.seek);
	if (result < 0) {
		return (long)result;
	}
	return zos_fs_scratch.seek.offset;
}

int zos_stat(const char *path, struct zfs_stat *out)
{
	unsigned int n = zfs_len(path);

	if (n == 0xFFFFu) {
		return -ZOS_FS_EINVAL;
	}
	zos_fs_scratch.stat.path = (unsigned int)path;
	zos_fs_scratch.stat.length = n;
	zos_fs_scratch.stat.out = (unsigned int)out;
	return zfs_call(ZOS_FS_STAT, &zos_fs_scratch.stat);
}

int zos_opendir(const char *path)
{
	return zfs_path_call(ZOS_FS_OPENDIR, path);
}

int zos_readdir(unsigned int fd, struct zfs_dirent *out)
{
	zos_fs_scratch.readdir.fd = (unsigned char)fd;
	zos_fs_scratch.readdir.out = (unsigned int)out;
	return zfs_call(ZOS_FS_READDIR, &zos_fs_scratch.readdir);
}

int zos_closedir(unsigned int fd)
{
	return zfs_fd_call(ZOS_FS_CLOSEDIR, fd);
}

int zos_mkdir(const char *path)
{
	return zfs_path_call(ZOS_FS_MKDIR, path);
}

int zos_rmdir(const char *path)
{
	return zfs_path_call(ZOS_FS_RMDIR, path);
}

int zos_unlink(const char *path)
{
	return zfs_path_call(ZOS_FS_UNLINK, path);
}

int zos_rename(const char *oldpath, const char *newpath)
{
	unsigned int from_len = zfs_len(oldpath);
	unsigned int to_len = zfs_len(newpath);

	if (from_len == 0xFFFFu || to_len == 0xFFFFu) {
		return -ZOS_FS_EINVAL;
	}
	zos_fs_scratch.rename.from = (unsigned int)oldpath;
	zos_fs_scratch.rename.from_length = from_len;
	zos_fs_scratch.rename.to = (unsigned int)newpath;
	zos_fs_scratch.rename.to_length = to_len;
	return zfs_call(ZOS_FS_RENAME, &zos_fs_scratch.rename);
}

int zos_chdir(const char *path)
{
	return zfs_path_call(ZOS_FS_CHDIR, path);
}

int zos_getcwd(char *buffer, unsigned int length)
{
	zos_fs_scratch.getcwd.buffer = (unsigned int)buffer;
	zos_fs_scratch.getcwd.length = length;
	return zfs_call(ZOS_FS_GETCWD, &zos_fs_scratch.getcwd);
}

int zos_sync(unsigned int fd)
{
	return zfs_fd_call(ZOS_FS_SYNC, fd);
}
