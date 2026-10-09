/* ZephyrOS M10 VFS root router and static namespace providers (package 42a).
 *
 * Architecture 36.13: the VFS owns `/` and routes a normalized global
 * path, ASCII case-insensitively, by its first component to the three
 * static providers - `/FS`, `/SYSTEM` and `/dev`.  The selected provider
 * owns everything below its root; this module never interprets a
 * provider's internal hierarchy beyond selecting the provider.
 *
 *   FS provider      nucleus routing; package 42b forwards below `/FS` to
 *                    the filesystem driver.  Until then every `/FS` bind
 *                    is ZOS_ENODEV.
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

#include "zephyros/errno.h"
#include "zephyros/process.h"
#include "zephyros/romfs.h"

static struct vfs_ns_publication
	vfs_ns_publications[VFS_NS_PUB_MAX];
static uint8_t vfs_ns_pub_count;

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

	if (norm == (const char *)0 || norm[0] != '/' ||
	    type == (uint8_t *)0) {
		return -ZOS_EINVAL;
	}
	length = ns_component_length(norm + 1);
	rest = &norm[1u + length];
	if (ns_component_eq(norm + 1, length, "FS")) {
		/* Package 42b: the FS provider forwards below `/FS` to the
		 * selected filesystem driver. */
		return -ZOS_ENODEV;
	}
	if (ns_component_eq(norm + 1, length, "SYSTEM")) {
		return vfs_ns_file_bind(pid, rest, flags, type);
	}
	if (ns_component_eq(norm + 1, length, "dev")) {
		return ns_device_bind(pid, rest, flags, type);
	}
	return -ZOS_ENOENT;
}

int vfs_ns_unbind(uint8_t pid, uint8_t handle)
{
	struct vfs_descriptor *desc;

	/* The standard handles are not a caller's to release: `bind` never
	 * returns their numbers and a program that dropped its console
	 * could not report why (architecture 36.13). */
	if (handle < ZOS_HANDLE_FIRST_BOUND) {
		return -ZOS_EINVAL;
	}
	desc = vfs_handle_find(pid, handle);
	if (desc == (struct vfs_descriptor *)0) {
		return -ZOS_EINVAL;
	}
	/* The provider release: a bound ROMFS FILE instance is the entry
	 * itself, and device instances are permanent (36.7). */
	vfs_handle_release(desc);
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
		return -ZOS_ENODEV; /* package 42b */
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

/* --- nucleus setup -------------------------------------------------------- */

void vfs_ns_reset(void)
{
	vfs_ns_pub_count = 0;
}

int vfs_ns_init(void)
{
	if (ipc_install_nucleus_endpoint(VFS_NS_FILE_ENDPOINT, 0u,
					 vfs_ns_file_dispatch) < 0) {
		return -ZOS_ENOSPC;
	}
	return 0;
}
