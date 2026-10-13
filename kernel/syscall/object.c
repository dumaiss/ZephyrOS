/* KonaOS native object and namespace syscall bodies.
 *
 * The cold half of the object lifecycle (architecture 4.9, 4.10 and
 * 6.2): SYS_BIND, SYS_CALL, SYS_UNBIND, SYS_NS_ENUM, SYS_CHDIR and
 * SYS_GETCWD.  Each body validates its complete argument block, copies the
 * caller path into the shared bank-7 VFS workspace path view, normalizes it
 * against the caller's CWD and hands the canonical absolute path to the VFS
 * root router (kernel/vfs/router.c).  The namespace enumeration writes
 * entries
 * into the caller buffer window through the checked bank copy gate.
 *
 * The object family runs at the outermost call level: when the resolved
 * handle names a `/fs` object, the FS provider is entered as plain C
 * (`vfs_ns_fs_call`), so the driver the provider forwards to is dispatched
 * by the IPC layer as an outermost call and runs on the driver stack
 * (architecture 4.2, 4.9).  Every other handle - a device object, the
 * in-process ROMFS FILE provider - takes the ordinary IPC path
 * (kernel/ipc/ipc.c ipc_syscall_call_object).
 *
 * Result conventions (docs/abi.md section 6): a successful number is
 * published in HL with carry clear; every refusal is a carried KOS_E* in
 * A.  The shared path rules are 1..127 non-NUL caller-window bytes for the
 * block's path fields and the 0100h-DFFFh buffer window for `out`.
 */

#include "kos/ipc.h"
#include "kos/object.h"
#include "kos/syscall.h"
#include "kos/errno.h"
#include "process.h"

#include "kernel/vfs/vfs.h"

#include "diag.h"
#include "object.h"

static uint8_t object_errno_of(int result)
{
	/* -KOS_E* -> KOS_E* without an SDCC NEG. */
	return (uint8_t)(uint16_t)(0u - (uint16_t)result);
}

static void object_fail(int result)
{
	kos_syscall_error(object_errno_of(result));
}

/* Copy one caller-window path into `out` (NUL-terminated, at most 127
 * bytes) with the shared validation rules: a length of 0, an over-long
 * path, a window violation or an embedded NUL is KOS_EINVAL.  A path is
 * caller data - a string literal, a stack buffer or a computed name - so
 * it lives in the user buffer window `0100h-DFFFh`, not in the 1 KiB
 * argument-block window the block itself must fit, and it is read
 * through the checked bank-copy gate like any other caller buffer: only
 * the gate maps the calling bank at an arbitrary address in that window.
 * SYS_EXEC uses the same rule, so its path is validated exactly like
 * `bind`'s. */
int kos_object_copy_path(uint16_t addr, uint16_t length, char *out)
{
	uint16_t done = 0u;

	if (length == 0 || length >= KOS_PATH_MAX) {
		return -KOS_EINVAL;
	}
	if (!kos_syscall_buffer_ok(addr, length)) {
		return -KOS_EINVAL;
	}
	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);
		uint16_t i;

		if (chunk > KOS_SYSCALL_STAGE_SIZE) {
			chunk = KOS_SYSCALL_STAGE_SIZE;
		}
		if (kos_syscall_bank_read((uint16_t)(addr + done),
					  chunk) != 0u) {
			return -KOS_EFAULT;
		}
		for (i = 0u; i < chunk; ++i) {
			char c = (char)kos_user_copy_staging[i];

			if (c == '\0') {
				return -KOS_EINVAL;
			}
			out[done + i] = c;
		}
		done = (uint16_t)(done + chunk);
	}
	out[length] = '\0';
	return 0;
}

/* Copy the caller path and normalize it against the caller's CWD into the
 * workspace's normalized slot.  The namespace family shares this gate:
 * `bind` and `exec` both hand their provider a canonical absolute path.
 * Returns 0 or -KOS_EINVAL. */
int object_normalize_path(uint16_t addr, uint16_t length)
{
	int result;

	result = kos_object_copy_path(addr, length, VFS_WS_IN_A);
	if (result != 0) {
		return result;
	}
	return vfs_path_normalize_at(vfs_cwd_for(process_current_pid()),
				     VFS_WS_IN_A, VFS_WS_NORM_A,
				     KOS_PATH_MAX);
}

static void object_bind(uint16_t block)
{
	struct kos_bind_args *args = (struct kos_bind_args *)block;
	uint8_t type = 0u;
	int result;

	if (!kos_syscall_block_ok(block, sizeof(*args))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	vfs_ws_enter(VFS_WS_VIEW_PATH);
	result = object_normalize_path(args->path, args->length);
	if (result == 0) {
		result = vfs_ns_bind(process_current_pid(), VFS_WS_NORM_A,
				     args->flags, &type);
	}
	vfs_ws_leave(VFS_WS_VIEW_PATH);
	if (result < 0) {
		object_fail(result);
		return;
	}
	args->type = type;
	args->reserved = 0u;
	kos_syscall_success((uint16_t)result);
}

static void object_unbind(uint16_t block)
{
	struct kos_unbind_args *args = (struct kos_unbind_args *)block;
	int result;

	if (!kos_syscall_block_ok(block, sizeof(*args))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	result = vfs_ns_unbind(process_current_pid(), args->handle);
	if (result < 0) {
		object_fail(result);
		return;
	}
	kos_syscall_success(0);
}

static void object_ns_enum(uint16_t block)
{
	struct kos_ns_enum_args *args = (struct kos_ns_enum_args *)block;
	uint16_t cookie;
	uint16_t count = 0u;
	int result;

	if (!kos_syscall_block_ok(block, sizeof(*args)) ||
	    !kos_syscall_buffer_ok(args->out, args->out_size) ||
	    args->out_size < KOS_NS_ENTRY_SIZE) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	vfs_ws_enter(VFS_WS_VIEW_PATH);
	result = object_normalize_path(args->path, args->length);
	vfs_ws_leave(VFS_WS_VIEW_PATH);
	if (result != 0) {
		object_fail(result);
		return;
	}
	cookie = args->cookie;
	result = vfs_ns_enum(kos_syscall_buffer_bank(args->out),
			     VFS_WS_NORM_A, &cookie, args->out,
			     args->out_size, &count);
	if (result < 0) {
		object_fail(result);
		return;
	}
	args->cookie = cookie;
	args->count = count;
	kos_syscall_success(count);
}

static void object_call(uint16_t block)
{
	struct kos_ipc_call_args *args = (struct kos_ipc_call_args *)block;
	uint8_t endpoint = 0u;
	uint8_t object = 0u;
	int result;

	if (!kos_syscall_block_ok(block, sizeof(*args)) ||
	    args->frame_length == 0u ||
	    args->frame_length > KOS_IPC_FRAME_SIZE ||
	    args->reserved != 0u) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	result = vfs_ns_resolve(process_current_pid(), args->handle,
				&endpoint, &object);
	if (result < 0) {
		object_fail(result);
		return;
	}
	if (endpoint == VFS_NS_FS_ENDPOINT) {
		kos_syscall_success((uint16_t)vfs_ns_fs_call(
			args->handle, object, args));
		return;
	}
	kos_syscall_success(
		(uint16_t)ipc_syscall_call_object(args, endpoint, object));
}

/* SYS_CHDIR: normalize against the caller's CWD, ask the owning provider
 * whether the path is a traversable location, and only then store the
 * canonical global path (architecture 4.10: chdir does not bind a
 * directory object). */
static void object_chdir(uint16_t block)
{
	struct kos_chdir_args *args = (struct kos_chdir_args *)block;
	uint8_t ok = 0u;
	int result;

	if (!kos_syscall_block_ok(block, sizeof(*args))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	vfs_ws_enter(VFS_WS_VIEW_PATH);
	result = object_normalize_path(args->path, args->length);
	if (result == 0) {
		result = vfs_ns_dir_check(VFS_WS_NORM_A, &ok);
		if (result == 0 && ok == 0u) {
			result = -KOS_ENOTDIR;
		}
	}
	if (result == 0) {
		result = vfs_cwd_store(VFS_WS_NORM_A);
	}
	vfs_ws_leave(VFS_WS_VIEW_PATH);
	if (result < 0) {
		object_fail(result);
		return;
	}
	kos_syscall_success(0);
}

/* SYS_GETCWD: the canonical CWD into the caller's buffer, through the
 * checked bank-write gate.  Returns the length excluding the NUL. */
static void object_getcwd(uint16_t block)
{
	struct kos_getcwd_args *args = (struct kos_getcwd_args *)block;
	const char *cwd;
	uint16_t length = 0u;
	uint16_t done = 0u;

	if (!kos_syscall_block_ok(block, sizeof(*args)) ||
	    !kos_syscall_buffer_ok(args->buffer, args->length)) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	cwd = vfs_cwd_for(process_current_pid());
	while (cwd[length] != '\0') {
		++length;
	}
	if ((uint32_t)length + 1u > (uint32_t)args->length) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	while (done <= length) {
		uint16_t chunk = (uint16_t)(length + 1u - done);
		uint16_t i;

		if (chunk > KOS_SYSCALL_STAGE_SIZE) {
			chunk = KOS_SYSCALL_STAGE_SIZE;
		}
		for (i = 0u; i < chunk; ++i) {
			kos_user_copy_staging[i] =
				(uint8_t)cwd[done + i];
		}
		if (kos_syscall_bank_write((uint16_t)(args->buffer + done),
					   chunk) != 0u) {
			kos_syscall_error(KOS_EFAULT);
			return;
		}
		done = (uint16_t)(done + chunk);
	}
	kos_syscall_success(length);
}

void kos_object_dispatch(uint8_t number, uint16_t block)
{
	switch (number) {
	case KOS_SYSCALL_BIND:
		object_bind(block);
		return;
	case KOS_SYSCALL_CALL:
		object_call(block);
		return;
	case KOS_SYSCALL_UNBIND:
		object_unbind(block);
		return;
	case KOS_SYSCALL_NS_ENUM:
		object_ns_enum(block);
		return;
	case KOS_SYSCALL_CHDIR:
		object_chdir(block);
		return;
	case KOS_SYSCALL_GETCWD:
		object_getcwd(block);
		return;
	default:
		kos_syscall_error(KOS_ENOSYS);
		return;
	}
}
