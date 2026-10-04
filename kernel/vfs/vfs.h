/* ZephyrOS M9 kernel VFS internal interface (kernel-private).
 *
 * This header is never visible to modules or applications.  It declares the
 * kernel file/directory API, the mutation entry points, the provider
 * registry, the mount lifecycle and the bounded `zmod_source` adapter used
 * by the module and ZEX loaders.
 *
 * Providers are path-based (architecture section 32.12): VFS normalizes a
 * path, selects the owning mount by its first component (flat selection,
 * section 32.8) and hands the provider a provider-relative path.  VFS keeps
 * no node objects, no cache and no object identity.
 *
 * Gate-stack discipline (work/M9-REFERENCE.md): syscall and loader paths run
 * on the 256-byte common gate stack, so no function here declares an
 * automatic object larger than 32 bytes.  Pathname scratch space is static
 * bank-7 (`_DATA`) workspace owned by kernel/vfs.
 */

#ifndef ZEPHYROS_KERNEL_VFS_H
#define ZEPHYROS_KERNEL_VFS_H

#include "zephyros/types.h"
#include "zephyros/fs.h"
#include "zephyros/vfs.h"
#include "zephyros/module.h"
#include "kernel/vfs/mount_config.h"

/* --- lifecycle -------------------------------------------------------- */

#define VFS_PHASE_BOOTSTRAP 0u /* vfs_init() registering providers      */
#define VFS_PHASE_MODULES   1u /* /SYSTEM mounted; modules may load     */
#define VFS_PHASE_MOUNT     2u /* normal mount pass running             */
#define VFS_PHASE_READY     3u /* namespace fixed; services available   */

int vfs_init(void);
int vfs_mount_all(void);
uint8_t vfs_phase(void);

/* --- provider registry (capacity VFS_PROVIDER_MAX) ------------------- */

#define VFS_PROVIDER_MAX 4u

/* Registers a provider (built-ins at bootstrap, modules during the module
 * phase).  Rejects a null/malformed provider, a duplicate name (compared
 * case-insensitively) and a full registry with a negative ZOS_E* value. */
int vfs_register_filesystem(const struct zfs_provider *provider);

/* Case-insensitive registry lookup; null when absent. */
const struct zfs_provider *vfs_find_provider(const char *name);

/* --- runtime mount state (parallel to the const config table) --------- */

#define VFS_MOUNT_UNMOUNTED 0u
#define VFS_MOUNT_MOUNTED   1u
#define VFS_MOUNT_FAILED    2u

/* No generation counter and no cache key: VFS holds no object identity and
 * no file-data cache (architecture section 32.12).  `last_error` keeps the
 * failing code for the required-mount diagnostic. */
struct vfs_mount_runtime {
	uint8_t state;                        /* VFS_MOUNT_*                */
	uint8_t reserved;
	const struct zfs_provider *provider;
	void *fs_state;
	int last_error;
};

extern struct vfs_mount_runtime vfs_mount_state[VFS_MAX_MOUNTS];

#if defined(__SDCC)
/* The array is indexed in parallel with the generated table, so its entry
 * size is part of the kernel contract (the acceptance tests walk it). */
_Static_assert(sizeof(struct vfs_mount_runtime) == 8,
	       "vfs_mount_runtime must be 8 packed bytes");
_Static_assert(__builtin_offsetof(struct vfs_mount_runtime, last_error) == 6,
	       "vfs_mount_runtime last_error offset");
#endif

/* --- path helpers (used by VFS and the acceptance tests) -------------- */

/* Normalize `path` (absolute against the base `/`) into an absolute
 * canonical path in `out`: repeated and trailing slashes collapsed, `.`
 * removed, `..` applied with traversal above `/` clamped to `/`.
 * Preserves case.  Returns 0, or -ZOS_EINVAL for a null argument, an
 * over-long path/component, or a buffer that cannot hold the result. */
int vfs_path_normalize(const char *path, char *out, uint16_t out_size);

/* ASCII case-insensitive name comparison (A-Z == a-z, all else exact). */
uint8_t vfs_name_equal(const char *a, const char *b);

/* Normalize `path` against the base `/` (package 27 adds CWD) and select
 * the owning mount with the flat first-component rule (architecture section
 * 32.8): the first component is compared case-insensitively with the mounted
 * root-level mountpoints; otherwise the filesystem attached at `/` owns the
 * path.  Returns one of the VFS_RESOLVED_* codes with *index and *rel set
 * (*rel is the provider-relative path, always beginning with '/'), or a
 * negative ZOS_E* value (-ZOS_ENODEV when no mounted filesystem can own
 * the path). */
#define VFS_RESOLVED_PROVIDER   0 /* *rel is the provider path          */
#define VFS_RESOLVED_ROOT       1 /* the path is `/`                    */
#define VFS_RESOLVED_MOUNTPOINT 2 /* *rel is `/`: a mounted mountpoint  */
int vfs_resolve_path(const char *path, uint16_t *index, const char **rel);

/* Flat mount selection over an already-normalized absolute path.  This is
 * the inner half of vfs_resolve_path and is used by rename for its second
 * path. */
int vfs_mount_resolve(const char *norm, uint16_t *index, const char **rel);

/* 1 when `norm` names one of the mounted root-level mountpoints, or the
 * VFS root itself.  VFS answers stat() and open() for those paths. */
uint8_t vfs_is_mountpoint(const char *norm);

/* 1 when the configured path of mount `index` is `/` or one root component
 * (architecture section 32.6), i.e. it is visible in the root directory and
 * eligible for flat first-component selection.  `vfs_mount_is_root` is the
 * stricter test for the `/` mount itself. */
uint8_t vfs_mount_is_root_level(uint16_t index);
uint8_t vfs_mount_is_root(uint16_t index);

/* --- kernel file API (path-based providers) --------------------------- */

/* Kernel-only open hint used by the executable loader.  It is not a native
 * application flag: a provider that distinguishes executable media (ROMFS)
 * refuses a non-executable entry opened with it, preserving the M6
 * ZEX_EXEC_ERR_TYPE rejection now that the provider reports only the file
 * size and lets VFS stat for the type. */
#define VFS_O_EXEC 0x8000u

/* Caller-provided open-file storage; never an automatic object on the gate
 * stack.  VFS keeps no descriptor table in this package (package 27 adds
 * the process-visible pool). */
struct vfs_file {
	struct vfs_mount_runtime *mount;
	void *handle;
	uint32_t size;
	uint16_t flags;
	uint8_t in_use;
};

/* Open `path` (absolute or relative to `/`).  `flags` is a ZFS_O_* set
 * optionally ORed with VFS_O_EXEC.  On success fills `file`; on failure
 * returns a negative ZOS_E* value and leaves `file` unused. */
int vfs_open(const char *path, uint16_t flags, struct vfs_file *file);

/* Read `length` bytes at `offset` into `buffer`.  Returns the number of
 * bytes read (a short read at EOF, 0 at/after EOF) or a negative error.
 * `buffer` must be common/gate-resident as required by the provider. */
int vfs_read(struct vfs_file *file, uint32_t offset, void *buffer,
	     uint16_t length);

/* Write through a writable provider; ZOS_EROFS when the provider leaves the
 * operation null (every M9 package-25/26 provider is read-only). */
int vfs_write(struct vfs_file *file, uint32_t offset, const void *buffer,
	      uint16_t length);

int vfs_sync(struct vfs_file *file);
int vfs_close(struct vfs_file *file);

/* Path-based stat.  `stat("/")` and `stat()` of a mounted mountpoint are
 * answered by VFS itself as directories. */
int vfs_stat(const char *path, struct zfs_stat *out);

/* --- path-based mutation entry points (syscall hooks, package 27) ----- */

int vfs_mkdir(const char *path);
int vfs_rmdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rename(const char *oldpath, const char *newpath);

/* --- kernel directory API -------------------------------------------- */

#define VFS_DIR_KIND_NONE     0u
#define VFS_DIR_KIND_ROOT     1u /* synthetic VFS root enumeration       */
#define VFS_DIR_KIND_PROVIDER 2u /* provider directory                   */

struct vfs_dir {
	struct vfs_mount_runtime *mount;
	void *handle;
	uint16_t index;   /* VFS_DIR_KIND_ROOT mount-table cursor */
	uint8_t kind;
	uint8_t in_use;
};

int vfs_opendir(const char *path, struct vfs_dir *dir);

/* Write the next entry and return 1, or return 0 at end of stream, or a
 * negative error. */
int vfs_readdir(struct vfs_dir *dir, struct zfs_dirent *out);

int vfs_closedir(struct vfs_dir *dir);

/* --- loader adapters -------------------------------------------------- */

/* Bound `source` to the open `file` so module_load_source()/ZEX validation
 * read it through the kernel file API without knowing where it lives. */
int vfs_file_source(struct vfs_file *file, struct zmod_source *source);

#endif /* ZEPHYROS_KERNEL_VFS_H */
