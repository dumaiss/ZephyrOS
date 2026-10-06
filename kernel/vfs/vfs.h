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

#include "zos/types.h"
#include "zos/fs.h"
#include "zephyros/vfs.h"
#include "zephyros/module.h"
#include "zephyros/process.h"
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

/* Provider-registration transaction, used by the module loader around each
 * module init exactly like the device and IRQ checkpoints.  A checkpoint
 * records the registration serial; rollback removes exactly the providers
 * registered after it (wrap-safe) and returns how many were removed.
 * Built-in providers registered at bootstrap predate every checkpoint and
 * are never touched. */
typedef uint16_t vfs_provider_checkpoint_t;
vfs_provider_checkpoint_t vfs_provider_checkpoint(void);
int vfs_provider_rollback(vfs_provider_checkpoint_t checkpoint);
void vfs_provider_set_owner(const void *owner);

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

/* --- bank-7 VFS workspace (package 27, correction-01) -----------------
 *
 * One 512-byte static workspace, owned by kernel/vfs/ and kernel-private
 * (never visible to modules or applications).  It replaces the six
 * separate pathname buffers and the staging buffer package 27 first
 * carried; work/M9-REFERENCE.md allows one shared pathname scratch plus a
 * second only for rename.  It has two mutually exclusive views:
 *
 *   staging view   all 512 bytes; READ/WRITE chunk data and the
 *                  stat/readdir/getcwd result assembled for the gate.
 *   path view      four 128-byte slots, in this order:
 *                    slot 0  copy-in A     caller path (NUL-terminated)
 *                    slot 1  normalized A  canonical path (CWD-relative)
 *                    slot 2  copy-in B     rename target only
 *                    slot 3  normalized B  rename target only
 *
 * Which operation uses which view:
 *   OPEN OPENDIR STAT MKDIR RMDIR UNLINK CHDIR   path view (0 -> 1)
 *   RENAME                                       path view (0->1, 2->3)
 *   GETCWD                                       path view (slot 1), then
 *                                                the staging view
 *   READ WRITE READDIR                           staging view
 *   CLOSE CLOSEDIR SYNC SEEK                     neither
 * Boot-time kernel callers (the loader and exec, through the kernel file
 * API) use the path view only; no syscall is active during boot.
 *
 * Execution is single-tasking, the gate is not re-entrant and no ISR
 * touches VFS, so the two views are never live at once. */
#define VFS_WORKSPACE_SIZE 512u
#define VFS_PATH_SLOT_SIZE 128u
#define VFS_PATH_SLOTS     4u

extern uint8_t vfs_workspace[VFS_WORKSPACE_SIZE];

#define VFS_WS_STAGING ((uint8_t *)vfs_workspace)
#define VFS_WS_SLOT(n) ((char *)&vfs_workspace[(n) * VFS_PATH_SLOT_SIZE])
#define VFS_WS_IN_A    VFS_WS_SLOT(0u)
#define VFS_WS_NORM_A  VFS_WS_SLOT(1u)
#define VFS_WS_IN_B    VFS_WS_SLOT(2u)
#define VFS_WS_NORM_B  VFS_WS_SLOT(3u)

#if defined(__SDCC)
_Static_assert(VFS_PATH_SLOTS * VFS_PATH_SLOT_SIZE == VFS_WORKSPACE_SIZE,
	       "the VFS workspace views must exactly tile the buffer");
#endif

/* View guard: the debug/acceptance builds (ZOS_VFS_WS_GUARD=1) fault on a
 * path-view / staging-view overlap; production compiles it out.  The check
 * is inlined (no helper call), so the guard adds no stack frame to the
 * syscall path and the measured gate-stack depth is unaffected. */
#ifndef ZOS_VFS_WS_GUARD
#define ZOS_VFS_WS_GUARD 0
#endif

#define VFS_WS_VIEW_PATH 1u
#define VFS_WS_VIEW_DATA 2u

#if ZOS_VFS_WS_GUARD
extern uint8_t vfs_ws_view; /* 0 free, else the active view */
void vfs_ws_fault(void);    /* never returns */

#define vfs_ws_enter(view) \
	do { \
		if (vfs_ws_view != 0u && vfs_ws_view != (view)) { \
			vfs_ws_fault(); \
		} \
		vfs_ws_view = (view); \
	} while (0)
#define vfs_ws_leave(view) \
	do { \
		if (vfs_ws_view == (view)) { \
			vfs_ws_view = 0u; \
		} \
	} while (0)
#else
#define vfs_ws_enter(view) ((void)(view))
#define vfs_ws_leave(view) ((void)(view))
#endif

/* --- path helpers (used by VFS and the acceptance tests) -------------- */

/* Normalize `path` into an absolute canonical path in `out`, resolving a
 * relative `path` against the canonical `base` directory (which must
 * itself be an absolute canonical path): repeated and trailing slashes
 * collapsed, `.` removed, `..` applied with traversal above `/` clamped
 * to `/`.  Preserves case.  Package 27 passes the caller's CWD.  Returns
 * 0, or -ZOS_EINVAL for a null argument, an over-long path/component, or
 * a buffer that cannot hold the result.  `base` and `out` must not alias. */
int vfs_path_normalize_at(const char *base, const char *path, char *out,
			  uint16_t out_size);

/* Normalize `path` against the calling process' CWD (package 27; the
 * kernel launcher's CWD is `/`) and select the owning mount with the flat
 * first-component rule (architecture section 32.8): the first component is compared case-insensitively with the mounted
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

/* Select the owning mount for an already-normalized absolute path, writing
 * the discriminant to the caller's `result` local (VFS_RESOLVED_* or a
 * negative ZOS_E* value) and setting *index / *rel.  This is the inner half
 * of vfs_resolve_path; `stat` and `chdir` use it on a path they normalized
 * into the workspace themselves, so the shared normalized slot is never read
 * and written at once.
 *
 * A macro, not a function: it must add no stack frame, so the FS_* syscall
 * path depth is identical to the pre-correction code (the workspace
 * consolidation must not deepen the gate stack). */
#define VFS_RESOLVE_NORM(norm, index, rel, result) \
	do { \
		if ((norm)[1] == '\0') { \
			(result) = VFS_RESOLVED_ROOT; \
		} else if (((result) = vfs_mount_resolve((norm), (index), \
							 (rel))) == 0) { \
			if (vfs_is_mountpoint(norm)) { \
				*(rel) = "/"; \
				(result) = VFS_RESOLVED_MOUNTPOINT; \
			} else { \
				(result) = VFS_RESOLVED_PROVIDER; \
			} \
		} \
	} while (0)

/* --- kernel file API (path-based providers) --------------------------- */

/* Kernel-only open hint used by the executable loader.  It is not a native
 * application flag: a provider that distinguishes executable media (ROMFS)
 * refuses a non-executable entry opened with it, preserving the M6
 * ZEX_EXEC_ERR_TYPE rejection now that the provider reports only the file
 * size and lets VFS stat for the type. */
#define VFS_O_EXEC 0x8000u

/* Caller-provided open-file storage; never an automatic object on the gate
 * stack.  The process-visible descriptor pool is built on this file
 * storage by kernel/vfs/process.c (package 27). */
struct vfs_file {
	struct vfs_mount_runtime *mount;
	void *handle;
	uint32_t size;
	uint32_t offset; /* current byte position, advanced by read/write */
	uint16_t flags;
	uint8_t in_use;
};

/* Open `path` (absolute or relative to `/`).  `flags` is a ZFS_O_* set
 * optionally ORed with VFS_O_EXEC.  Unknown bits, TRUNC/CREATE/EXCL without
 * WRITE, and EXCL without CREATE are rejected with ZOS_EINVAL.  On success
 * fills `file` with the position at 0; on failure returns a negative ZOS_E*
 * value and leaves `file` unused. */
int vfs_open(const char *path, uint16_t flags, struct vfs_file *file);

/* Read `length` bytes at the file position into `buffer` and advance the
 * position by the number of bytes read.  Returns the number of bytes read (a
 * short read at EOF, 0 at/after EOF) or a negative error; EOF is a
 * successful zero-byte read.  `buffer` must be common/gate-resident as
 * required by the provider. */
int vfs_read(struct vfs_file *file, void *buffer, uint16_t length);

/* Write through a writable provider at the file position; ZOS_EROFS when the
 * provider leaves the operation null (every M9 package-25/26 provider is
 * read-only).  The position advances, and the cached size grows, only after
 * the provider reports success. */
int vfs_write(struct vfs_file *file, const void *buffer, uint16_t length);

/* Reposition the file.  `offset` is a signed 32-bit byte count and `whence`
 * is ZFS_SEEK_SET/CUR/END.  Stores the resulting 32-bit position in
 * *position when `position` is non-null.  A result below zero is rejected
 * with ZOS_EINVAL; seeking past the current end is allowed. */
int vfs_seek(struct vfs_file *file, int32_t offset, uint8_t whence,
	     uint32_t *position);

int vfs_sync(struct vfs_file *file);
/* Flushes through the provider's sync (when present) and then releases the
 * provider handle. */
int vfs_close(struct vfs_file *file);

/* Path-based stat.  `stat("/")` and `stat()` of a mounted mountpoint are
 * answered by VFS itself as directories. */
int vfs_stat(const char *path, struct zfs_stat *out);

/* Stat for an already normalized absolute path (no normalization, so it is
 * safe to call with a buffer the caller normalized into the workspace). */
int vfs_stat_norm(const char *norm, struct zfs_stat *out);

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
	struct vfs_mount_runtime *mount; /* provider dir; `/` for root merge */
	void *handle;
	uint16_t index;   /* VFS_DIR_KIND_ROOT mount-table cursor */
	uint8_t kind;
	uint8_t merge;    /* root cursor: 0 mountpoints, 1 provider entries */
	uint8_t in_use;
};

int vfs_opendir(const char *path, struct vfs_dir *dir);

/* Write the next entry and return 1, or return 0 at end of stream, or a
 * negative error.  For `/`, the merged stream first yields each mounted
 * root-level mountpoint, then the `/` provider's entries with shadowed
 * mountpoint names (case-insensitive) skipped; it still works when `/` has
 * no mounted filesystem. */
int vfs_readdir(struct vfs_dir *dir, struct zfs_dirent *out);

int vfs_closedir(struct vfs_dir *dir);

/* --- global descriptor pool and per-process FS state (package 27) -----
 *
 * One global `_DATA` pool of VFS_FD_MAX entries; a file and a directory
 * iterator share it (architecture section 32.22).  Each entry is owner
 * tagged: the fd a process names is the pool index, and every operation
 * verifies the owner.  A suspended parent keeps its descriptors while a
 * child runs (single-tasking), so the pool is never per-process.  A full
 * pool reports ZOS_EMFILE; a range/type/owner mismatch reports
 * ZOS_EINVAL.  Provider handles never leave the kernel. */
#define VFS_FD_MAX 8u
#define VFS_FD_NONE 0xFFu

#define VFS_DESC_FREE      0u
#define VFS_DESC_FILE      1u
#define VFS_DESC_DIRECTORY 2u

/* Up to ZOS_EXEC_MAX_DEPTH active process chain levels hold a canonical
 * 128-byte CWD each; a process inherits its parent's CWD and releases it
 * on exit.  The kernel launcher's CWD is the constant `/`.
 * (ZOS_EXEC_MAX_DEPTH is owned by include/zephyros/process.h.) */

struct vfs_descriptor {
	uint8_t owner;   /* pid 0-6, or VFS_FD_NONE when FREE */
	uint8_t type;    /* VFS_DESC_*                      */
	union {
		struct vfs_file file; /* FILE: mount/handle/size/offset/flags */
		struct vfs_dir dir;   /* DIRECTORY: mount/handle/cursor       */
	} u;
};

extern struct vfs_descriptor vfs_descriptors[VFS_FD_MAX];

#if defined(__SDCC)
_Static_assert(sizeof(struct vfs_descriptor) == 17,
	       "vfs_descriptor must be 17 packed bytes");
_Static_assert(__builtin_offsetof(struct vfs_descriptor, type) == 1,
	       "vfs_descriptor type offset");
_Static_assert(__builtin_offsetof(struct vfs_descriptor, u) == 2,
	       "vfs_descriptor union offset");
#endif

/* Discard every descriptor and CWD slot; called once at VFS bootstrap so
 * the pools never depend on the C runtime having zeroed `_DATA`. */
void vfs_fs_state_init(void);

/* Allocate the lowest free descriptor of `type` owned by `pid`, returning
 * its index (0..VFS_FD_MAX-1) or -ZOS_EMFILE.  `path`/`flags` are passed
 * to the kernel file/dir API; on failure nothing is allocated. */
int vfs_fd_open(uint8_t pid, uint8_t type, const char *path, uint16_t flags);
int vfs_fd_opendir(uint8_t pid, const char *path);

/* Return the descriptor `fd` only when it is allocated, has exactly `type`
 * and is owned by `pid`; otherwise null (ZOS_EINVAL at the syscall layer).
 * The caller must not retain the pointer across an exit. */
struct vfs_descriptor *vfs_fd_check(uint8_t fd, uint8_t type, uint8_t pid);

/* Close the provider handle/dir iterator and free the entry.  Idempotent
 * for an already-free index. */
int vfs_fd_close(uint8_t fd);
int vfs_fd_closedir(uint8_t fd);

/* Number of allocated descriptors (acceptance/leak queries). */
uint8_t vfs_fd_active_count(void);

/* CWD lifecycle.  vfs_process_start() gives the child the parent's CWD
 * (or the constant `/` when `parent_pid` is PROCESS_KERNEL_PARENT / an
 * unknown/missing parent) and fails with -ZOS_ENOSPC when no slot is
 * free.  vfs_process_exit() closes every descriptor owned by `pid` with
 * `close` semantics, frees its CWD, and is idempotent; it runs on every
 * exit path.  `vfs_cwd_for()` never returns null (`/` when unowned). */
int vfs_process_start(uint8_t pid, uint8_t parent_pid);
void vfs_process_exit(uint8_t pid);
const char *vfs_cwd_for(uint8_t pid);

/* Canonical `chdir` (path resolved against the caller's CWD, the target
 * must be an existing directory) and `getcwd` (writes the NUL-terminated
 * canonical CWD, returns its length in bytes excluding the NUL, or
 * -ZOS_EINVAL when the buffer is too small). */
int vfs_chdir(const char *path);
int vfs_getcwd(char *buffer, uint16_t length);

/* Loader adapters -------------------------------------------------- */

/* Bound `source` to the open `file` so module_load_source()/ZEX validation
 * read it through the kernel file API without knowing where it lives. */
int vfs_file_source(struct vfs_file *file, struct zmod_source *source);

#endif /* ZEPHYROS_KERNEL_VFS_H */
