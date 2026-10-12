/* ZephyrOS VFS part: the root router. */
/* ZephyrOS M10 VFS root router and static namespace providers (package 42a).
 *
 * Architecture 36.13: the VFS owns `/` and routes a normalized global
 * path, ASCII case-insensitively, by its first component to the three
 * static providers - `/fs`, `/system` and `/dev`.  The selected provider
 * owns everything below its root; this module never interprets a
 * provider's internal hierarchy beyond selecting the provider.
 *
 *   FS provider      nucleus routing: it forwards the remainder below
 *                    `/fs` to the filesystem driver over IPC, whose
 *                    endpoint comes from the host-built `/fs` publication
 *                    (a build with no filesystem driver is ZOS_ENODEV).
 *                    It holds no FAT/FS2 semantics: paths, name
 *                    operations and enumeration are the driver's.
 *   ROMFS provider   kernel/vfs/system.c over the one ROMFS reader: binds
 *                    a FILE instance served in-process, so `/system` works
 *                    before any resident driver exists.
 *   DEVICE provider  the host-built publication table below.  The packer
 *                    emits one bindable record per `/dev` object from the
 *                    selected drivers' metadata and the resident loader
 *                    installs them; installing a record also adds the
 *                    grouping rows of its parent paths (`/dev/input`,
 *                    `/dev/serial`), so the table is exactly the flat
 *                    publication set plus its containers - no driver
 *                    registers a string at run time.
 *
 * Path normalization and the caller CWD stay in kernel/vfs/path.c and
 * kernel/vfs/handles.c; the syscall bodies hand this module a canonical
 * absolute path.  Candidate objects come from the ROMFS directory
 * (`romfs_entry_at`) and the publication table, both small and static.
 */

#include "kernel/vfs/vfs.h"
#include "kernel/lib/name.h"
#include "kernel/ipc/ipc.h"
#include "kernel/syscall/diag.h"

#include "kernel.h"
#include "zos/errno.h"
#include "process.h"
#include "romfs.h"

#include "zos/protocol.h"

/* Route one normalized absolute path and bind it. */
int vfs_ns_bind(uint8_t pid, const char *norm, uint16_t flags, uint8_t *type)
{
	uint16_t length;
	const char *rest;
	int result;

	if (norm == (const char *)0 || norm[0] != '/' ||
	    type == (uint8_t *)0) {
		return -ZOS_EINVAL;
	}
	length = vfs_component_length(norm + 1);
	rest = &norm[1u + length];
	if (vfs_component_eq(norm + 1, length, "fs")) {
		if (vfs_ns_fs_endpoint == 0u) {
			return -ZOS_ENODEV; /* no filesystem driver in this build */
		}
		if (rest[0] == '\0') {
			/* The filesystem-namespace handle: `fs.h` binds the
			 * root and sends its name operations with the path
			 * remainder. */
			result = vfs_fs_alloc(pid, ZOS_TYPE_FS, 0u, 0u);
			if (result >= 0) {
				*type = ZOS_TYPE_FS;
			}
			return result;
		}
		return vfs_fs_bind_file(pid, rest, flags, type);
	}
	if (vfs_component_eq(norm + 1, length, "system")) {
		return vfs_ns_file_bind(pid, rest, flags, type);
	}
	if (vfs_component_eq(norm + 1, length, "dev")) {
		return vfs_device_bind(pid, rest, flags, type);
	}
	return -ZOS_ENOENT;
}

int vfs_ns_unbind(uint8_t pid, uint8_t handle)
{
	struct vfs_handle_entry *entry;

	/* The standard handles are not a caller's to release: `bind` never
	 * returns their numbers and a program that dropped its console
	 * could not report why (architecture 36.13).  Process teardown is
	 * the one caller that releases them (vfs_process_exit). */
	if (handle < ZOS_HANDLE_FIRST_BOUND) {
		return -ZOS_EINVAL;
	}
	entry = vfs_handle_find(pid, handle);
	if (entry == (struct vfs_handle_entry *)0) {
		return -ZOS_EINVAL;
	}
	vfs_ns_release_entry(entry);
	return 0;
}

int vfs_ns_resolve(uint8_t pid, uint8_t handle, uint8_t *endpoint,
		   uint8_t *object)
{
	struct vfs_handle_entry *entry = vfs_handle_find(pid, handle);

	if (entry == (struct vfs_handle_entry *)0) {
		return -ZOS_EINVAL;
	}
	*endpoint = entry->endpoint;
	*object = entry->object;
	return 0;
}

/* --- enumeration --------------------------------------------------------- */

/* One entry, assembled on the gate stack and written to the caller's bank
 * through the checked copy gate (the common staging buffer is the gate's
 * single staging block; single-tasking makes it unambiguously ours). */
int vfs_emit(uint8_t bank, uint16_t out, uint16_t slot,
		   const char *name, uint16_t name_length, uint8_t type,
		   uint8_t flags)
{
	struct zos_bank_copy_req req;
	struct zos_ns_entry entry;
	uint8_t i;

	if (name_length == 0u || name_length + 1u > ZOS_NS_NAME_MAX) {
		return -ZOS_EINVAL;
	}
	entry.type = type;
	entry.flags = flags;
	entry.name_length = (uint8_t)(name_length + 1u);
	entry.reserved = 0u;
	for (i = 0; i < ZOS_NS_NAME_MAX; ++i) {
		entry.name[i] = (i < name_length) ? name[i] : '\0';
	}
	for (i = 0; i < ZOS_NS_ENTRY_SIZE; ++i) {
		zos_user_copy_staging[i] = ((const uint8_t *)&entry)[i];
	}
	req.bank = bank;
	req.op = ZOS_BANK_OP_WRITE;
	req.addr = (uint16_t)(out + slot * ZOS_NS_ENTRY_SIZE);
	req.buffer = zos_user_copy_staging;
	req.length = ZOS_NS_ENTRY_SIZE;
	if (zos_bank_copy(&req) != 0u) {
		return -ZOS_EFAULT;
	}
	return 0;
}

/* Room for one more entry in this batch?  The kernel links no arithmetic
 * helpers, so the comparison walks the entry size instead of multiplying:
 * a page holds at most ZOS_NS_BATCH_MAX entries. */
uint8_t vfs_room(uint16_t slot, uint16_t out_size)
{
	uint16_t needed = 0u;
	uint16_t index;

	for (index = 0u; index <= slot; ++index) {
		if (needed > (uint16_t)(out_size - ZOS_NS_ENTRY_SIZE)) {
			return 0u;
		}
		needed = (uint16_t)(needed + ZOS_NS_ENTRY_SIZE);
	}
	return 1u;
}

static int ns_enum_root(uint8_t bank, uint16_t *cookie, uint16_t out,
			uint16_t out_size, uint16_t *count)
{
	static const char names[3][7] = { "fs", "system", "dev" };
	uint16_t slot = 0;

	while (*cookie < 3u && vfs_room(slot, out_size) != 0u) {
		const char *name = names[*cookie];
		uint16_t length = vfs_component_length(name);

		/* All three root entries are containers; their provider
		 * protocols are not bindable objects in M10. */
		if (vfs_emit(bank, out, slot, name, length, 0u,
			    ZOS_NS_ENTRY_CONTAINER) != 0) {
			return -ZOS_EFAULT;
		}
		++*cookie;
		++slot;
		++*count;
	}
	return 0;
}

int vfs_ns_enum(uint8_t bank, const char *norm, uint16_t *cookie,
		uint16_t out, uint16_t out_size, uint16_t *count)
{
	uint16_t length;
	const char *rest;

	if (norm == (const char *)0 || norm[0] != '/' ||
	    cookie == (uint16_t *)0 || count == (uint16_t *)0 ||
	    out_size < ZOS_NS_ENTRY_SIZE) {
		return -ZOS_EINVAL;
	}
	*count = 0;
	if (norm[1] == '\0') {
		return ns_enum_root(bank, cookie, out, out_size, count);
	}
	length = vfs_component_length(norm + 1);
	rest = &norm[1u + length];
	if (vfs_component_eq(norm + 1, length, "fs")) {
		if (vfs_ns_fs_endpoint == 0u) {
			return -ZOS_ENODEV;
		}
		return vfs_enum_fs(rest, cookie, out, out_size, count);
	}
	if (vfs_component_eq(norm + 1, length, "system")) {
		if (rest[0] == '\0') {
			return vfs_enum_romfs(bank, cookie, out, out_size,
					     count);
		}
		return -ZOS_ENOTDIR;
	}
	if (vfs_component_eq(norm + 1, length, "dev")) {
		const struct vfs_ns_publication *pub;

		if (rest[0] == '\0') {
			return vfs_enum_device(bank, "", cookie, out, out_size,
					      count);
		}
		pub = vfs_pub_find(rest);
		if (pub == (const struct vfs_ns_publication *)0) {
			return -ZOS_ENOENT;
		}
		if (pub->flags == 0u) {
			return -ZOS_ENOTDIR; /* a bindable object */
		}
		return vfs_enum_device(bank, rest, cookie, out, out_size,
				      count);
	}
	return -ZOS_ENOENT;
}

/* One filesystem-namespace operation (`STAT`, `MKDIR`, `RMDIR`, `UNLINK`,
 * `RENAME`) on `norm` (RENAME adds `norm_b`): route it to the owning
 * provider and write the reply into the caller's frame. */
int vfs_fs_namespace_op(uint8_t op, const char *norm,
			      const char *norm_b, struct zos_ipc_frame *frame)
{
	int result;

	frame->op = op;
	frame->flags = 0u;
	frame->desc = ZOS_IPC_DESC_NONE;
	frame->arg0 = 0u;
	frame->arg1 = 0u;
	if (vfs_path_under(norm, "/system")) {
		/* ROMFS is read-only: only metadata has an answer. */
		if (op == ZFS_OP_STAT) {
			frame->payload[0] = norm[7];
			return vfs_ns_file_meta(norm + 7, frame);
		}
		return -ZOS_EROFS;
	}
	if (!vfs_path_under(norm, VFS_NS_FS_ROOT)) {
		return -ZOS_ENOENT;
	}
	if (vfs_ns_fs_endpoint == 0u) {
		return -ZOS_ENODEV;
	}
	vfs_fs_put_path(frame->payload, vfs_fs_rel(norm));
	if (op == ZFS_OP_RENAME) {
		if (norm_b == (const char *)0 ||
		    !vfs_path_under(norm_b, VFS_NS_FS_ROOT)) {
			return -ZOS_EXDEV;
		}
		vfs_fs_put_path(&frame->payload[2], vfs_fs_rel(norm_b));
	}
	result = ipc_nucleus_call(vfs_ns_fs_endpoint, frame);
	return result;
}

/* One nucleus-side FILE call on a bound handle (the cold loader). */
int16_t vfs_ns_file_call(uint8_t handle, uint8_t object,
			 struct zos_ipc_frame *frame, uint16_t length,
			 uint8_t dir)
{
	struct vfs_handle_entry *entry;

	entry = vfs_handle_find(vfs_owner_current(), handle);

	if (entry == (struct vfs_handle_entry *)0) {
		return -ZOS_EINVAL;
	}
	if (entry->endpoint == VFS_NS_FS_ENDPOINT) {
		if (vfs_ns_fs_endpoint == 0u) {
			return -ZOS_ENODEV;
		}
		if (length == 0u) {
			frame->payload[0] = object;
			return ipc_nucleus_call(vfs_ns_fs_endpoint, frame);
		}
		return ipc_nucleus_call_common(vfs_ns_fs_endpoint, frame,
					       length, dir);
	}
	if (length == 0u) {
		return ipc_nucleus_call_object(entry->endpoint, object, handle,
					       frame);
	}
	return ipc_nucleus_call_object_common(entry->endpoint, object, handle,
					      frame, length, dir);
}

/* `chdir` asks the owning provider whether `norm` is traversable. */
int vfs_ns_dir_check(const char *norm, uint8_t *ok)
{
	struct zos_ipc_frame frame;
	uint16_t length;

	*ok = 0u;
	if (norm == (const char *)0 || norm[0] != '/') {
		return -ZOS_EINVAL;
	}
	if (norm[1] == '\0') {
		*ok = 1u; /* the VFS owns `/`; it is traversable */
		return 0;
	}
	length = vfs_component_length(norm + 1);
	if (vfs_component_eq(norm + 1, length, "fs")) {
		int result;

		if (vfs_ns_fs_endpoint == 0u) {
			return -ZOS_ENODEV;
		}
		frame.op = ZFS_OP_STAT;
		frame.flags = 0u;
		frame.desc = ZOS_IPC_DESC_NONE;
		frame.arg0 = 0u;
		frame.arg1 = 0u;
		vfs_fs_put_path(frame.payload, vfs_fs_rel(norm));
		result = ipc_nucleus_call(vfs_ns_fs_endpoint, &frame);
		if (result < 0) {
			return result;
		}
		*ok = (uint8_t)((frame.payload[0] == ZFS_TYPE_DIR) ? 1u : 0u);
		return 0;
	}
	if (vfs_component_eq(norm + 1, length, "system")) {
		if (norm[1u + length] == '\0') {
			*ok = 1u;
			return 0;
		}
		return -ZOS_ENOTDIR; /* ROMFS is flat */
	}
	if (vfs_component_eq(norm + 1, length, "dev")) {
		const char *rel = &norm[1u + length];

		if (rel[0] == '\0') {
			*ok = 1u;
			return 0;
		}
		{
			const struct vfs_ns_publication *pub = vfs_pub_find(rel);

			if (pub == (const struct vfs_ns_publication *)0) {
				return -ZOS_ENOENT;
			}
			if (pub->flags == 0u) {
				return -ZOS_ENOTDIR; /* a bindable object */
			}
			*ok = 1u;
		}
		return 0;
	}
	return -ZOS_ENOENT;
}

/* --- nucleus setup -------------------------------------------------------- */

/* The VFS bootstrap (kernel/main.c): the ROMFS reader first, so `/system`
 * is usable before any resident driver exists, then the handle/CWD state,
 * the empty provider tables and the in-process FILE endpoint.  There is no
 * mount phase: the three providers are static (architecture 36.13). */
static uint8_t vfs_ready;

int vfs_init(void)
{
	int result = romfs_init();

	if (result != 0) {
		return result;
	}
	vfs_fs_state_init();
	vfs_ns_reset();
	result = vfs_ns_init();
	if (result != 0) {
		return result;
	}
	vfs_ready = 1u;
	return 0;
}

uint8_t vfs_phase(void)
{
	return (vfs_ready != 0u) ? VFS_PHASE_READY : VFS_PHASE_BOOTSTRAP;
}

void vfs_ns_reset(void)
{
	vfs_ns_pub_count = 0;
	vfs_ns_fs_endpoint = 0;
}

int vfs_ns_init(void)
{
	if (ipc_install_nucleus_endpoint(VFS_NS_FILE_ENDPOINT, 0u,
					 vfs_ns_file_dispatch) < 0) {
		return -ZOS_ENOSPC;
	}
	return 0;
}
