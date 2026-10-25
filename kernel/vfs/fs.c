/* KonaOS VFS part: /fs, the forwarding provider. */

#include "kernel/vfs/vfs.h"
#include "kernel/lib/name.h"
#include "kernel/ipc/ipc.h"
#include "kernel/syscall/diag.h"

#include "kernel.h"
#include "kos/errno.h"
#include "process.h"
#include "romfs.h"

#include "kos/protocol.h"

/* The filesystem driver's endpoint, from the host-built `/fs` publication
 * (0 when the build selected no filesystem driver). */
uint8_t vfs_ns_fs_endpoint;

/* --- the FS provider: /fs routing to the filesystem driver -------------- */

/* Payload words 0..1 name the path: the driver window maps the bank-7
 * VFS workspace (the same handover the frame and the boot bundle use),
 * never the kernel image, so every spelling staged here must be a
 * workspace copy. */
void vfs_fs_put_path(uint8_t *payload, const char *path)
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

/* The driver-relative spelling of the normalized `/fs` path `norm`: the
 * driver's own convention is a leading slash (the same spelling a FILE
 * bind passes), and the root itself is "/".  "/fs/KOSTEST" therefore
 * stages as "/KOSTEST" and "/fs" as "/". */
const char *vfs_fs_rel(const char *norm)
{
	return (norm[3] == '\0') ? ns_fs_root_path() : (norm + 3);
}

/* Allocate one bound `/fs` object: the provider marker is its endpoint,
 * the driver's instance identity its object (0 for the namespace root). */
int vfs_fs_alloc(uint8_t pid, uint8_t type, uint8_t object,
		       uint16_t flags)
{
	struct vfs_handle_entry *entry =
		vfs_handle_alloc(pid, KOS_HANDLE_FIRST_BOUND);

	if (entry == (struct vfs_handle_entry *)0) {
		return -KOS_EMFILE;
	}
	entry->type = type;
	entry->endpoint = VFS_NS_FS_ENDPOINT;
	entry->object = object;
	entry->flags = (uint8_t)flags;
	return (int)entry->number;
}

/* KFS_OP_CLOSE: release one driver instance. */
static void ns_fs_close(uint8_t object)
{
	struct kos_ipc_frame frame;

	frame.op = KFS_OP_CLOSE;
	frame.flags = 0u;
	frame.desc = KOS_IPC_DESC_NONE;
	frame.arg0 = 0u;
	frame.arg1 = 0u;
	frame.payload[0] = object;
	(void)ipc_nucleus_call(vfs_ns_fs_endpoint, &frame);
}

/* KFS_OP_OPEN: bind one `/fs` path as a FILE instance.  `rel` is the
 * provider-relative remainder (it begins with '/', or is empty for the
 * provider root) in the fixed-core workspace the driver reads. */
int vfs_fs_bind_file(uint8_t pid, const char *rel, uint16_t flags,
			   uint8_t *type)
{
	struct kos_ipc_frame frame;
	uint8_t object;
	int result;

	if ((flags & (uint16_t)~((uint16_t)(KFS_O_READ | KFS_O_WRITE |
					    KFS_O_CREATE | KFS_O_EXCL |
					    KFS_O_TRUNC))) != 0u) {
		return -KOS_EINVAL;
	}
	frame.op = KFS_OP_OPEN;
	frame.flags = 0u;
	frame.desc = KOS_IPC_DESC_NONE;
	frame.arg0 = 0u;
	frame.arg1 = 0u;
	vfs_fs_put_path(frame.payload, rel);
	frame.payload[2] = (uint8_t)flags;
	frame.payload[3] = (uint8_t)(flags >> 8);
	result = ipc_nucleus_call(vfs_ns_fs_endpoint, &frame);
	if (result < 0) {
		return result;
	}
	object = frame.payload[0];
	result = vfs_fs_alloc(pid, KOS_TYPE_FILE, object, flags);
	if (result < 0) {
		ns_fs_close(object);
		return result;
	}
	*type = KOS_TYPE_FILE;
	return result;
}

/* Copy the namespace operation's caller path (1..127 caller-bank bytes;
 * RENAME packs "from\0to") into the workspace's copy-in slot.  The
 * buffer's bank follows the buffer-identity rule.  Returns 0 or a
 * negative KOS_E* value. */
int vfs_fs_read_path(const struct kos_ipc_call_args *args)
{
	uint16_t length = args->length;
	uint16_t done = 0u;

	if (length == 0u || length >= KOS_PATH_MAX ||
	    !kos_syscall_buffer_ok(args->buffer, length)) {
		return -KOS_EINVAL;
	}
	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);
		uint16_t i;

		if (chunk > KOS_SYSCALL_STAGE_SIZE) {
			chunk = KOS_SYSCALL_STAGE_SIZE;
		}
		if (kos_syscall_bank_read((uint16_t)(args->buffer + done),
					  chunk) != 0u) {
			return -KOS_EFAULT;
		}
		for (i = 0u; i < chunk; ++i) {
			VFS_WS_IN_A[done + i] = (char)kos_user_copy_staging[i];
		}
		done = (uint16_t)(done + chunk);
	}
	VFS_WS_IN_A[length] = '\0';
	return 0;
}

/* SYS_CALL on a bound `/fs` object: run the object's protocol at the
 * outermost call level (the syscall body calls this before the IPC layer),
 * so the driver it forwards to runs on the driver stack. */
int16_t vfs_ns_fs_call(uint8_t handle, uint8_t object,
		       struct kos_ipc_call_args *args)
{
	struct vfs_handle_entry *entry =
		vfs_handle_find(vfs_owner_current(), handle);
	int result;

	if (entry == (struct vfs_handle_entry *)0 ||
	    entry->endpoint != VFS_NS_FS_ENDPOINT) {
		return -KOS_EINVAL;
	}
	if (args->op <= KFILE_OP_SYNC || args->op == KFILE_OP_TRUNCATE
#if KOS_IOC_PHASES
	    /* The measurement build's read-only fetch of the provider's
	     * read-path phase table: forwarded like the other FILE
	     * operations, and refused by the driver (KOS_EINVAL, or
	     * KOS_ENODEV without media) unless the provider itself was
	     * built with the instrument.  A shipped kernel does not
	     * forward it at all, so no shipped image changes. */
	    || args->op == KFILE_OP_PHASES
#endif
	) {
		/* FILE protocol: the driver acts on the bound instance. */
		if (entry->type != KOS_TYPE_FILE || vfs_ns_fs_endpoint == 0u) {
			return -KOS_EINVAL;
		}
		args->frame.payload[0] = object;
		return ipc_syscall_call_object(args, vfs_ns_fs_endpoint,
					       object);
	}
	if (args->op < KFS_OP_MKDIR || args->op > KFS_OP_STAT ||
	    entry->type != KOS_TYPE_FS) {
		return -KOS_EINVAL;
	}
	vfs_ws_enter(VFS_WS_VIEW_PATH);
	result = vfs_fs_read_path(args);
	if (result == 0) {
		uint8_t has_b = 0u;

		if (args->op == KFS_OP_RENAME) {
			/* Split "from\0to" into the two copy-in slots. */
			uint16_t i = 0u;

			while (VFS_WS_IN_A[i] != '\0' && i < args->length) {
				++i;
			}
			if (i >= args->length || i + 1u >= args->length) {
				result = -KOS_EINVAL;
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
				VFS_WS_IN_A, VFS_WS_NORM_A, KOS_PATH_MAX);
		}
		if (result == 0 && has_b != 0u) {
			result = vfs_path_normalize_at(
				vfs_cwd_for(process_current_pid()),
				VFS_WS_IN_B, VFS_WS_NORM_B, KOS_PATH_MAX);
		}
		if (result == 0) {
			/* The frame must cross to the bank-6 driver in fixed
			 * core: `args` lives in the caller's own bank, which
			 * the driver's window cannot see, so the call runs on
			 * a staged copy (the same convention
			 * ipc_syscall_call_object() applies to FILE ops) and
			 * both halves of the reply travel back. */
			struct kos_ipc_frame frame = args->frame;

			result = vfs_fs_namespace_op(
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
	if (type == KOS_TYPE_FILE && object != 0u &&
	    vfs_ns_fs_endpoint != 0u) {
		ns_fs_close(object);
	}
}

/* `ns_enum` below `/fs`: forward to the driver, whose entries land in the
 * caller's buffer through the descriptor copy gate. */
int vfs_enum_fs(const char *rel, uint16_t *cookie, uint16_t out,
		      uint16_t out_size, uint16_t *count)
{
	struct kos_ipc_frame frame;
	int16_t result;

	frame.op = KFS_OP_ENUM;
	frame.flags = 0u;
	frame.desc = KOS_IPC_DESC_NONE;
	frame.arg0 = 0u;
	frame.arg1 = 0u;
	vfs_fs_put_path(frame.payload,
		       (rel[0] == '\0') ? ns_fs_root_path() : rel);
	frame.payload[2] = (uint8_t)*cookie;
	frame.payload[3] = (uint8_t)(*cookie >> 8);
	frame.payload[4] = (uint8_t)out_size;
	frame.payload[5] = (uint8_t)(out_size >> 8);
	result = ipc_nucleus_call_gate(vfs_ns_fs_endpoint, &frame, out,
				       out_size, KOS_IPC_DESC_OUT);
	if (result < 0) {
		return (int)result;
	}
	*cookie = (uint16_t)((uint16_t)frame.payload[2] |
			     ((uint16_t)frame.payload[3] << 8));
	*count = (uint16_t)result;
	return 0;
}
