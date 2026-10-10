/* ZephyrOS M10 ROMFS FILE provider (package 42a, architecture 36.13).
 *
 * The static `/SYSTEM` namespace provider: nucleus code over the one
 * ROMFS reader (kernel/romfs), usable before any resident driver exists.
 * `bind("/SYSTEM/name", flags)` resolves the flat name with
 * `romfs_lookup_ci` and creates a FILE instance inline in the handle
 * entry, so the ROMFS provider needs no second table; `bind("/SYSTEM", 0)`
 * yields the filesystem-namespace handle the name operations use
 * (`fs.h`'s `stat`; every mutation is `-ZOS_EROFS`), and the namespace
 * operations themselves are routed by kernel/vfs/ns.c.  The FILE endpoint
 * is an in-process *nucleus* endpoint installed by `vfs_ns_init()`, and a
 * call reaches `vfs_ns_file_dispatch()` through the same handle-call path
 * as a bank-6 endpoint (frame staging, descriptor capture, nesting and
 * re-entrancy all belong to the fixed-core IPC substrate).
 *
 * The dispatch serves the FILE protocol's read, seek and metadata
 * operations (abi/zos/protocol.h ZFILE_OP_*).  Data moves from ROM to the
 * caller's buffer through the ROMFS reader's staging and the checked
 * descriptor copy, one bounded chunk at a time; write is the filesystem
 * driver's operation and arrives with IOC-FAT in package 42b.
 */

#include "kernel/vfs/vfs.h"
#include "kernel/vfs/ns.h"
#include "kernel/ipc/ipc.h"
#include "kernel/syscall/diag.h"

#include "zos/fs.h"
#include "zos/object.h"
#include "zos/protocol.h"

#include "zephyros/errno.h"
#include "zephyros/process.h"
#include "zephyros/romfs.h"

/* Largest transfer per ROM staging/copy chunk (the common staging block
 * both primitives use). */
#define NS_FILE_CHUNK ZOS_SYSCALL_STAGE_SIZE

static uint32_t ns_file_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void ns_file_store32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)(value & 0xFFu);
	p[1] = (uint8_t)((value >> 8) & 0xFFu);
	p[2] = (uint8_t)((value >> 16) & 0xFFu);
	p[3] = (uint8_t)((value >> 24) & 0xFFu);
}

int vfs_ns_file_bind(uint8_t pid, const char *name, uint16_t flags,
		     uint8_t *type)
{
	struct vfs_descriptor *desc;
	romfs_file_t romfs;

	if (name == (const char *)0 || type == (uint8_t *)0) {
		return -ZOS_EINVAL;
	}
	if ((flags & (uint16_t)~((uint16_t)(ZFS_O_READ | ZFS_O_WRITE |
					    ZFS_O_CREATE | ZFS_O_EXCL |
					    ZFS_O_TRUNC))) != 0u) {
		return -ZOS_EINVAL;
	}
	if ((flags & (uint16_t)(ZFS_O_WRITE | ZFS_O_CREATE | ZFS_O_EXCL |
				ZFS_O_TRUNC)) != 0u) {
		return -ZOS_EROFS; /* ROMFS is read-only */
	}
	if (name[0] == '\0') {
		/* `/SYSTEM` itself: the filesystem-namespace handle
		 * (`fs.h`'s `stat` and the mutations it refuses). */
		struct vfs_descriptor *root =
			vfs_handle_alloc(pid, ZOS_HANDLE_FIRST_BOUND);

		if (root == (struct vfs_descriptor *)0) {
			return -ZOS_EMFILE;
		}
		root->type = ZOS_TYPE_FS;
		root->endpoint = VFS_NS_FS_ENDPOINT;
		root->object = 0u;
		root->flags = 0u;
		*type = ZOS_TYPE_FS;
		return (int)root->number;
	}
	if (name[0] != '/') {
		return -ZOS_EINVAL;
	}
	if (romfs_lookup_ci(&name[1], &romfs) != 0) {
		return -ZOS_ENOENT;
	}
	desc = vfs_handle_alloc(pid, ZOS_HANDLE_FIRST_BOUND);
	if (desc == (struct vfs_descriptor *)0) {
		return -ZOS_EMFILE;
	}
	desc->type = ZOS_TYPE_FILE;
	desc->endpoint = VFS_NS_FILE_ENDPOINT;
	desc->object = 0u;
	desc->flags = (uint8_t)flags;
	desc->u.ns.romfs = romfs;
	desc->u.ns.offset = 0u;
	desc->u.ns.flags = (uint8_t)flags;
	*type = ZOS_TYPE_FILE;
	return (int)desc->number;
}

/* ZFILE_OP_READ: `arg0` = offset inside the caller buffer, `arg1` =
 * length; the descriptor is an OUT capture.  Returns the bytes
 * transferred (0 at end of file). */
static int16_t ns_file_read(struct vfs_ns_file *file,
			    const struct zos_ipc_frame *frame)
{
	uint16_t done = 0u;
	uint16_t offset = frame->arg0;
	uint16_t length = frame->arg1;
	int16_t result;

	if (frame->desc == ZOS_IPC_DESC_NONE) {
		return -ZOS_EINVAL;
	}
	while (done < length) {
		uint32_t remaining = file->romfs.size - file->offset;
		uint16_t chunk = (uint16_t)(length - done);
		int copied;

		if (remaining == 0u) {
			break; /* end of file: a short successful read */
		}
		if ((uint32_t)chunk > remaining) {
			chunk = (uint16_t)remaining;
		}
		if (chunk > NS_FILE_CHUNK) {
			chunk = NS_FILE_CHUNK;
		}
		copied = romfs_read(&file->romfs, file->offset,
				    zos_user_copy_staging, chunk);
		if (copied < 0) {
			return (int16_t)copied;
		}
		if (copied == 0) {
			break;
		}
		result = ipc_nucleus_copy_out(frame->desc,
					      (uint16_t)(offset + done),
					      (uint16_t)copied);
		if (result < 0) {
			return result;
		}
		file->offset += (uint32_t)copied;
		done = (uint16_t)(done + (uint16_t)copied);
	}
	return (int16_t)done;
}

/* ZFILE_OP_SEEK: `arg0` = whence, payload[1..4] = signed 32-bit offset;
 * the reply payload carries the resulting position at the same place. */
static int16_t ns_file_seek(struct vfs_ns_file *file,
			    struct zos_ipc_frame *frame)
{
	int32_t base;
	int32_t result;
	uint8_t *payload = &frame->payload[1];

	if (frame->arg0 == ZFS_SEEK_SET) {
		base = 0;
	} else if (frame->arg0 == ZFS_SEEK_CUR) {
		base = (int32_t)file->offset;
	} else if (frame->arg0 == ZFS_SEEK_END) {
		base = (int32_t)file->romfs.size;
	} else {
		return -ZOS_EINVAL;
	}
	result = base + (int32_t)ns_file_le32(payload);
	if (result < 0) {
		return -ZOS_EINVAL;
	}
	file->offset = (uint32_t)result;
	ns_file_store32(payload, file->offset);
	return 0;
}

/* ZFILE_OP_META: the reply payload holds a packed `struct zfs_stat`. */
static int16_t ns_file_meta(struct vfs_ns_file *file,
			    struct zos_ipc_frame *frame)
{
	struct zfs_stat st;
	const uint8_t *src = (const uint8_t *)&st;
	uint8_t i;

	st.type = ZFS_TYPE_FILE;
	st.attributes = 0u;
	st.reserved = 0u;
	st.size = file->romfs.size;
	st.ctime = ZFS_TIME_UNKNOWN;
	st.mtime = ZFS_TIME_UNKNOWN;
	for (i = 0; i < (uint8_t)sizeof(st); ++i) {
		frame->payload[i] = src[i];
	}
	return 0;
}

/* `ZFS_OP_STAT` for one `/SYSTEM` path: `rel` is the provider-relative
 * remainder ("" for the ROMFS root).  The reply payload holds the packed
 * `struct zfs_stat`, exactly like the driver's STAT. */
int vfs_ns_file_meta(const char *rel, struct zos_ipc_frame *frame)
{
	struct zfs_stat st;
	const uint8_t *src = (const uint8_t *)&st;
	romfs_file_t romfs;
	uint8_t i;

	if (rel[0] == '\0') {
		st.type = ZFS_TYPE_DIR; /* ROMFS is a flat directory */
		st.attributes = 0u;
		st.reserved = 0u;
		st.size = 0u;
	} else {
		if (rel[0] != '/' || rel[1] == '\0') {
			return -ZOS_EINVAL;
		}
		if (romfs_lookup_ci(&rel[1], &romfs) != 0) {
			return -ZOS_ENOENT;
		}
		st.type = ZFS_TYPE_FILE;
		st.attributes = 0u;
		st.reserved = 0u;
		st.size = romfs.size;
	}
	st.ctime = ZFS_TIME_UNKNOWN;
	st.mtime = ZFS_TIME_UNKNOWN;
	src = (const uint8_t *)&st;
	for (i = 0u; i < (uint8_t)sizeof(st); ++i) {
		frame->payload[i] = src[i];
	}
	return 0;
}

int16_t vfs_ns_file_dispatch(uint8_t handle, uint8_t object,
			     struct zos_ipc_frame *frame)
{
	struct vfs_descriptor *desc =
		vfs_handle_find(vfs_owner_current(), handle);

	(void)object; /* the FILE instance is the handle entry itself */
	if (desc == (struct vfs_descriptor *)0 ||
	    desc->endpoint != VFS_NS_FILE_ENDPOINT ||
	    desc->type != ZOS_TYPE_FILE) {
		return -ZOS_EINVAL;
	}
	if (frame->flags != 0u) {
		return -ZOS_EINVAL; /* the FILE operations define no flags */
	}
	switch (frame->op) {
	case ZFILE_OP_READ:
		return ns_file_read(&desc->u.ns, frame);
	case ZFILE_OP_SEEK:
		return ns_file_seek(&desc->u.ns, frame);
	case ZFILE_OP_META:
		return ns_file_meta(&desc->u.ns, frame);
	case ZFILE_OP_WRITE:
		return -ZOS_EROFS;
	default:
		return -ZOS_EINVAL;
	}
}
