/* KonaOS native filesystem ABI (application-visible types).
 *
 * The metadata and limit vocabulary the FILE protocol and the
 * filesystem-namespace protocol share (architecture 36.13, 36.14): the
 * kernel router and providers, the filesystem driver, the SDK `fs.h`
 * wrappers and native programs.  `fs.h` (the SDK header of the same name)
 * adds the wrapper prototypes; `abi/kos/protocol.h` owns the operation
 * numbers and frame layouts.  The M9 syscall argument blocks are gone:
 * files are typed handles, and the `FS_*` family is retired (package 42b).
 *
 * A path or component over a limit is KOS_EINVAL; see kos/errno.h.
 */

#ifndef KONAOS_FS_H
#define KONAOS_FS_H

#include "kos/types.h"

/* Path and component limits.  Both include the terminating NUL, so the
 * longest usable path is 127 bytes and the longest component is 63. */
#define KOS_PATH_MAX 128u
#define KFS_NAME_MAX 64u

/* Object type (kfs_stat.type, kfs_dirent.type). */
#define KFS_TYPE_FILE 1u
#define KFS_TYPE_DIR  2u

/* Native attributes.  There is no archive attribute. */
#define KFS_ATTR_READONLY 0x01u
#define KFS_ATTR_HIDDEN   0x02u
#define KFS_ATTR_SYSTEM   0x04u

/* Zephyr-80 has no RTC; zero means unknown/unavailable.  The fields stay
 * 32-bit so a future time source does not change the layout.  M9 exposes
 * ctime and mtime, not atime. */
#define KFS_TIME_UNKNOWN 0u

/* stat() result: 16 bytes, packed on the Z80 (SDCC inserts no padding for
 * these fields).  No inode numbers, clusters, sectors or modes are exposed.
 * M9 has no statfs/df: architecture section 32.12 keeps filesystem-wide
 * accounting out of the native ABI until a consumer exists. */
struct kfs_stat {
	uint8_t  type;        /* KFS_TYPE_*                    */
	uint8_t  attributes;  /* KFS_ATTR_*                    */
	uint16_t reserved;    /* 0                             */
	uint32_t size;        /* bytes (0 for a directory)     */
	uint32_t ctime;       /* KFS_TIME_UNKNOWN              */
	uint32_t mtime;       /* KFS_TIME_UNKNOWN              */
};

/* readdir() result: 80 bytes.  `name` is NUL-terminated and `name_length`
 * counts the terminating NUL, so it is 1..KFS_NAME_MAX for a real entry. */
struct kfs_dirent {
	uint8_t  type;         /* KFS_TYPE_*                   */
	uint8_t  attributes;   /* KFS_ATTR_*                   */
	uint16_t name_length;  /* bytes in `name`, incl. NUL   */
	uint32_t size;
	uint32_t ctime;
	uint32_t mtime;
	char     name[KFS_NAME_MAX];
};

/* open() flags: the bind flags `fs_open` passes to `bind` and the selected
 * filesystem provider interprets (architecture 36.7 "FILE uses it for
 * read/write/create/truncate style open flags"). */
#define KFS_O_READ   0x01u
#define KFS_O_WRITE  0x02u
#define KFS_O_CREATE 0x04u
#define KFS_O_EXCL   0x08u
#define KFS_O_TRUNC  0x10u

/* seek() origins. */
#define KFS_SEEK_SET 0u
#define KFS_SEEK_CUR 1u
#define KFS_SEEK_END 2u

#if defined(__SDCC)
_Static_assert(sizeof(struct kfs_stat) == 16,
	       "kfs_stat must be 16 packed bytes");
_Static_assert(__builtin_offsetof(struct kfs_stat, type) == 0,
	       "kfs_stat type offset");
_Static_assert(__builtin_offsetof(struct kfs_stat, attributes) == 1,
	       "kfs_stat attributes offset");
_Static_assert(__builtin_offsetof(struct kfs_stat, reserved) == 2,
	       "kfs_stat reserved offset");
_Static_assert(__builtin_offsetof(struct kfs_stat, size) == 4,
	       "kfs_stat size offset");
_Static_assert(__builtin_offsetof(struct kfs_stat, ctime) == 8,
	       "kfs_stat ctime offset");
_Static_assert(__builtin_offsetof(struct kfs_stat, mtime) == 12,
	       "kfs_stat mtime offset");

_Static_assert(sizeof(struct kfs_dirent) == 80,
	       "kfs_dirent must be 80 packed bytes");
_Static_assert(__builtin_offsetof(struct kfs_dirent, type) == 0,
	       "kfs_dirent type offset");
_Static_assert(__builtin_offsetof(struct kfs_dirent, attributes) == 1,
	       "kfs_dirent attributes offset");
_Static_assert(__builtin_offsetof(struct kfs_dirent, name_length) == 2,
	       "kfs_dirent name_length offset");
_Static_assert(__builtin_offsetof(struct kfs_dirent, size) == 4,
	       "kfs_dirent size offset");
_Static_assert(__builtin_offsetof(struct kfs_dirent, ctime) == 8,
	       "kfs_dirent ctime offset");
_Static_assert(__builtin_offsetof(struct kfs_dirent, mtime) == 12,
	       "kfs_dirent mtime offset");
_Static_assert(__builtin_offsetof(struct kfs_dirent, name) == 16,
	       "kfs_dirent name offset");

#endif /* __SDCC */

#endif /* KONAOS_FS_H */
