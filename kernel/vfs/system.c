/* KonaOS VFS part: /system, the ROMFS provider. */
/* KonaOS M10 ROMFS FILE provider (package 42a, architecture 36.13).
 *
 * The static `/system` namespace provider: nucleus code over the one
 * ROMFS reader (kernel/romfs), usable before any resident driver exists.
 * `bind("/system/name", flags)` resolves the flat name with
 * `romfs_lookup_ci` and creates a FILE instance inline in the handle
 * entry, so the ROMFS provider needs no second table; `bind("/system", 0)`
 * yields the filesystem-namespace handle the name operations use
 * (`fs.h`'s `stat`; every mutation is `-KOS_EROFS`), and the namespace
 * operations themselves are routed by kernel/vfs/router.c.  The FILE
 * endpoint
 * is an in-process *nucleus* endpoint installed by `vfs_ns_init()`, and a
 * call reaches `vfs_ns_file_dispatch()` through the same handle-call path
 * as a bank-6 endpoint (frame staging, descriptor capture, nesting and
 * re-entrancy all belong to the fixed-core IPC substrate).
 *
 * The dispatch serves the FILE protocol's read, seek and metadata
 * operations (abi/kos/protocol.h KFILE_OP_*).  Data moves from ROM to the
 * caller's buffer through the ROMFS reader's staging and the checked
 * descriptor copy, one bounded chunk at a time; write is the filesystem
 * driver's operation and arrives with IOC-FAT in package 42b.
 */

#include "kernel/vfs/vfs.h"
#include "kernel/ipc/ipc.h"
#include "kernel/syscall/diag.h"

#include "kos/fs.h"
#include "kos/object.h"
#include "kos/protocol.h"

#include "kos/errno.h"
#include "process.h"
#include "romfs.h"

/* Largest transfer per ROM staging/copy chunk (the common staging block
 * both primitives use). */
#define NS_FILE_CHUNK KOS_SYSCALL_STAGE_SIZE

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
	struct vfs_handle_entry *entry;
	romfs_file_t romfs;

	if (name == (const char *)0 || type == (uint8_t *)0) {
		return -KOS_EINVAL;
	}
	if ((flags & (uint16_t)~((uint16_t)(KFS_O_READ | KFS_O_WRITE |
					    KFS_O_CREATE | KFS_O_EXCL |
					    KFS_O_TRUNC))) != 0u) {
		return -KOS_EINVAL;
	}
	if ((flags & (uint16_t)(KFS_O_WRITE | KFS_O_CREATE | KFS_O_EXCL |
				KFS_O_TRUNC)) != 0u) {
		return -KOS_EROFS; /* ROMFS is read-only */
	}
	if (name[0] == '\0') {
		/* `/system` itself: the filesystem-namespace handle
		 * (`fs.h`'s `stat` and the mutations it refuses). */
		struct vfs_handle_entry *root =
			vfs_handle_alloc(pid, KOS_HANDLE_FIRST_BOUND);

		if (root == (struct vfs_handle_entry *)0) {
			return -KOS_EMFILE;
		}
		root->type = KOS_TYPE_FS;
		root->endpoint = VFS_NS_FS_ENDPOINT;
		root->object = 0u;
		root->flags = 0u;
		*type = KOS_TYPE_FS;
		return (int)root->number;
	}
	if (name[0] != '/') {
		return -KOS_EINVAL;
	}
	if (romfs_lookup_ci(&name[1], &romfs) != 0) {
		return -KOS_ENOENT;
	}
	entry = vfs_handle_alloc(pid, KOS_HANDLE_FIRST_BOUND);
	if (entry == (struct vfs_handle_entry *)0) {
		return -KOS_EMFILE;
	}
	entry->type = KOS_TYPE_FILE;
	entry->endpoint = VFS_NS_FILE_ENDPOINT;
	entry->object = 0u;
	entry->flags = (uint8_t)flags;
	entry->u.ns.romfs = romfs;
	entry->u.ns.offset = 0u;
	entry->u.ns.flags = (uint8_t)flags;
	*type = KOS_TYPE_FILE;
	return (int)entry->number;
}

/* KFILE_OP_READ: `arg0` = offset inside the caller buffer, `arg1` =
 * length; the descriptor is an OUT capture.  Returns the bytes
 * transferred (0 at end of file). */
static int16_t ns_file_read(struct vfs_ns_file *file,
			    const struct kos_ipc_frame *frame)
{
	uint16_t done = 0u;
	uint16_t offset = frame->arg0;
	uint16_t length = frame->arg1;
	int16_t result;

	if (frame->desc == KOS_IPC_DESC_NONE) {
		return -KOS_EINVAL;
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
				    kos_user_copy_staging, chunk);
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

/* KFILE_OP_SEEK: `arg0` = whence, payload[1..4] = signed 32-bit offset;
 * the reply payload carries the resulting position at the same place. */
static int16_t ns_file_seek(struct vfs_ns_file *file,
			    struct kos_ipc_frame *frame)
{
	int32_t base;
	int32_t result;
	uint8_t *payload = &frame->payload[1];

	if (frame->arg0 == KFS_SEEK_SET) {
		base = 0;
	} else if (frame->arg0 == KFS_SEEK_CUR) {
		base = (int32_t)file->offset;
	} else if (frame->arg0 == KFS_SEEK_END) {
		base = (int32_t)file->romfs.size;
	} else {
		return -KOS_EINVAL;
	}
	result = base + (int32_t)ns_file_le32(payload);
	if (result < 0) {
		return -KOS_EINVAL;
	}
	file->offset = (uint32_t)result;
	ns_file_store32(payload, file->offset);
	return 0;
}

/* KFILE_OP_META: the reply payload holds a packed `struct kfs_stat`. */
static int16_t ns_file_meta(struct vfs_ns_file *file,
			    struct kos_ipc_frame *frame)
{
	struct kfs_stat st;
	const uint8_t *src = (const uint8_t *)&st;
	uint8_t i;

	st.type = KFS_TYPE_FILE;
	st.attributes = 0u;
	st.reserved = 0u;
	st.size = file->romfs.size;
	st.ctime = KFS_TIME_UNKNOWN;
	st.mtime = KFS_TIME_UNKNOWN;
	for (i = 0; i < (uint8_t)sizeof(st); ++i) {
		frame->payload[i] = src[i];
	}
	return 0;
}

/* `KFS_OP_STAT` for one `/system` path: `rel` is the provider-relative
 * remainder ("" for the ROMFS root).  The reply payload holds the packed
 * `struct kfs_stat`, exactly like the driver's STAT. */
int vfs_ns_file_meta(const char *rel, struct kos_ipc_frame *frame)
{
	struct kfs_stat st;
	const uint8_t *src = (const uint8_t *)&st;
	romfs_file_t romfs;
	uint8_t i;

	if (rel[0] == '\0') {
		st.type = KFS_TYPE_DIR; /* ROMFS is a flat directory */
		st.attributes = 0u;
		st.reserved = 0u;
		st.size = 0u;
	} else {
		if (rel[0] != '/' || rel[1] == '\0') {
			return -KOS_EINVAL;
		}
		if (romfs_lookup_ci(&rel[1], &romfs) != 0) {
			return -KOS_ENOENT;
		}
		st.type = KFS_TYPE_FILE;
		st.attributes = 0u;
		st.reserved = 0u;
		st.size = romfs.size;
	}
	st.ctime = KFS_TIME_UNKNOWN;
	st.mtime = KFS_TIME_UNKNOWN;
	src = (const uint8_t *)&st;
	for (i = 0u; i < (uint8_t)sizeof(st); ++i) {
		frame->payload[i] = src[i];
	}
	return 0;
}

int16_t vfs_ns_file_dispatch(uint8_t handle, uint8_t object,
			     struct kos_ipc_frame *frame)
{
	struct vfs_handle_entry *entry =
		vfs_handle_find(vfs_owner_current(), handle);

	(void)object; /* the FILE instance is the handle entry itself */
	if (entry == (struct vfs_handle_entry *)0 ||
	    entry->endpoint != VFS_NS_FILE_ENDPOINT ||
	    entry->type != KOS_TYPE_FILE) {
		return -KOS_EINVAL;
	}
	if (frame->flags != 0u) {
		return -KOS_EINVAL; /* the FILE operations define no flags */
	}
	switch (frame->op) {
	case KFILE_OP_READ:
		return ns_file_read(&entry->u.ns, frame);
	case KFILE_OP_SEEK:
		return ns_file_seek(&entry->u.ns, frame);
	case KFILE_OP_META:
		return ns_file_meta(&entry->u.ns, frame);
	case KFILE_OP_WRITE:
		return -KOS_EROFS;
	default:
		return -KOS_EINVAL;
	}
}

int vfs_enum_romfs(uint8_t bank, uint16_t *cookie, uint16_t out,
			 uint16_t out_size, uint16_t *count)
{
	static char name[ROMFS_NAME_SIZE];
	static romfs_file_t file;
	uint16_t slot = 0;

	while (vfs_room(slot, out_size) != 0u) {
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
		result = vfs_emit(bank, out, slot, name, length, KOS_TYPE_FILE,
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
