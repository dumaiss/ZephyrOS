/* ZephyrOS native object and namespace syscall bodies (package 42a).
 *
 * The cold half of the object lifecycle (architecture 36.7, 36.13 and
 * 36.14): SYS_BIND, SYS_UNBIND and SYS_NS_ENUM.  Each body validates its
 * complete argument block, copies the caller path into the shared bank-7
 * VFS workspace path view, normalizes it against the caller's CWD and
 * hands the canonical absolute path to the VFS root router
 * (kernel/vfs/ns.c).  The namespace enumeration writes entries into the
 * caller buffer window through the checked bank copy gate.
 *
 * SYS_CALL rides the same block rules: its body resolves the bound handle
 * to the endpoint and provider object the table records (kernel/vfs/ns.c)
 * and then runs the ordinary IPC path (kernel/ipc/ipc.c
 * ipc_syscall_call_object), so an application reaches a bank-6 service and
 * an in-process FILE provider through one code path.
 *
 * Result conventions (docs/abi.md section 12): a successful number is
 * published in HL with carry clear; every refusal is a carried ZOS_E* in
 * A.  The shared path rules of the M9 filesystem family apply unchanged
 * (1..127 non-NUL caller-window bytes).
 */

#include "zos/ipc.h"
#include "zos/object.h"
#include "zos/syscall.h"
#include "zephyros/errno.h"
#include "zephyros/process.h"

#include "kernel/vfs/vfs.h"
#include "kernel/vfs/ns.h"

#include "diag.h"
#include "fs.h"
#include "object.h"

static uint8_t object_errno_of(int result)
{
	/* -ZOS_E* -> ZOS_E* without an SDCC NEG. */
	return (uint8_t)(uint16_t)(0u - (uint16_t)result);
}

static void object_fail(int result)
{
	zos_syscall_error(object_errno_of(result));
}

/* Little-endian 16-bit field at `offset` in the caller-window block. */
static uint16_t object_le16(uint16_t block, uint8_t offset)
{
	const uint8_t *p = (const uint8_t *)block;

	return (uint16_t)(p[offset] | ((uint16_t)p[offset + 1] << 8));
}

/* Copy the caller path and normalize it against the caller's CWD into the
 * workspace's normalized slot.  Returns 0 or -ZOS_EINVAL. */
static int object_normalize_path(uint16_t addr, uint16_t length)
{
	int result;

	result = zos_fs_copy_path(addr, length, VFS_WS_IN_A);
	if (result != 0) {
		return result;
	}
	return vfs_path_normalize_at(vfs_cwd_for(process_current_pid()),
				     VFS_WS_IN_A, VFS_WS_NORM_A,
				     ZOS_PATH_MAX);
}

static void object_bind(uint16_t block)
{
	struct zos_bind_args *args = (struct zos_bind_args *)block;
	uint8_t type = 0u;
	int result;

	if (!zos_syscall_block_ok(block, sizeof(*args))) {
		zos_syscall_error(ZOS_EINVAL);
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
	zos_syscall_success((uint16_t)result);
}

static void object_unbind(uint16_t block)
{
	struct zos_unbind_args *args = (struct zos_unbind_args *)block;
	int result;

	if (!zos_syscall_block_ok(block, sizeof(*args))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	result = vfs_ns_unbind(process_current_pid(), args->handle);
	if (result < 0) {
		object_fail(result);
		return;
	}
	zos_syscall_success(0);
}

static void object_ns_enum(uint8_t bank, uint16_t block)
{
	struct zos_ns_enum_args *args = (struct zos_ns_enum_args *)block;
	uint16_t cookie;
	uint16_t count = 0u;
	int result;

	if (!zos_syscall_block_ok(block, sizeof(*args)) ||
	    !zos_syscall_buffer_ok(args->out, args->out_size) ||
	    args->out_size < ZOS_NS_ENTRY_SIZE) {
		zos_syscall_error(ZOS_EINVAL);
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
	result = vfs_ns_enum(bank, VFS_WS_NORM_A, &cookie, args->out,
			     args->out_size, &count);
	if (result < 0) {
		object_fail(result);
		return;
	}
	args->cookie = cookie;
	args->count = count;
	zos_syscall_success(count);
}

static void object_call(uint16_t block)
{
	struct zos_ipc_call_args *args = (struct zos_ipc_call_args *)block;
	uint8_t endpoint = 0u;
	uint8_t object = 0u;
	int result;

	if (!zos_syscall_block_ok(block, sizeof(*args)) ||
	    args->frame_length == 0u ||
	    args->frame_length > ZOS_IPC_FRAME_SIZE ||
	    args->reserved != 0u) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	result = vfs_ns_resolve(process_current_pid(), args->handle,
				&endpoint, &object);
	if (result < 0) {
		object_fail(result);
		return;
	}
	zos_syscall_success(
		(uint16_t)ipc_syscall_call_object(args, endpoint, object));
}

void zos_object_dispatch(uint8_t number, uint8_t bank, uint16_t block)
{
	switch (number) {
	case ZOS_SYSCALL_BIND:
		object_bind(block);
		return;
	case ZOS_SYSCALL_CALL:
		object_call(block);
		return;
	case ZOS_SYSCALL_UNBIND:
		object_unbind(block);
		return;
	case ZOS_SYSCALL_NS_ENUM:
		object_ns_enum(bank, block);
		return;
	default:
		zos_syscall_error(ZOS_ENOSYS);
		return;
	}
}
