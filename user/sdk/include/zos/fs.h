/* ZephyrOS SDK: native filesystem and exec wrappers.
 *
 * C-facing wrappers over the pinned `FS_*` syscall family (10h-1Fh,
 * architecture section 32.26).  Strings are NUL-terminated; a string
 * over 127 bytes (or an empty/null one) is rejected locally with
 * -ZOS_EINVAL before any syscall, because ZOS_PATH_MAX counts the
 * terminating NUL.  The kernel owns all window, descriptor and path
 * validation.
 *
 * Return convention matches zos/console.h: a non-negative result, or the
 * negated native error code.  The `struct zos_fs_call_req` /
 * zos_fs_call() pair exposes the single gate stub to low-level callers
 * (it is used by the filesystem acceptance program to build raw blocks).
 */

#ifndef ZOS_SDK_FS_H
#define ZOS_SDK_FS_H

#include "zos/types.h"
#include "zos/fs.h"

/* One request for the shared gate stub: the syscall number and the block
 * address (always the SDK's low-window scratch). */
struct zos_fs_call_req {
	unsigned char number;
	void         *block;
};

int zos_fs_call(struct zos_fs_call_req *req);

int zos_open(const char *path, unsigned char flags);
int zos_close(unsigned int fd);
int zos_read(unsigned int fd, void *buffer, unsigned int length);
int zos_write(unsigned int fd, const void *buffer, unsigned int length);
/* Returns the resulting 32-bit position (as a long) or -errno. */
long zos_seek(unsigned int fd, long offset, unsigned char whence);
int zos_stat(const char *path, struct zfs_stat *out);
int zos_opendir(const char *path);
int zos_readdir(unsigned int fd, struct zfs_dirent *out);
int zos_closedir(unsigned int fd);
int zos_mkdir(const char *path);
int zos_rmdir(const char *path);
int zos_unlink(const char *path);
int zos_rename(const char *oldpath, const char *newpath);
int zos_chdir(const char *path);
/* Returns the path length excluding the NUL, or -errno. */
int zos_getcwd(char *buffer, unsigned int length);
int zos_sync(unsigned int fd);

#endif /* ZOS_SDK_FS_H */
