/* ZephyrOS M10 VFS root router and static namespace providers (package 42a).
 *
 * Architecture 36.13: the VFS owns `/` and routes a normalized global
 * path, ASCII case-insensitively, by its first component to the three
 * static providers - `/FS`, `/SYSTEM` and `/dev`.  The selected provider
 * owns everything below its root; this module never interprets a
 * provider's internal hierarchy beyond selecting the provider.
 *
 *   FS provider      nucleus routing: it forwards the remainder below
 *                    `/FS` to the filesystem driver over IPC, whose
 *                    endpoint comes from the host-built `/FS` publication
 *                    (a build with no filesystem driver is ZOS_ENODEV).
 *                    It holds no FAT/FS2 semantics: paths, name
 *                    operations and enumeration are the driver's.
 *   ROMFS provider   kernel/vfs/ns_file.c over the one ROMFS reader: binds
 *                    a FILE instance served in-process, so `/SYSTEM` works
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
 * kernel/vfs/process.c; the syscall bodies hand this module a canonical
 * absolute path.  Candidate objects come from the ROMFS directory
 * (`romfs_entry_at`) and the publication table, both small and static.
 */

#include "kernel/vfs/vfs.h"
#include "kernel/vfs/ns.h"
#include "kernel/lib/name.h"
#include "kernel/ipc/ipc.h"
#include "kernel/syscall/diag.h"

#include "zephyros/kernel.h"
#include "zephyros/errno.h"
#include "zephyros/process.h"
#include "zephyros/romfs.h"

#include "zos/protocol.h"

static int ns_fs_alloc(uint8_t pid, uint8_t type, uint8_t object,
		       uint16_t flags);
static int ns_fs_bind_file(uint8_t pid, const char *rel, uint16_t flags,
			   uint8_t *type);
static int ns_enum_fs(const char *rel, uint16_t *cookie, uint16_t out,
		      uint16_t out_size, uint16_t *count);

static struct vfs_ns_publication
	vfs_ns_publications[VFS_NS_PUB_MAX];
static uint8_t vfs_ns_pub_count;

/* The filesystem driver's endpoint, from the host-built `/FS` publication
 * (0 when the build selected no filesystem driver). */
static uint8_t vfs_ns_fs_endpoint;

/* --- ASCII helpers ------------------------------------------------------ */

static uint8_t ns_fold(uint8_t c)
{
	return (uint8_t)((c >= 'a' && c <= 'z') ? (c - 32u) : c);
}

/* Case-insensitive equality of a `length`-byte span against the
 * NUL-terminated `word`, which must be exactly `length` bytes long. */
static uint8_t ns_component_eq(const char *p, uint16_t length,
			       const char *word)
{
	uint16_t i;

	for (i = 0; i < length; ++i) {
		if (word[i] == '\0' ||
		    ns_fold((uint8_t)p[i]) != ns_fold((uint8_t)word[i])) {
			return 0u;
		}
	}
	return (uint8_t)(word[length] == '\0');
}

/* Length of the first path component of a normalized path fragment that
 * does not begin with '/'. */
static uint16_t ns_component_length(const char *p)
{
	uint16_t length = 0;

	while (p[length] != '\0' && p[length] != '/') {
		++length;
	}
	return length;
}

/* --- publication table -------------------------------------------------- */

static struct vfs_ns_publication *ns_pub_new(void)
{
	if (vfs_ns_pub_count >= VFS_NS_PUB_MAX) {
		return (struct vfs_ns_publication *)0;
	}
	return &vfs_ns_publications[vfs_ns_pub_count++];
}

static const struct vfs_ns_publication *ns_pub_find(const char *rel)
{
	uint8_t i;

	for (i = 0; i < vfs_ns_pub_count; ++i) {
		if (zos_ascii_equal(vfs_ns_publications[i].name, rel, 0u,
				    1u)) {
			return &vfs_ns_publications[i];
		}
	}
	return (const struct vfs_ns_publication *)0;
}

static const struct vfs_ns_publication *ns_pub_find(const char *rel);

uint8_t vfs_ns_publication_endpoint(const char *rel)
{
	const struct vfs_ns_publication *pub = ns_pub_find(rel);

	return (pub != (const struct vfs_ns_publication *)0 &&
		pub->flags == 0u)
	       ? pub->number : 0u;
}

/* Is `name` exactly one component below the container `rel` (`""` for the
 * root)?  Then the component starts at `name + strlen(rel) + 1`. */
static const char *ns_pub_child(const char *name, const char *rel)
{
	uint16_t i = 0;

	if (rel[0] != '\0') {
		while (rel[i] != '\0') {
			if (name[i] == '\0' ||
			    ns_fold((uint8_t)name[i]) !=
			    ns_fold((uint8_t)rel[i])) {
				return (const char *)0;
			}
			++i;
		}
	}
	/* Behind `rel` (or the root's leading '/') comes exactly one
	 * component: non-empty and with no further separator. */
	if (name[i] != '/' || name[i + 1u] == '\0') {
		return (const char *)0;
	}
	++i;
	{
		uint16_t k = i;

		while (name[k] != '\0') {
			if (name[k] == '/') {
				return (const char *)0;
			}
			++k;
		}
	}
	return &name[i];
}

/* Add one row (bindable or container) with an already validated name. */
static int ns_pub_add(const char *name, uint8_t length, uint8_t type,
		      uint8_t number, uint8_t object, uint8_t flags)
{
	struct vfs_ns_publication *pub = ns_pub_new();
	uint8_t i;

	if (pub == (struct vfs_ns_publication *)0) {
		return -ZOS_ENOSPC;
	}
	for (i = 0; i <= length; ++i) {
		pub->name[i] = name[i];
	}
	pub->type = type;
	pub->number = number;
	pub->object = object;
	pub->flags = flags;
	return 0;
}

int vfs_ns_install_publication(const char *name, uint8_t type, uint8_t number,
			       uint8_t object)
{
	char parent[VFS_NS_NAME_MAX];
	uint8_t length = 0;
	uint8_t i;

	if (name == (const char *)0 || name[0] != '/') {
		return -ZOS_EINVAL;
	}
	while (name[length] != '\0') {
		uint8_t c = (uint8_t)name[length];

		if ((uint16_t)length + 1u >= VFS_NS_NAME_MAX ||
		    c < 0x21u || c > 0x7Eu) {
			return -ZOS_EINVAL;
		}
		++length;
	}
	if (length == 1u) { /* "/" alone is the provider root, not an object */
		return -ZOS_EINVAL;
	}
	if (type < ZOS_TYPE_SERIAL || type > ZOS_TYPE_TIMER || number == 0u) {
		return -ZOS_EINVAL;
	}
	/* The `/FS` root is the filesystem-namespace publication: it names
	 * the build's filesystem driver and stays out of the `/dev` table
	 * (the FS provider routes to it). */
	if (length == 3u && type == ZOS_TYPE_FS &&
	    ns_component_eq(name + 1, 2u, "FS")) {
		if (object != 0u) {
			return -ZOS_EINVAL;
		}
		if (vfs_ns_fs_endpoint != 0u) {
			return -ZOS_EEXIST;
		}
		vfs_ns_fs_endpoint = number;
		return 0;
	}
	if (ns_pub_find(name) != (const struct vfs_ns_publication *)0) {
		return -ZOS_EEXIST;
	}
	/* Synthesize the grouping rows of every parent path ("/input" for
	 * "/input/keyboard"), never duplicating one already present. */
	for (i = 1u; i < length; ++i) {
		int result;

		if (name[i] != '/') {
			continue;
		}
		parent[0] = '/';
		{
			uint8_t k;

			for (k = 1u; k < i; ++k) {
				parent[k] = name[k];
			}
			parent[i] = '\0';
		}
		if (ns_pub_find(parent) !=
		    (const struct vfs_ns_publication *)0) {
			continue;
		}
		result = ns_pub_add(parent, i, 0u, 0u, 0u,
				    ZOS_NS_ENTRY_CONTAINER);
		if (result != 0) {
			return result;
		}
	}
	return ns_pub_add(name, length, type, number, object, 0u);
}

/* --- bind and release ---------------------------------------------------- */

/* Bind one DEVICE publication.  `rel` is provider-relative ("/console"). */
static int ns_device_bind(uint8_t pid, const char *rel, uint16_t flags,
			  uint8_t *type)
{
	const struct vfs_ns_publication *pub = ns_pub_find(rel);
	struct vfs_descriptor *desc;

	if ((flags & (uint16_t)~0x00FFu) != 0u) {
		return -ZOS_EINVAL; /* the entry records one flag byte */
	}
	if (pub == (const struct vfs_ns_publication *)0) {
		if (rel[0] == '\0') {
			return -ZOS_EISDIR; /* `/dev` itself */
		}
		return -ZOS_ENOENT;
	}
	if (pub->flags != 0u) {
		return -ZOS_EISDIR; /* a grouping entry, not an object */
	}
	desc = vfs_handle_alloc(pid, ZOS_HANDLE_FIRST_BOUND);
	if (desc == (struct vfs_descriptor *)0) {
		return -ZOS_EMFILE;
	}
	desc->type = pub->type;
	desc->endpoint = pub->number;
	desc->object = pub->object;
	desc->flags = (uint8_t)flags;
	*type = pub->type;
	return (int)desc->number;
}

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
	length = ns_component_length(norm + 1);
	rest = &norm[1u + length];
	if (ns_component_eq(norm + 1, length, "FS")) {
		if (vfs_ns_fs_endpoint == 0u) {
			return -ZOS_ENODEV; /* no filesystem driver in this build */
		}
		if (rest[0] == '\0') {
			/* The filesystem-namespace handle: `fs.h` binds the
			 * root and sends its name operations with the path
			 * remainder. */
			result = ns_fs_alloc(pid, ZOS_TYPE_FS, 0u, 0u);
			if (result >= 0) {
				*type = ZOS_TYPE_FS;
			}
			return result;
		}
		return ns_fs_bind_file(pid, rest, flags, type);
	}
	if (ns_component_eq(norm + 1, length, "SYSTEM")) {
		return vfs_ns_file_bind(pid, rest, flags, type);
	}
	if (ns_component_eq(norm + 1, length, "dev")) {
		return ns_device_bind(pid, rest, flags, type);
	}
	return -ZOS_ENOENT;
}

void vfs_ns_release_desc(struct vfs_descriptor *desc)
{
	/* The provider release: a bound ROMFS FILE instance is the entry
	 * itself, a `/FS` FILE instance is the driver's and device
	 * instances are permanent (36.7). */
	if (desc->endpoint == VFS_NS_FS_ENDPOINT) {
		vfs_ns_fs_release(desc->type, desc->object);
	}
	vfs_handle_release(desc);
}

int vfs_ns_unbind(uint8_t pid, uint8_t handle)
{
	struct vfs_descriptor *desc;

	/* The standard handles are not a caller's to release: `bind` never
	 * returns their numbers and a program that dropped its console
	 * could not report why (architecture 36.13).  Process teardown is
	 * the one caller that releases them (vfs_process_exit). */
	if (handle < ZOS_HANDLE_FIRST_BOUND) {
		return -ZOS_EINVAL;
	}
	desc = vfs_handle_find(pid, handle);
	if (desc == (struct vfs_descriptor *)0) {
		return -ZOS_EINVAL;
	}
	vfs_ns_release_desc(desc);
	return 0;
}

int vfs_ns_resolve(uint8_t pid, uint8_t handle, uint8_t *endpoint,
		   uint8_t *object)
{
	struct vfs_descriptor *desc = vfs_handle_find(pid, handle);

	if (desc == (struct vfs_descriptor *)0) {
		return -ZOS_EINVAL;
	}
	*endpoint = desc->endpoint;
	*object = desc->object;
	return 0;
}

/* --- standard handles ---------------------------------------------------- */

void vfs_ns_standard_handles(uint8_t pid)
{
	const struct vfs_ns_publication *pub = ns_pub_find("/console");
	uint8_t number;

	if (pub == (const struct vfs_ns_publication *)0) {
		return; /* no console driver in this build */
	}
	for (number = ZOS_HANDLE_STDOUT; number <= ZOS_HANDLE_STDIN;
	     ++number) {
		struct vfs_descriptor *desc = vfs_handle_alloc(pid, number);

		if (desc == (struct vfs_descriptor *)0) {
			return;
		}
		desc->type = ZOS_TYPE_CONSOLE;
		desc->endpoint = pub->number;
		desc->object = pub->object;
		desc->flags = 0u;
	}
}

/* --- enumeration --------------------------------------------------------- */

/* One entry, assembled on the gate stack and written to the caller's bank
 * through the checked copy gate (the common staging buffer is the gate's
 * single staging block; single-tasking makes it unambiguously ours). */
static int ns_emit(uint8_t bank, uint16_t out, uint16_t slot,
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
static uint8_t ns_room(uint16_t slot, uint16_t out_size)
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
	static const char names[3][7] = { "FS", "SYSTEM", "dev" };
	uint16_t slot = 0;

	while (*cookie < 3u && ns_room(slot, out_size) != 0u) {
		const char *name = names[*cookie];
		uint16_t length = ns_component_length(name);

		/* All three root entries are containers; their provider
		 * protocols are not bindable objects in M10. */
		if (ns_emit(bank, out, slot, name, length, 0u,
			    ZOS_NS_ENTRY_CONTAINER) != 0) {
			return -ZOS_EFAULT;
		}
		++*cookie;
		++slot;
		++*count;
	}
	return 0;
}

static int ns_enum_romfs(uint8_t bank, uint16_t *cookie, uint16_t out,
			 uint16_t out_size, uint16_t *count)
{
	static char name[ROMFS_NAME_SIZE];
	static romfs_file_t file;
	uint16_t slot = 0;

	while (ns_room(slot, out_size) != 0u) {
		uint16_t length = 0;
		int result = romfs_entry_at(*cookie, &file, name,
					    ROMFS_NAME_SIZE);

		if (result != 0) {
			return 0; /* end of stream */
		}
		while (name[length] != '\0') {
			++length;
		}
		/* ROMFS is flat: every entry is a bindable FILE. */
		result = ns_emit(bank, out, slot, name, length, ZOS_TYPE_FILE,
				 0u);
		if (result != 0) {
			return result;
		}
		++*cookie;
		++slot;
		++*count;
	}
	return 0;
}

/* Enumerate the rows directly below `rel` (`""` for `/dev`): the cookie is
 * the publication-table cursor, so a page resumes exactly where the
 * previous batch stopped. */
static int ns_enum_device(uint8_t bank, const char *rel, uint16_t *cookie,
			  uint16_t out, uint16_t out_size, uint16_t *count)
{
	uint16_t slot = 0;
	uint8_t i;

	for (i = (uint8_t)*cookie; i < vfs_ns_pub_count; ++i) {
		const char *component = ns_pub_child(
			vfs_ns_publications[i].name, rel);
		uint16_t length;

		if (component == (const char *)0) {
			continue;
		}
		length = ns_component_length(component);
		if (ns_room(slot, out_size) == 0u) {
			break; /* the batch is full; the cursor stays here */
		}
		if (ns_emit(bank, out, slot, component, length,
			    vfs_ns_publications[i].type,
			    vfs_ns_publications[i].flags) != 0) {
			return -ZOS_EFAULT;
		}
		++slot;
		++*count;
		*cookie = (uint16_t)(i + 1u);
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
	length = ns_component_length(norm + 1);
	rest = &norm[1u + length];
	if (ns_component_eq(norm + 1, length, "FS")) {
		if (vfs_ns_fs_endpoint == 0u) {
			return -ZOS_ENODEV;
		}
		return ns_enum_fs(rest, cookie, out, out_size, count);
	}
	if (ns_component_eq(norm + 1, length, "SYSTEM")) {
		if (rest[0] == '\0') {
			return ns_enum_romfs(bank, cookie, out, out_size,
					     count);
		}
		return -ZOS_ENOTDIR;
	}
	if (ns_component_eq(norm + 1, length, "dev")) {
		const struct vfs_ns_publication *pub;

		if (rest[0] == '\0') {
			return ns_enum_device(bank, "", cookie, out, out_size,
					      count);
		}
		pub = ns_pub_find(rest);
		if (pub == (const struct vfs_ns_publication *)0) {
			return -ZOS_ENOENT;
		}
		if (pub->flags == 0u) {
			return -ZOS_ENOTDIR; /* a bindable object */
		}
		return ns_enum_device(bank, rest, cookie, out, out_size,
				      count);
	}
	return -ZOS_ENOENT;
}

/* --- the FS provider: /FS routing to the filesystem driver -------------- */

/* Payload words 0..1 name the path: the driver window maps the bank-7
 * VFS workspace (the same handover the frame and the boot bundle use),
 * never the kernel image, so every spelling staged here must be a
 * workspace copy. */
static void ns_fs_put_path(uint8_t *payload, const char *path)
{
	uint16_t address = (uint16_t)path;

	payload[0] = (uint8_t)address;
	payload[1] = (uint8_t)(address >> 8);
}

/* The provider-root spelling, staged in the path view's spare slot: the
 * root is the one path with no workspace copy of its own, and the
 * literal "/" in the image is not readable through the driver window. */
static const char *ns_fs_root_path(void)
{
	VFS_WS_IN_B[0] = '/';
	VFS_WS_IN_B[1] = '\0';
	return VFS_WS_IN_B;
}

/* The driver-relative spelling of the normalized `/FS` path `norm`: the
 * driver's own convention is a leading slash (the same spelling a FILE
 * bind passes), and the root itself is "/".  "/FS/ZOSTEST" therefore
 * stages as "/ZOSTEST" and "/FS" as "/". */
static const char *ns_fs_rel(const char *norm)
{
	return (norm[3] == '\0') ? ns_fs_root_path() : (norm + 3);
}

/* Allocate one bound `/FS` object: the provider marker is its endpoint,
 * the driver's instance identity its object (0 for the namespace root). */
static int ns_fs_alloc(uint8_t pid, uint8_t type, uint8_t object,
		       uint16_t flags)
{
	struct vfs_descriptor *desc =
		vfs_handle_alloc(pid, ZOS_HANDLE_FIRST_BOUND);

	if (desc == (struct vfs_descriptor *)0) {
		return -ZOS_EMFILE;
	}
	desc->type = type;
	desc->endpoint = VFS_NS_FS_ENDPOINT;
	desc->object = object;
	desc->flags = (uint8_t)flags;
	return (int)desc->number;
}

/* ZFS_OP_CLOSE: release one driver instance. */
static void ns_fs_close(uint8_t object)
{
	struct zos_ipc_frame frame;

	frame.op = ZFS_OP_CLOSE;
	frame.flags = 0u;
	frame.desc = ZOS_IPC_DESC_NONE;
	frame.arg0 = 0u;
	frame.arg1 = 0u;
	frame.payload[0] = object;
	(void)ipc_nucleus_call(vfs_ns_fs_endpoint, &frame);
}

/* ZFS_OP_OPEN: bind one `/FS` path as a FILE instance.  `rel` is the
 * provider-relative remainder (it begins with '/', or is empty for the
 * provider root) in the fixed-core workspace the driver reads. */
static int ns_fs_bind_file(uint8_t pid, const char *rel, uint16_t flags,
			   uint8_t *type)
{
	struct zos_ipc_frame frame;
	uint8_t object;
	int result;

	if ((flags & (uint16_t)~((uint16_t)(ZFS_O_READ | ZFS_O_WRITE |
					    ZFS_O_CREATE | ZFS_O_EXCL |
					    ZFS_O_TRUNC))) != 0u) {
		return -ZOS_EINVAL;
	}
	frame.op = ZFS_OP_OPEN;
	frame.flags = 0u;
	frame.desc = ZOS_IPC_DESC_NONE;
	frame.arg0 = 0u;
	frame.arg1 = 0u;
	ns_fs_put_path(frame.payload, rel);
	frame.payload[2] = (uint8_t)flags;
	frame.payload[3] = (uint8_t)(flags >> 8);
	result = ipc_nucleus_call(vfs_ns_fs_endpoint, &frame);
	if (result < 0) {
		return result;
	}
	object = frame.payload[0];
	result = ns_fs_alloc(pid, ZOS_TYPE_FILE, object, flags);
	if (result < 0) {
		ns_fs_close(object);
		return result;
	}
	*type = ZOS_TYPE_FILE;
	return result;
}

/* Copy the namespace operation's caller path (1..127 caller-bank bytes;
 * RENAME packs "from\0to") into the workspace's copy-in slot.  The
 * buffer's bank follows the buffer-identity rule.  Returns 0 or a
 * negative ZOS_E* value. */
static int ns_fs_read_path(const struct zos_ipc_call_args *args)
{
	uint16_t length = args->length;
	uint16_t done = 0u;

	if (length == 0u || length >= ZOS_PATH_MAX ||
	    !zos_syscall_buffer_ok(args->buffer, length)) {
		return -ZOS_EINVAL;
	}
	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);
		uint16_t i;

		if (chunk > ZOS_SYSCALL_STAGE_SIZE) {
			chunk = ZOS_SYSCALL_STAGE_SIZE;
		}
		if (zos_syscall_bank_read((uint16_t)(args->buffer + done),
					  chunk) != 0u) {
			return -ZOS_EFAULT;
		}
		for (i = 0u; i < chunk; ++i) {
			VFS_WS_IN_A[done + i] = (char)zos_user_copy_staging[i];
		}
		done = (uint16_t)(done + chunk);
	}
	VFS_WS_IN_A[length] = '\0';
	return 0;
}

/* The normalized path names the `root` provider (and no other)? */
static uint8_t ns_path_under(const char *norm, const char *root)
{
	uint8_t length = 0;

	while (root[length] != '\0') {
		if (ns_fold((uint8_t)norm[length]) !=
		    ns_fold((uint8_t)root[length])) {
			return 0u;
		}
		++length;
	}
	return (uint8_t)(norm[length] == '\0' || norm[length] == '/');
}

/* One filesystem-namespace operation (`STAT`, `MKDIR`, `RMDIR`, `UNLINK`,
 * `RENAME`) on `norm` (RENAME adds `norm_b`): route it to the owning
 * provider and write the reply into the caller's frame. */
static int ns_fs_namespace_op(uint8_t op, const char *norm,
			      const char *norm_b, struct zos_ipc_frame *frame)
{
	int result;

	frame->op = op;
	frame->flags = 0u;
	frame->desc = ZOS_IPC_DESC_NONE;
	frame->arg0 = 0u;
	frame->arg1 = 0u;
	if (ns_path_under(norm, "/SYSTEM")) {
		/* ROMFS is read-only: only metadata has an answer. */
		if (op == ZFS_OP_STAT) {
			frame->payload[0] = norm[7];
			return vfs_ns_file_meta(norm + 7, frame);
		}
		return -ZOS_EROFS;
	}
	if (!ns_path_under(norm, VFS_NS_FS_ROOT)) {
		return -ZOS_ENOENT;
	}
	if (vfs_ns_fs_endpoint == 0u) {
		return -ZOS_ENODEV;
	}
	ns_fs_put_path(frame->payload, ns_fs_rel(norm));
	if (op == ZFS_OP_RENAME) {
		if (norm_b == (const char *)0 ||
		    !ns_path_under(norm_b, VFS_NS_FS_ROOT)) {
			return -ZOS_EXDEV;
		}
		ns_fs_put_path(&frame->payload[2], ns_fs_rel(norm_b));
	}
	result = ipc_nucleus_call(vfs_ns_fs_endpoint, frame);
	return result;
}

/* SYS_CALL on a bound `/FS` object: run the object's protocol at the
 * outermost call level (the syscall body calls this before the IPC layer),
 * so the driver it forwards to runs on the driver stack. */
int16_t vfs_ns_fs_call(uint8_t handle, uint8_t object,
		       struct zos_ipc_call_args *args)
{
	struct vfs_descriptor *desc =
		vfs_handle_find(vfs_owner_current(), handle);
	int result;

	if (desc == (struct vfs_descriptor *)0 ||
	    desc->endpoint != VFS_NS_FS_ENDPOINT) {
		return -ZOS_EINVAL;
	}
	if (args->op <= ZFILE_OP_SYNC) {
		/* FILE protocol: the driver acts on the bound instance. */
		if (desc->type != ZOS_TYPE_FILE || vfs_ns_fs_endpoint == 0u) {
			return -ZOS_EINVAL;
		}
		args->frame.payload[0] = object;
		return ipc_syscall_call_object(args, vfs_ns_fs_endpoint,
					       object);
	}
	if (args->op < ZFS_OP_MKDIR || args->op > ZFS_OP_STAT ||
	    desc->type != ZOS_TYPE_FS) {
		return -ZOS_EINVAL;
	}
	vfs_ws_enter(VFS_WS_VIEW_PATH);
	result = ns_fs_read_path(args);
	if (result == 0) {
		uint8_t has_b = 0u;

		if (args->op == ZFS_OP_RENAME) {
			/* Split "from\0to" into the two copy-in slots. */
			uint16_t i = 0u;

			while (VFS_WS_IN_A[i] != '\0' && i < args->length) {
				++i;
			}
			if (i >= args->length || i + 1u >= args->length) {
				result = -ZOS_EINVAL;
			} else {
				uint16_t k = (uint16_t)(i + 1u);
				uint16_t n = 0u;

				while (k + n < args->length) {
					VFS_WS_IN_B[n] = VFS_WS_IN_A[k + n];
					++n;
				}
				VFS_WS_IN_A[i] = '\0';
				VFS_WS_IN_B[n] = '\0';
				has_b = 1u;
			}
		}
		if (result == 0) {
			result = vfs_path_normalize_at(
				vfs_cwd_for(process_current_pid()),
				VFS_WS_IN_A, VFS_WS_NORM_A, ZOS_PATH_MAX);
		}
		if (result == 0 && has_b != 0u) {
			result = vfs_path_normalize_at(
				vfs_cwd_for(process_current_pid()),
				VFS_WS_IN_B, VFS_WS_NORM_B, ZOS_PATH_MAX);
		}
		if (result == 0) {
			/* The frame must cross to the bank-6 driver in fixed
			 * core: `args` lives in the caller's own bank, which
			 * the driver's window cannot see, so the call runs on
			 * a staged copy (the same convention
			 * ipc_syscall_call_object() applies to FILE ops) and
			 * both halves of the reply travel back. */
			struct zos_ipc_frame frame = args->frame;

			result = ns_fs_namespace_op(
				args->op, VFS_WS_NORM_A,
				(has_b != 0u) ? (const char *)VFS_WS_NORM_B
					      : (const char *)0,
				&frame);
			args->frame = frame;
			args->status = (int16_t)result;
		}
	}
	vfs_ws_leave(VFS_WS_VIEW_PATH);
	return (int16_t)result;
}

void vfs_ns_fs_release(uint8_t type, uint8_t object)
{
	if (type == ZOS_TYPE_FILE && object != 0u &&
	    vfs_ns_fs_endpoint != 0u) {
		ns_fs_close(object);
	}
}

/* One nucleus-side FILE call on a bound handle (the cold loader). */
int16_t vfs_ns_file_call(uint8_t handle, uint8_t object,
			 struct zos_ipc_frame *frame, uint16_t length,
			 uint8_t dir)
{
	struct vfs_descriptor *desc;

	desc = vfs_handle_find(vfs_owner_current(), handle);

	if (desc == (struct vfs_descriptor *)0) {
		return -ZOS_EINVAL;
	}
	if (desc->endpoint == VFS_NS_FS_ENDPOINT) {
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
		return ipc_nucleus_call_object(desc->endpoint, object, handle,
					       frame);
	}
	return ipc_nucleus_call_object_common(desc->endpoint, object, handle,
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
	length = ns_component_length(norm + 1);
	if (ns_component_eq(norm + 1, length, "FS")) {
		int result;

		if (vfs_ns_fs_endpoint == 0u) {
			return -ZOS_ENODEV;
		}
		frame.op = ZFS_OP_STAT;
		frame.flags = 0u;
		frame.desc = ZOS_IPC_DESC_NONE;
		frame.arg0 = 0u;
		frame.arg1 = 0u;
		ns_fs_put_path(frame.payload, ns_fs_rel(norm));
		result = ipc_nucleus_call(vfs_ns_fs_endpoint, &frame);
		if (result < 0) {
			return result;
		}
		*ok = (uint8_t)((frame.payload[0] == ZFS_TYPE_DIR) ? 1u : 0u);
		return 0;
	}
	if (ns_component_eq(norm + 1, length, "SYSTEM")) {
		if (norm[1u + length] == '\0') {
			*ok = 1u;
			return 0;
		}
		return -ZOS_ENOTDIR; /* ROMFS is flat */
	}
	if (ns_component_eq(norm + 1, length, "dev")) {
		const char *rel = &norm[1u + length];

		if (rel[0] == '\0') {
			*ok = 1u;
			return 0;
		}
		{
			const struct vfs_ns_publication *pub = ns_pub_find(rel);

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

/* `ns_enum` below `/FS`: forward to the driver, whose entries land in the
 * caller's buffer through the descriptor copy gate. */
static int ns_enum_fs(const char *rel, uint16_t *cookie, uint16_t out,
		      uint16_t out_size, uint16_t *count)
{
	struct zos_ipc_frame frame;
	int16_t result;

	frame.op = ZFS_OP_ENUM;
	frame.flags = 0u;
	frame.desc = ZOS_IPC_DESC_NONE;
	frame.arg0 = 0u;
	frame.arg1 = 0u;
	ns_fs_put_path(frame.payload,
		       (rel[0] == '\0') ? ns_fs_root_path() : rel);
	frame.payload[2] = (uint8_t)*cookie;
	frame.payload[3] = (uint8_t)(*cookie >> 8);
	frame.payload[4] = (uint8_t)out_size;
	frame.payload[5] = (uint8_t)(out_size >> 8);
	result = ipc_nucleus_call_gate(vfs_ns_fs_endpoint, &frame, out,
				       out_size, ZOS_IPC_DESC_OUT);
	if (result < 0) {
		return (int)result;
	}
	*cookie = (uint16_t)((uint16_t)frame.payload[2] |
			     ((uint16_t)frame.payload[3] << 8));
	*count = (uint16_t)result;
	return 0;
}

/* --- nucleus setup -------------------------------------------------------- */

/* The VFS bootstrap (kernel/main.c): the ROMFS reader first, so `/SYSTEM`
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
