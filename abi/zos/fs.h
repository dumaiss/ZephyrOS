/* ZephyrOS M9 native filesystem ABI (application-visible types).
 *
 * This header owns the native metadata and limit definitions shared by the
 * kernel VFS, filesystem providers, the native syscall family (package 27)
 * and native applications.  It deliberately contains no provider callbacks;
 * those live in include/zephyros/vfs.h.
 *
 * Values are pinned by work/M9-REFERENCE.md and architecture section 32.25.
 * A path or component over a limit is ZOS_EINVAL; see include/zephyros/errno.h.
 */

#ifndef ZEPHYROS_FS_H
#define ZEPHYROS_FS_H

#include "zos/types.h"

/* Path and component limits.  Both include the terminating NUL, so the
 * longest usable path is 127 bytes and the longest component is 63. */
#define ZOS_PATH_MAX 128u
#define ZFS_NAME_MAX 64u

/* Object type (zfs_stat.type, zfs_dirent.type). */
#define ZFS_TYPE_FILE 1u
#define ZFS_TYPE_DIR  2u

/* Native attributes.  There is no archive attribute. */
#define ZFS_ATTR_READONLY 0x01u
#define ZFS_ATTR_HIDDEN   0x02u
#define ZFS_ATTR_SYSTEM   0x04u

/* Zephyr-80 has no RTC; zero means unknown/unavailable.  The fields stay
 * 32-bit so a future time source does not change the layout.  M9 exposes
 * ctime and mtime, not atime. */
#define ZFS_TIME_UNKNOWN 0u

/* stat() result: 16 bytes, packed on the Z80 (SDCC inserts no padding for
 * these fields).  No inode numbers, clusters, sectors or modes are exposed.
 * M9 has no statfs/df: architecture section 32.12 keeps filesystem-wide
 * accounting out of the native ABI until a consumer exists. */
struct zfs_stat {
	uint8_t  type;        /* ZFS_TYPE_*                    */
	uint8_t  attributes;  /* ZFS_ATTR_*                    */
	uint16_t reserved;    /* 0                             */
	uint32_t size;        /* bytes (0 for a directory)     */
	uint32_t ctime;       /* ZFS_TIME_UNKNOWN              */
	uint32_t mtime;       /* ZFS_TIME_UNKNOWN              */
};

/* readdir() result: 80 bytes.  `name` is NUL-terminated and `name_length`
 * counts the terminating NUL, so it is 1..ZFS_NAME_MAX for a real entry. */
struct zfs_dirent {
	uint8_t  type;         /* ZFS_TYPE_*                   */
	uint8_t  attributes;   /* ZFS_ATTR_*                   */
	uint16_t name_length;  /* bytes in `name`, incl. NUL   */
	uint32_t size;
	uint32_t ctime;
	uint32_t mtime;
	char     name[ZFS_NAME_MAX];
};

/* open() flags (package 27 exposes them through SYS_FS_OPEN). */
#define ZFS_O_READ   0x01u
#define ZFS_O_WRITE  0x02u
#define ZFS_O_CREATE 0x04u
#define ZFS_O_EXCL   0x08u
#define ZFS_O_TRUNC  0x10u

/* seek() origins. */
#define ZFS_SEEK_SET 0u
#define ZFS_SEEK_CUR 1u
#define ZFS_SEEK_END 2u

/* Native filesystem syscall argument blocks (package 27, architecture
 * section 32.26).  Every block lives in the caller window
 * 0100h-1FFFh and is packed on the Z80: the numeric file descriptor is
 * the one-byte VFS descriptor-pool index, never a provider handle.
 *
 * Path fields are `{path:u16, length:u16}`: `path` addresses 1..127
 * non-NUL bytes of the caller's bank, read only through the caller
 * window.  `out`/`buffer` fields are caller-bank addresses in the
 * 0100h-DFFFh user-buffer window and move only through the checked
 * bank-copy gates. */
struct zfs_path_args {          /* 4 bytes: MKDIR/RMDIR/UNLINK/CHDIR */
	uint16_t path;
	uint16_t length;
};

struct zfs_open_args {          /* 5 bytes */
	uint16_t path;
	uint16_t length;
	uint8_t  flags;         /* ZFS_O_* */
};

struct zfs_io_args {            /* 5 bytes: READ/WRITE */
	uint8_t  fd;
	uint16_t buffer;
	uint16_t length;
};

struct zfs_seek_args {          /* 6 bytes; offset written back on return */
	uint8_t  fd;
	uint8_t  whence;        /* ZFS_SEEK_* */
	int32_t  offset;
};

struct zfs_stat_args {          /* 6 bytes */
	uint16_t path;
	uint16_t length;
	uint16_t out;           /* struct zfs_stat destination */
};

struct zfs_readdir_args {       /* 3 bytes */
	uint8_t  fd;
	uint16_t out;           /* struct zfs_dirent destination */
};

struct zfs_getcwd_args {        /* 4 bytes */
	uint16_t buffer;
	uint16_t length;        /* bytes available, including the NUL */
};

struct zfs_rename_args {        /* 8 bytes */
	uint16_t from;
	uint16_t from_length;
	uint16_t to;
	uint16_t to_length;
};

struct zfs_fd_args {            /* 1 byte: CLOSE/CLOSEDIR/SYNC */
	uint8_t  fd;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct zfs_path_args) == 4,
	       "zfs_path_args must be 4 packed bytes");
_Static_assert(__builtin_offsetof(struct zfs_path_args, path) == 0,
	       "zfs_path_args path offset");
_Static_assert(__builtin_offsetof(struct zfs_path_args, length) == 2,
	       "zfs_path_args length offset");
_Static_assert(sizeof(struct zfs_open_args) == 5,
	       "zfs_open_args must be 5 packed bytes");
_Static_assert(__builtin_offsetof(struct zfs_open_args, path) == 0,
	       "zfs_open_args path offset");
_Static_assert(__builtin_offsetof(struct zfs_open_args, length) == 2,
	       "zfs_open_args length offset");
_Static_assert(__builtin_offsetof(struct zfs_open_args, flags) == 4,
	       "zfs_open_args flags offset");
_Static_assert(sizeof(struct zfs_io_args) == 5,
	       "zfs_io_args must be 5 packed bytes");
_Static_assert(__builtin_offsetof(struct zfs_io_args, fd) == 0,
	       "zfs_io_args fd offset");
_Static_assert(__builtin_offsetof(struct zfs_io_args, buffer) == 1,
	       "zfs_io_args buffer offset");
_Static_assert(__builtin_offsetof(struct zfs_io_args, length) == 3,
	       "zfs_io_args length offset");
_Static_assert(sizeof(struct zfs_seek_args) == 6,
	       "zfs_seek_args must be 6 packed bytes");
_Static_assert(__builtin_offsetof(struct zfs_seek_args, fd) == 0,
	       "zfs_seek_args fd offset");
_Static_assert(__builtin_offsetof(struct zfs_seek_args, whence) == 1,
	       "zfs_seek_args whence offset");
_Static_assert(__builtin_offsetof(struct zfs_seek_args, offset) == 2,
	       "zfs_seek_args offset offset");
_Static_assert(sizeof(struct zfs_stat_args) == 6,
	       "zfs_stat_args must be 6 packed bytes");
_Static_assert(__builtin_offsetof(struct zfs_stat_args, path) == 0,
	       "zfs_stat_args path offset");
_Static_assert(__builtin_offsetof(struct zfs_stat_args, length) == 2,
	       "zfs_stat_args length offset");
_Static_assert(__builtin_offsetof(struct zfs_stat_args, out) == 4,
	       "zfs_stat_args out offset");
_Static_assert(sizeof(struct zfs_readdir_args) == 3,
	       "zfs_readdir_args must be 3 packed bytes");
_Static_assert(__builtin_offsetof(struct zfs_readdir_args, fd) == 0,
	       "zfs_readdir_args fd offset");
_Static_assert(__builtin_offsetof(struct zfs_readdir_args, out) == 1,
	       "zfs_readdir_args out offset");
_Static_assert(sizeof(struct zfs_getcwd_args) == 4,
	       "zfs_getcwd_args must be 4 packed bytes");
_Static_assert(__builtin_offsetof(struct zfs_getcwd_args, buffer) == 0,
	       "zfs_getcwd_args buffer offset");
_Static_assert(__builtin_offsetof(struct zfs_getcwd_args, length) == 2,
	       "zfs_getcwd_args length offset");
_Static_assert(sizeof(struct zfs_rename_args) == 8,
	       "zfs_rename_args must be 8 packed bytes");
_Static_assert(__builtin_offsetof(struct zfs_rename_args, from) == 0,
	       "zfs_rename_args from offset");
_Static_assert(__builtin_offsetof(struct zfs_rename_args, from_length) == 2,
	       "zfs_rename_args from_length offset");
_Static_assert(__builtin_offsetof(struct zfs_rename_args, to) == 4,
	       "zfs_rename_args to offset");
_Static_assert(__builtin_offsetof(struct zfs_rename_args, to_length) == 6,
	       "zfs_rename_args to_length offset");
_Static_assert(sizeof(struct zfs_fd_args) == 1,
	       "zfs_fd_args must be 1 packed byte");
_Static_assert(__builtin_offsetof(struct zfs_fd_args, fd) == 0,
	       "zfs_fd_args fd offset");
_Static_assert(sizeof(struct zfs_stat) == 16,
	       "zfs_stat must be 16 packed bytes");
_Static_assert(__builtin_offsetof(struct zfs_stat, type) == 0,
	       "zfs_stat type offset");
_Static_assert(__builtin_offsetof(struct zfs_stat, attributes) == 1,
	       "zfs_stat attributes offset");
_Static_assert(__builtin_offsetof(struct zfs_stat, reserved) == 2,
	       "zfs_stat reserved offset");
_Static_assert(__builtin_offsetof(struct zfs_stat, size) == 4,
	       "zfs_stat size offset");
_Static_assert(__builtin_offsetof(struct zfs_stat, ctime) == 8,
	       "zfs_stat ctime offset");
_Static_assert(__builtin_offsetof(struct zfs_stat, mtime) == 12,
	       "zfs_stat mtime offset");

_Static_assert(sizeof(struct zfs_dirent) == 80,
	       "zfs_dirent must be 80 packed bytes");
_Static_assert(__builtin_offsetof(struct zfs_dirent, type) == 0,
	       "zfs_dirent type offset");
_Static_assert(__builtin_offsetof(struct zfs_dirent, attributes) == 1,
	       "zfs_dirent attributes offset");
_Static_assert(__builtin_offsetof(struct zfs_dirent, name_length) == 2,
	       "zfs_dirent name_length offset");
_Static_assert(__builtin_offsetof(struct zfs_dirent, size) == 4,
	       "zfs_dirent size offset");
_Static_assert(__builtin_offsetof(struct zfs_dirent, ctime) == 8,
	       "zfs_dirent ctime offset");
_Static_assert(__builtin_offsetof(struct zfs_dirent, mtime) == 12,
	       "zfs_dirent mtime offset");
_Static_assert(__builtin_offsetof(struct zfs_dirent, name) == 16,
	       "zfs_dirent name offset");

#endif /* __SDCC */

#endif /* ZEPHYROS_FS_H */
