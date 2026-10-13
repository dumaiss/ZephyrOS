/* KonaOS kernel VFS internal interface (kernel-private).
 *
 * This header is never visible to drivers or applications.  It owns
 * (architecture 4.10):
 *
 *   - the global owner-tagged handle table: a bound object records its owner, its 8-bit protocol type,
 *     the endpoint that serves it, the provider-private object identity,
 *     the bind flags and the per-process handle number.  The two standard
 *     handles sit at the fixed numbers 1 and 2 of every process;
 *   - the per-process CWD pool: one canonical path per active process;
 *   - the bank-7 workspace the syscall bodies use for pathname scratch and
 *     result staging;
 *   - the one path normalizer.
 *
 * The root router (kernel/vfs/router.c) routes to three static
 * providers, and the providers answer with handles.
 *
 * Gate-stack discipline: syscall paths run on the shared common gate
 * stack, so no function here declares an automatic object larger than
 * 32 bytes.  Pathname scratch space is the static
 * fixed-core workspace owned by kernel/vfs.
 */

#ifndef KONAOS_KERNEL_VFS_H
#define KONAOS_KERNEL_VFS_H

#include "kos/types.h"
#include "kos/fs.h"
#include "kos/object.h"
#include "romfs.h"
#include "driver.h"
#include "kernel/ipc/ipc.h"

/* --- lifecycle -------------------------------------------------------- */

#define VFS_PHASE_BOOTSTRAP 0u /* vfs_init() still running             */
#define VFS_PHASE_READY     1u /* the providers are live               */

/* One-time nucleus setup: the ROMFS reader, the handle table, the CWD
 * pool, the provider tables and the in-process FILE endpoint.  There is
 * no mount phase (architecture 4.10).  Returns 0 or a negative KOS_E*
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

/* View guard: the debug/acceptance builds (KOS_VFS_WS_GUARD=1) fault on a
 * path-view / staging-view overlap; production compiles it out. */
#ifndef KOS_VFS_WS_GUARD
#define KOS_VFS_WS_GUARD 0
#endif

#define VFS_WS_VIEW_PATH 1u
#define VFS_WS_VIEW_DATA 2u

#if KOS_VFS_WS_GUARD
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
 * to `/`.  Preserves case.  Returns 0, or -KOS_EINVAL for a null
 * argument, an over-long path/component, or a buffer that cannot hold the
 * result.  `base` and `out` must not alias. */
int vfs_path_normalize_at(const char *base, const char *path, char *out,
			  uint16_t out_size);

/* --- the global handle table ------------------------------------------
 *
 * One global fixed-core pool of VFS_HANDLE_MAX entries; every entry is owner
 * tagged, and the handle a process names is the per-process number `bind`
 * handed out (1 and 2 are the standard console handles).  A suspended
 * parent keeps its handles while a child runs (single-tasking), so the
 * pool is never per-process.  A full pool reports KOS_EMFILE; a
 * stale/foreign/mistyped handle reports KOS_EINVAL.
 *
 * A bound FILE instance keeps its immutable metadata (the file size the
 * provider's bind reported, and - for the in-process ROMFS provider - the
 * source metadata and the read position) inline in the entry. */
#define VFS_HANDLE_MAX 12u
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
	uint8_t flags;      /* KFS_O_* subset of the bind flags       */
};

struct vfs_handle_entry {
	uint8_t owner;    /* pid, or VFS_FD_NONE when FREE       */
	uint8_t type;     /* KOS_TYPE_*, 0 when FREE             */
	uint8_t number;   /* per-process handle number           */
	uint8_t endpoint; /* serving endpoint (provider marker)  */
	uint8_t object;   /* provider-private instance identity  */
	uint8_t flags;    /* low byte of the bind flags          */
	union {
		struct vfs_ns_file ns;
	} u;
};

extern struct vfs_handle_entry vfs_handles[VFS_HANDLE_MAX];

#if defined(__SDCC)
_Static_assert(sizeof(struct vfs_ns_file) == 15,
	       "vfs_ns_file must be 15 packed bytes");
_Static_assert(sizeof(struct vfs_handle_entry) == 21,
	       "vfs_handle_entry must be 21 packed bytes");
_Static_assert(__builtin_offsetof(struct vfs_handle_entry, endpoint) == 3,
	       "vfs_handle_entry endpoint offset");
_Static_assert(__builtin_offsetof(struct vfs_handle_entry, u) == 6,
	       "vfs_handle_entry union offset");
#endif

/* Discard every handle and CWD slot; called once at VFS bootstrap so the
 * pools never depend on the C runtime having zeroed `_DATA`. */
void vfs_fs_state_init(void);

/* Allocate the lowest free entry for `pid`, give it the lowest free handle
 * number >= `first`, and return the entry (owner/number set) or null when
 * the pool is full.  Type, endpoint, object, flags and the union payload
 * are the caller's to fill. */
struct vfs_handle_entry *vfs_handle_alloc(uint8_t pid, uint8_t first);

/* The bound entry the process `pid` names with handle `number`, or null
 * for a free/stale/foreign handle. */
struct vfs_handle_entry *vfs_handle_find(uint8_t pid, uint8_t number);

/* The owner id a call site resolving "the current process's" instances
 * must use: the running process's pid, or VFS_OWNER_KERNEL while the
 * nucleus itself runs (no process is RUNNING). */
uint8_t vfs_owner_current(void);

/* Return a bound entry to the pool.  No provider call. */
void vfs_handle_release(struct vfs_handle_entry *entry);

/* Number of allocated handles (acceptance/leak queries). */
uint8_t vfs_handle_active_count(void);

/* CWD lifecycle.  vfs_process_start() gives the child the parent's CWD
 * (or the constant `/` when `parent_pid` is PROCESS_KERNEL_PARENT / an
 * unknown/missing parent) and fails with -KOS_ENOSPC when no slot is
 * free.  vfs_process_exit() releases every handle owned by `pid` (through
 * the owning provider) and its CWD slot, and is idempotent; it runs on
 * every exit path.  `vfs_cwd_for()` never returns null (`/` when
 * unowned). */
int vfs_process_start(uint8_t pid, uint8_t parent_pid);
void vfs_process_exit(uint8_t pid);
const char *vfs_cwd_for(uint8_t pid);

/* Store the canonical path `norm` as the running process' CWD (SYS_CHDIR,
 * after the owning provider accepted it).  Returns 0 or -KOS_EINVAL when
 * the caller owns no CWD slot. */
int vfs_cwd_store(const char *norm);

/* The VFS root router and the static namespace providers (kernel-private).
 *
 * Architecture 4.9 and 4.10: the VFS owns the global root
 * `/` and routes a normalized global path, ASCII case-insensitively, by
 * its first component to exactly three static providers:
 *
 *   /fs       -> the FS provider: nucleus routing code that forwards the
 *                remainder to the build's filesystem driver over IPC
 *                (the driver's endpoint comes from the host-built `/fs`
 *                publication; a build with no filesystem driver is
 *                ENODEV)
 *   /system   -> the ROMFS provider over kernel/romfs (available before
 *                any resident driver)
 *   /dev      -> the DEVICE provider over the host-built publication
 *                table the resident loader installs
 *
 * There is no mount table, no provider registration and no root merge; a
 * provider owns everything below its root.
 *
 * The handle side lives in kernel/vfs/handles.c;
 * this module resolves names to objects and fills or releases the pool
 * entries.  Two objects are nucleus-served: `vfs_ns_file_dispatch()` is
 * the in-process FILE endpoint the ROMFS provider binds (installed as a
 * nucleus endpoint, reached through the same handle-call path as a bank-6
 * endpoint), and the FS provider's own operations are C entry points the
 * SYS_CALL/SYS_UNBIND/SYS_NS_ENUM bodies call at the outermost call level
 * (`vfs_ns_fs_call`, `vfs_ns_release`, `vfs_ns_enum`), so a driver they
 * forward to runs on the driver stack.
 */

/* Publication table capacity: the production build declares five roles
 * (console, input/keyboard, video, serial/0, ioc), which expand to seven
 * rows with the grouping entries; an acceptance set adds its synthetic
 * names.  The loader refuses a set that overflows the table. */
#define VFS_NS_PUB_MAX 12u

/* Publication name field: "/input/keyboard" is the longest role above, so
 * the table stores the flat provider-relative names in 16 bytes. */
#define VFS_NS_NAME_MAX 16u

/* The in-process FILE endpoint's number: nucleus endpoints live above the
 * host-assigned driver numbers (1..N), which the packer bounds by the
 * nucleus table size.  Endpoint numbers stay nucleus-internal. */
#define VFS_NS_FILE_ENDPOINT ((uint8_t)(IPC_EP_NUCLEUS_BASE + 1u))

/* The FS provider's marker: a bound `/fs` object (the filesystem-namespace
 * handle and every `/fs` FILE instance) records it as its endpoint, and
 * the syscall bodies route such calls into this module instead of the IPC
 * endpoint table.  It is never an installed endpoint. */
#define VFS_NS_FS_ENDPOINT   ((uint8_t)(IPC_EP_NUCLEUS_BASE + 2u))

/* The `/fs` provider root and its length. */
#define VFS_NS_FS_ROOT "/fs"

struct vfs_ns_publication {
	char    name[VFS_NS_NAME_MAX]; /* provider-relative, e.g. "/console" */
	uint8_t type;                  /* KOS_TYPE_*, 0 on a container       */
	uint8_t number;                /* endpoint number, 0 on a container  */
	uint8_t object;                /* provider object identity           */
	uint8_t flags;                 /* KOS_NS_ENTRY_CONTAINER on a grouping row */
};

/* One-time nucleus setup (the in-process FILE endpoint) plus the empty
 * start for the provider tables; called from vfs_init().  Returns 0 or a
 * negative KOS_E* value. */
int vfs_ns_init(void);

/* Discard every host-built publication (vfs_init(), and the acceptance
 * replays that re-initialize the nucleus). */
void vfs_ns_reset(void);

/* Install one host-built publication (the resident loader, before any
 * driver code runs): a `/dev` object into the DEVICE table, or the `/fs`
 * root, which records the filesystem driver's endpoint.  The loader
 * already checked the record against the container; the checks here
 * protect the nucleus table: capacity, a bounded printable name beginning
 * with '/', a legal protocol type and a nonzero endpoint.  Returns 0 or
 * -KOS_EINVAL/-KOS_ENOSPC. */
int vfs_ns_install_publication(const char *name, uint8_t type, uint8_t number,
			       uint8_t object);

/* The endpoint one installed `/dev` publication names (`rel` is the
 * provider-relative name, e.g. "/console"), or 0 when no such object was
 * published.  The nucleus's own boot console output uses it (there is no
 * runtime class lookup). */
uint8_t vfs_ns_publication_endpoint(const char *rel);

/* Resolve the normalized absolute path `norm` and bind an instance for
 * `pid`, storing the 8-bit protocol type in *type and returning the handle
 * number (>= 1) or a negative KOS_E* value.  `flags` is the caller's
 * 16-bit bind flags, interpreted by the selected provider. */
int vfs_ns_bind(uint8_t pid, const char *norm, uint16_t flags, uint8_t *type);

/* Release the instance the handle names for `pid`.  Returns 0 or
 * -KOS_EINVAL.  The standard handles (1, 2) are refused. */
int vfs_ns_unbind(uint8_t pid, uint8_t handle);

/* Release one bound object: the provider's release, then the entry.
 * Process teardown uses this for every descriptor the pid owns, the
 * standard handles included (vfs_process_exit); a live caller goes
 * through vfs_ns_unbind above. */
void vfs_ns_release_entry(struct vfs_handle_entry *entry);

/* The handle's endpoint and object identity for SYS_CALL, or a negative
 * KOS_E* value when the handle is not a bound object. */
int vfs_ns_resolve(uint8_t pid, uint8_t handle, uint8_t *endpoint,
		   uint8_t *object);

/* Bind the process `pid`'s two standard handles to `/dev/console` (absent
 * when the build publishes none).  Called once at process start. */
void vfs_ns_standard_handles(uint8_t pid);

/* Namespace enumeration for SYS_NS_ENUM: write as many `struct
 * kos_ns_entry` records as fit `out_size` bytes to the caller buffer
 * (bank `bank`, address `out`) starting at `*cookie`, returning the count
 * written and updating `*cookie`, or a negative KOS_E* value.  Below
 * `/fs` the provider forwards to the filesystem driver, whose entries
 * land in the caller's buffer through the descriptor copy gate. */
int vfs_ns_enum(uint8_t bank, const char *norm, uint16_t *cookie,
		uint16_t out, uint16_t out_size, uint16_t *count);

/* --- the FS provider's dispatch (kernel/vfs/fs.c) --------------------- */

/* One SYS_CALL on a bound `/fs` object, run at the outermost call level by
 * the syscall body: FILE operations go to the filesystem driver with the
 * provider instance stamped into the frame, filesystem-namespace
 * operations carry the path (already copied and normalized by this
 * module) to the driver.  `object` is the instance the handle resolved
 * to. */
int16_t vfs_ns_fs_call(uint8_t handle, uint8_t object,
		       struct kos_ipc_call_args *args);

/* One SYS_UNBIND of a bound `/fs` object: release the driver's instance
 * (a filesystem-namespace handle has none). */
void vfs_ns_fs_release(uint8_t type, uint8_t object);

/* The provider the normalized path `norm` belongs to is asked whether it
 * names a traversable directory (SYS_CHDIR): 0 with *ok set, or a
 * negative KOS_E* value. */
int vfs_ns_dir_check(const char *norm, uint8_t *ok);

/* One nucleus-side FILE call on a bound FILE handle (the cold loader):
 * `frame` carries the operation, `object` the instance the handle
 * resolved to.  A nonzero `length` moves `length` bytes through the common
 * staging block in direction `dir` (FILE data); length 0 is the
 * descriptor-less form (metadata, seek, sync).  Returns the call status. */
int16_t vfs_ns_file_call(uint8_t handle, uint8_t object,
			 struct kos_ipc_frame *frame, uint16_t length,
			 uint8_t dir);

/* --- the ROMFS FILE provider (kernel/vfs/system.c) ------------------- */

/* Bind the ROMFS entry `name` (provider-relative, e.g. "/MONITOR.EXE") as
 * a FILE instance for `pid`, or the `/system` root (`name` empty) as the
 * filesystem-namespace handle.  Returns the handle or a negative KOS_E*
 * value; `flags` carries the KFS_O_* open flags. */
int vfs_ns_file_bind(uint8_t pid, const char *name, uint16_t flags,
		     uint8_t *type);

/* `KFS_OP_STAT` for one `/system` path: `rel` is the provider-relative
 * remainder ("" for the ROMFS root) and the reply payload receives the
 * packed `struct kfs_stat`.  Returns 0 or a negative KOS_E* value. */
int vfs_ns_file_meta(const char *rel, struct kos_ipc_frame *frame);

/* The in-process FILE endpoint's dispatch (installed by vfs_ns_init());
 * the nucleus calls it through the ordinary handle-call path.  `handle`
 * is the caller's handle number, `object` the publication identity (unused
 * by ROMFS: the instance is the handle entry). */
int16_t vfs_ns_file_dispatch(uint8_t handle, uint8_t object,
			     struct kos_ipc_frame *frame);

/* --- cross-file helpers --------------------------------
 *
 * Statics another part now calls: each definition is extern in the part
 * that owns it, every caller sees the declaration here.  Names keep the
 * VFS spelling the split gives them (`ns_*` helpers became `vfs_*`). */

/* path.c */
uint8_t vfs_fold(uint8_t c);
uint8_t vfs_component_eq(const char *p, uint16_t length, const char *word);
uint16_t vfs_component_length(const char *p);
uint8_t vfs_path_under(const char *norm, const char *root);

/* dev.c */
const struct vfs_ns_publication *vfs_pub_find(const char *rel);
int vfs_device_bind(uint8_t pid, const char *rel, uint16_t flags,
		    uint8_t *type);
int vfs_enum_device(uint8_t bank, const char *rel, uint16_t *cookie,
		    uint16_t out, uint16_t out_size, uint16_t *count);
extern uint8_t vfs_ns_pub_count;

/* fs.c */
extern uint8_t vfs_ns_fs_endpoint;
int vfs_fs_alloc(uint8_t pid, uint8_t type, uint8_t object, uint16_t flags);
int vfs_fs_bind_file(uint8_t pid, const char *rel, uint16_t flags,
		     uint8_t *type);
void vfs_fs_put_path(uint8_t *payload, const char *path);
const char *vfs_fs_rel(const char *norm);
int vfs_fs_read_path(const struct kos_ipc_call_args *args);
int vfs_enum_fs(const char *rel, uint16_t *cookie, uint16_t out,
		uint16_t out_size, uint16_t *count);

/* system.c */
int vfs_enum_romfs(uint8_t bank, uint16_t *cookie, uint16_t out,
		   uint16_t out_size, uint16_t *count);

/* router.c */
int vfs_fs_namespace_op(uint8_t op, const char *norm, const char *norm_b,
			struct kos_ipc_frame *frame);
uint8_t vfs_room(uint16_t slot, uint16_t out_size);
int vfs_emit(uint8_t bank, uint16_t out, uint16_t slot, const char *name,
	     uint16_t name_length, uint8_t type, uint8_t flags);

#endif /* KONAOS_KERNEL_VFS_H */
