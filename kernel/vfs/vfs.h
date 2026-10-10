/* ZephyrOS M10 kernel VFS internal interface (kernel-private).
 *
 * This header is never visible to modules or applications.  It owns the
 * pieces of the M9 VFS the namespace model keeps (architecture 36.13,
 * package 42b):
 *
 *   - the global owner-tagged handle table (the extended M9 descriptor
 *     pool): a bound object records its owner, its 8-bit protocol type,
 *     the endpoint that serves it, the provider-private object identity,
 *     the bind flags and the per-process handle number.  The two standard
 *     handles sit at the fixed numbers 1 and 2 of every process;
 *   - the per-process CWD pool: one canonical path per active process;
 *   - the bank-7 workspace the syscall bodies use for pathname scratch and
 *     result staging;
 *   - the one path normalizer.
 *
 * The mount table, the provider registry, the path/mount resolver and the
 * M9 file/directory API are gone: the root router (kernel/vfs/ns.c) routes
 * to three static providers and the providers answer with handles.
 *
 * Gate-stack discipline (work/M10-REFERENCE.md): syscall paths run on the
 * 256-byte common gate stack, so no function here declares an automatic
 * object larger than 32 bytes.  Pathname scratch space is the static
 * fixed-core workspace owned by kernel/vfs.
 */

#ifndef ZEPHYROS_KERNEL_VFS_H
#define ZEPHYROS_KERNEL_VFS_H

#include "zos/types.h"
#include "zos/fs.h"
#include "zos/object.h"
#include "zephyros/romfs.h"

/* --- lifecycle -------------------------------------------------------- */

#define VFS_PHASE_BOOTSTRAP 0u /* vfs_init() still running             */
#define VFS_PHASE_READY     1u /* the providers are live               */

/* One-time nucleus setup: the ROMFS reader, the handle table, the CWD
 * pool, the provider tables and the in-process FILE endpoint.  There is
 * no mount phase (architecture 36.13).  Returns 0 or a negative ZOS_E*
 * value. */
int vfs_init(void);
uint8_t vfs_phase(void);

/* --- bank-7 VFS workspace ---------------------------------------------
 *
 * One 512-byte static workspace, owned by kernel/vfs/ and kernel-private
 * (never visible to modules or applications).  It has two mutually
 * exclusive views:
 *
 *   path view      four 128-byte slots, in this order:
 *                    slot 0  copy-in A     caller path (NUL-terminated)
 *                    slot 1  normalized A  canonical path
 *                    slot 2  copy-in B     rename target only
 *                    slot 3  normalized B  rename target only
 *   staging view   all 512 bytes; result assembly for the gate.
 *
 * Which operation uses which view:
 *   BIND NS_ENUM CHDIR CALL(ns op)   path view (0 -> 1, 2 -> 3)
 *   GETCWD                           path view (slot 1), then staging
 *   a CALL carrying file data        the descriptor copies (no view)
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
 * path-view / staging-view overlap; production compiles it out. */
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

/* --- path normalizer --------------------------------------------------- */

/* Normalize `path` into an absolute canonical path in `out`, resolving a
 * relative `path` against the canonical `base` directory (which must
 * itself be an absolute canonical path): repeated and trailing slashes
 * collapsed, `.` removed, `..` applied with traversal above `/` clamped
 * to `/`.  Preserves case.  Returns 0, or -ZOS_EINVAL for a null
 * argument, an over-long path/component, or a buffer that cannot hold the
 * result.  `base` and `out` must not alias. */
int vfs_path_normalize_at(const char *base, const char *path, char *out,
			  uint16_t out_size);

/* --- the global handle table ------------------------------------------
 *
 * One global fixed-core pool of VFS_FD_MAX entries; every entry is owner
 * tagged, and the handle a process names is the per-process number `bind`
 * handed out (1 and 2 are the standard console handles).  A suspended
 * parent keeps its handles while a child runs (single-tasking), so the
 * pool is never per-process.  A full pool reports ZOS_EMFILE; a
 * stale/foreign/mistyped handle reports ZOS_EINVAL.
 *
 * A bound FILE instance keeps its immutable metadata (the file size the
 * provider's bind reported, and - for the in-process ROMFS provider - the
 * source metadata and the read position) inline in the entry. */
#define VFS_FD_MAX 12u
#define VFS_FD_NONE 0xFFu

/* The owner id nucleus-internal instances belong to (the boot launcher's
 * executable handle, and anything else the kernel binds on a process's
 * behalf before one runs).  It is distinct from VFS_FD_NONE, which marks
 * a free slot, and from every real pid. */
#define VFS_OWNER_KERNEL 0xFEu

#define VFS_DESC_FREE 0u

struct vfs_ns_file {
	romfs_file_t romfs; /* the in-process ROMFS provider's source */
	uint32_t offset;    /* its read position                      */
	uint8_t flags;      /* ZFS_O_* subset of the bind flags       */
};

struct vfs_descriptor {
	uint8_t owner;    /* pid, or VFS_FD_NONE when FREE       */
	uint8_t type;     /* ZOS_TYPE_*, 0 when FREE             */
	uint8_t number;   /* per-process handle number           */
	uint8_t endpoint; /* serving endpoint (provider marker)  */
	uint8_t object;   /* provider-private instance identity  */
	uint8_t flags;    /* low byte of the bind flags          */
	union {
		struct vfs_ns_file ns;
	} u;
};

extern struct vfs_descriptor vfs_descriptors[VFS_FD_MAX];

#if defined(__SDCC)
_Static_assert(sizeof(struct vfs_ns_file) == 15,
	       "vfs_ns_file must be 15 packed bytes");
_Static_assert(sizeof(struct vfs_descriptor) == 21,
	       "vfs_descriptor must be 21 packed bytes");
_Static_assert(__builtin_offsetof(struct vfs_descriptor, endpoint) == 3,
	       "vfs_descriptor endpoint offset");
_Static_assert(__builtin_offsetof(struct vfs_descriptor, u) == 6,
	       "vfs_descriptor union offset");
#endif

/* Discard every handle and CWD slot; called once at VFS bootstrap so the
 * pools never depend on the C runtime having zeroed `_DATA`. */
void vfs_fs_state_init(void);

/* Allocate the lowest free entry for `pid`, give it the lowest free handle
 * number >= `first`, and return the entry (owner/number set) or null when
 * the pool is full.  Type, endpoint, object, flags and the union payload
 * are the caller's to fill. */
struct vfs_descriptor *vfs_handle_alloc(uint8_t pid, uint8_t first);

/* The bound entry the process `pid` names with handle `number`, or null
 * for a free/stale/foreign handle. */
struct vfs_descriptor *vfs_handle_find(uint8_t pid, uint8_t number);

/* The owner id a call site resolving "the current process's" instances
 * must use: the running process's pid, or VFS_OWNER_KERNEL while the
 * nucleus itself runs (no process is RUNNING). */
uint8_t vfs_owner_current(void);

/* Return a bound entry to the pool.  No provider call. */
void vfs_handle_release(struct vfs_descriptor *desc);

/* Number of allocated handles (acceptance/leak queries). */
uint8_t vfs_fd_active_count(void);

/* CWD lifecycle.  vfs_process_start() gives the child the parent's CWD
 * (or the constant `/` when `parent_pid` is PROCESS_KERNEL_PARENT / an
 * unknown/missing parent) and fails with -ZOS_ENOSPC when no slot is
 * free.  vfs_process_exit() releases every handle owned by `pid` (through
 * the owning provider) and its CWD slot, and is idempotent; it runs on
 * every exit path.  `vfs_cwd_for()` never returns null (`/` when
 * unowned). */
int vfs_process_start(uint8_t pid, uint8_t parent_pid);
void vfs_process_exit(uint8_t pid);
const char *vfs_cwd_for(uint8_t pid);

/* Store the canonical path `norm` as the running process' CWD (SYS_CHDIR,
 * after the owning provider accepted it).  Returns 0 or -ZOS_EINVAL when
 * the caller owns no CWD slot. */
int vfs_cwd_store(const char *norm);

#endif /* ZEPHYROS_KERNEL_VFS_H */
