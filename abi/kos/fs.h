/* ZephyrOS native filesystem ABI (application-visible types).
 *
 * The metadata and limit vocabulary the FILE protocol and the
 * filesystem-namespace protocol share (architecture 36.13, 36.14): the
 * kernel router and providers, the filesystem driver, the SDK `fs.h`
 * wrappers and native programs.  `fs.h` (the SDK header of the same name)
 * adds the wrapper prototypes; `abi/zos/protocol.h` owns the operation
 * numbers and frame layouts.  The M9 syscall argument blocks are gone:
 * files are typed handles, and the `FS_*` family is retired (package 42b).
 *
 * A path or component over a limit is ZOS_EINVAL; see zos/errno.h.
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

/* open() flags: the bind flags `fs_open` passes to `bind` and the selected
 * filesystem provider interprets (architecture 36.7 "FILE uses it for
 * read/write/create/truncate style open flags"). */
#define ZFS_O_READ   0x01u
#define ZFS_O_WRITE  0x02u
#define ZFS_O_CREATE 0x04u
#define ZFS_O_EXCL   0x08u
#define ZFS_O_TRUNC  0x10u

/* seek() origins. */
#define ZFS_SEEK_SET 0u
#define ZFS_SEEK_CUR 1u
#define ZFS_SEEK_END 2u

#if defined(__SDCC)
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
