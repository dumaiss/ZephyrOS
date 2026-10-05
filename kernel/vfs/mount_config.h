/* ZephyrOS M9 generated mount-configuration build contract (kernel-private).
 *
 * This header is the interface between tools/mountgen.py and kernel/vfs.
 * It is neither a native application ABI nor a module ABI: only the kernel
 * VFS (and the generator) may include it.  The generated table
 * (build/generated/vfs_mounts.c) includes this header and defines exactly
 * one `const struct vfs_mount_config vfs_mount_table[]` plus
 * `const uint8_t vfs_mount_count`.
 *
 * Mount policy lives only in the selected JSON manifest; VFS source never
 * contains paths, provider names or source strings.  The table is `const`
 * ROM data and VFS never writes it; mutable runtime state is kept in a
 * parallel array (kernel/vfs/vfs.h).
 */

#ifndef ZEPHYROS_KERNEL_VFS_MOUNT_CONFIG_H
#define ZEPHYROS_KERNEL_VFS_MOUNT_CONFIG_H

#include "zos/types.h"

/* Kernel mount-table capacity.  The generator rejects a manifest with more
 * entries; VFS re-checks that the system entry count is exactly one. */
#define VFS_MAX_MOUNTS 4u

/* Record flags.  `system` is stronger than `required`: the system entry is
 * instantiated during bootstrap and carries no separate required flag. */
#define VFS_MOUNT_SYSTEM   0x01u
#define VFS_MOUNT_REQUIRED 0x02u
#define VFS_MOUNT_OPTIONAL 0x00u

struct vfs_mount_config {
	const char *path;       /* canonical mount path, e.g. "/SYSTEM" */
	uint8_t path_len;       /* strlen(path), 1..ZOS_PATH_MAX-1     */
	const char *filesystem; /* provider registry name               */
	const char *source;     /* opaque, NUL-terminated provider source */
	uint8_t flags;          /* VFS_MOUNT_*                          */
};

/* Generated in build/generated/vfs_mounts.c and linked into the kernel. */
extern const struct vfs_mount_config vfs_mount_table[];
extern const uint8_t vfs_mount_count;

#endif /* ZEPHYROS_KERNEL_VFS_MOUNT_CONFIG_H */
