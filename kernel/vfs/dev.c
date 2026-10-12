/* ZephyrOS VFS part: /dev, the DEVICE provider. */

#include "kernel/vfs/vfs.h"
#include "kernel/lib/name.h"
#include "kernel/ipc/ipc.h"
#include "kernel/syscall/diag.h"

#include "kernel.h"
#include "zos/errno.h"
#include "process.h"
#include "romfs.h"

#include "zos/protocol.h"

static struct vfs_ns_publication
	vfs_ns_publications[VFS_NS_PUB_MAX];
uint8_t vfs_ns_pub_count;

/* --- publication table -------------------------------------------------- */

static struct vfs_ns_publication *ns_pub_new(void)
{
	if (vfs_ns_pub_count >= VFS_NS_PUB_MAX) {
		return (struct vfs_ns_publication *)0;
	}
	return &vfs_ns_publications[vfs_ns_pub_count++];
}

const struct vfs_ns_publication *vfs_pub_find(const char *rel)
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

uint8_t vfs_ns_publication_endpoint(const char *rel)
{
	const struct vfs_ns_publication *pub = vfs_pub_find(rel);

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
			    vfs_fold((uint8_t)name[i]) !=
			    vfs_fold((uint8_t)rel[i])) {
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
	/* The `/fs` root is the filesystem-namespace publication: it names
	 * the build's filesystem driver and stays out of the `/dev` table
	 * (the FS provider routes to it). */
	if (length == 3u && type == ZOS_TYPE_FS &&
	    vfs_component_eq(name + 1, 2u, "fs")) {
		if (object != 0u) {
			return -ZOS_EINVAL;
		}
		if (vfs_ns_fs_endpoint != 0u) {
			return -ZOS_EEXIST;
		}
		vfs_ns_fs_endpoint = number;
		return 0;
	}
	if (vfs_pub_find(name) != (const struct vfs_ns_publication *)0) {
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
		if (vfs_pub_find(parent) !=
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
int vfs_device_bind(uint8_t pid, const char *rel, uint16_t flags,
			  uint8_t *type)
{
	const struct vfs_ns_publication *pub = vfs_pub_find(rel);
	struct vfs_handle_entry *entry;

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
	entry = vfs_handle_alloc(pid, ZOS_HANDLE_FIRST_BOUND);
	if (entry == (struct vfs_handle_entry *)0) {
		return -ZOS_EMFILE;
	}
	entry->type = pub->type;
	entry->endpoint = pub->number;
	entry->object = pub->object;
	entry->flags = (uint8_t)flags;
	*type = pub->type;
	return (int)entry->number;
}

/* Enumerate the rows directly below `rel` (`""` for `/dev`): the cookie is
 * the publication-table cursor, so a page resumes exactly where the
 * previous batch stopped. */
int vfs_enum_device(uint8_t bank, const char *rel, uint16_t *cookie,
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
		length = vfs_component_length(component);
		if (vfs_room(slot, out_size) == 0u) {
			break; /* the batch is full; the cursor stays here */
		}
		if (vfs_emit(bank, out, slot, component, length,
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
