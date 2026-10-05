/* ZephyrOS M9 native filesystem runtime (user/runtime, package 27).
 *
 * C-facing wrappers over the pinned `FS_*` syscall family (10h-1Fh,
 * architecture section 32.26).  Programs call these instead of the raw
 * `RST 30h` ABI: the wrappers own the low-window argument block, convert a
 * carry-set error to the negated native errno, and never expose SDCC
 * calling conventions.
 *
 * Strings are NUL-terminated; a string over 127 bytes (or an empty/null
 * one) is rejected locally with -ZOS_FS_EINVAL before any syscall, because
 * `ZOS_PATH_MAX` counts the terminating NUL.  The kernel owns all window,
 * descriptor and path validation.
 *
 * Return convention matches zex_runtime.h: a non-negative result, or the
 * negated native error code.  The wrapper types mirror
 * include/zephyros/fs.h; the pair is pinned by host tests.
 */

#ifndef ZEPHYROS_USER_ZEX_FS_RUNTIME_H
#define ZEPHYROS_USER_ZEX_FS_RUNTIME_H

/* Mirrors include/zephyros/errno.h ZOS_EINVAL.  ZEX_PATH_MAX mirrors the
 * usable path length (ZOS_PATH_MAX - 1). */
#define ZOS_FS_EINVAL 0x02
#define ZEX_PATH_MAX  127

/* One request for the shared gate stub: the syscall number and the block
 * address (always this module's low-window scratch). */
struct zos_fs_call_req {
	unsigned char number;
	void         *block;
};

/* Argument blocks are complete elsewhere; the wrappers only need pointers
 * to the native metadata types (defined by include/zephyros/fs.h). */
struct zfs_stat;
struct zfs_dirent;

/* Reserve `number` bytes for the shared scratch block's type only in
 * include/zephyros/fs.h; this stub marshals through it. */
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

#endif /* ZEPHYROS_USER_ZEX_FS_RUNTIME_H */
