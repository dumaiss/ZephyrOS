/* KonaOS libkos: object and namespace client wrappers.
 *
 * Owns the low-window argument blocks every object syscall marshals
 * through (bind, unbind, ns_enum, chdir, getcwd; the call block is the
 * stage-3 one, unchanged) and the one raw gate stub.  There is no lookup
 * and no cache: a program names the object it wants with its namespace
 * path, and endpoint numbers never reach it.
 */

#include "kos.h"

#include "kos_internal.h"

/* The syscall argument blocks live in the fixed _KOS_SDK_DATA area
 * (runtime.asm and nsgate.asm).  They are declared here with their ABI
 * types so the C marshalling stays checked, while the explicitly linked
 * objects own every gate-visible byte and place it before executable
 * code. */
extern struct kos_ipc_call_req kos_ipc_raw_req;
extern struct kos_ipc_call_args kos_ipc_call_block;
extern struct kos_bind_args kos_bind_block;
extern struct kos_unbind_args kos_unbind_block;
extern struct kos_ns_enum_args kos_ns_enum_block;
extern struct kos_chdir_args kos_chdir_block;
extern struct kos_getcwd_args kos_getcwd_block;

int kos_ipc_raw(unsigned char number, void *block)
{
	kos_ipc_raw_req.number = number;
	kos_ipc_raw_req.block = block;
	kos_ipc_raw_req.error = 0u;
	return kos_ipc_raw_call(&kos_ipc_raw_req);
}

/* NUL-terminated length in 1..(KOS_PATH_MAX-1), or 0xFFFF for a
 * null/empty/over-long path (the rule every path wrapper applies). */
static unsigned int src_path_len(const char *path)
{
	unsigned int n = 0;

	if (path == (const char *)0) {
		return 0xFFFFu;
	}
	while (path[n] != '\0') {
		if (n >= (unsigned int)(KOS_PATH_MAX - 1u)) {
			return 0xFFFFu;
		}
		++n;
	}
	if (n == 0u) {
		return 0xFFFFu;
	}
	return n;
}

int bind(const char *path, unsigned int flags, unsigned char *type)
{
	unsigned int length = src_path_len(path);
	int result;

	if (length == 0xFFFFu) {
		return -KOS_EINVAL;
	}
	kos_bind_block.path = (unsigned int)path;
	kos_bind_block.length = length;
	kos_bind_block.flags = flags;
	kos_bind_block.type = 0u;
	kos_bind_block.reserved = 0u;
	result = kos_ipc_raw(KOS_SYSCALL_BIND, &kos_bind_block);
	if (kos_ipc_raw_req.error != 0u) {
		return result;
	}
	if (type != (unsigned char *)0) {
		*type = kos_bind_block.type;
	}
	return result;
}

int unbind(unsigned char handle)
{
	int result;

	kos_unbind_block.handle = handle;
	result = kos_ipc_raw(KOS_SYSCALL_UNBIND, &kos_unbind_block);
	if (kos_ipc_raw_req.error != 0u) {
		return result;
	}
	return 0;
}

int list(const char *path, unsigned int *cookie,
		struct kos_ns_entry *out, unsigned int max_entries)
{
	unsigned int length = src_path_len(path);
	unsigned int out_size;
	int result;

	if (length == 0xFFFFu || cookie == (unsigned int *)0 ||
	    out == (struct kos_ns_entry *)0 || max_entries == 0u) {
		return -KOS_EINVAL;
	}
	out_size = max_entries * (unsigned int)sizeof(struct kos_ns_entry);
	kos_ns_enum_block.path = (unsigned int)path;
	kos_ns_enum_block.length = length;
	kos_ns_enum_block.cookie = *cookie;
	kos_ns_enum_block.out = (unsigned int)out;
	kos_ns_enum_block.out_size = out_size;
	kos_ns_enum_block.count = 0u;
	result = kos_ipc_raw(KOS_SYSCALL_NS_ENUM, &kos_ns_enum_block);
	if (kos_ipc_raw_req.error != 0u) {
		return result;
	}
	*cookie = kos_ns_enum_block.cookie;
	return (int)kos_ns_enum_block.count;
}

int chdir(const char *path)
{
	unsigned int length = src_path_len(path);
	int result;

	if (length == 0xFFFFu) {
		return -KOS_EINVAL;
	}
	kos_chdir_block.path = (unsigned int)path;
	kos_chdir_block.length = length;
	result = kos_ipc_raw(KOS_SYSCALL_CHDIR, &kos_chdir_block);
	if (kos_ipc_raw_req.error != 0u) {
		return result;
	}
	return 0;
}

int getcwd(char *buffer, unsigned int length)
{
	int result;

	if (buffer == (char *)0 || length == 0u) {
		return -KOS_EINVAL;
	}
	kos_getcwd_block.buffer = (unsigned int)buffer;
	kos_getcwd_block.length = (unsigned int)length;
	result = kos_ipc_raw(KOS_SYSCALL_GETCWD, &kos_getcwd_block);
	if (kos_ipc_raw_req.error != 0u) {
		return result;
	}
	return result;
}

int call(unsigned char handle, struct kos_ipc_request *req)
{
	int result;

	if (req == (struct kos_ipc_request *)0) {
		return -KOS_EINVAL;
	}
	kos_ipc_call_block.handle = handle;
	kos_ipc_call_block.op = req->frame.op;
	kos_ipc_call_block.flags = req->frame.flags;
	kos_ipc_call_block.frame_length = KOS_IPC_FRAME_SIZE;
	kos_ipc_call_block.buffer = (unsigned int)req->buffer;
	kos_ipc_call_block.length = req->length;
	kos_ipc_call_block.direction = req->direction;
	kos_ipc_call_block.reserved = 0u;
	kos_ipc_call_block.frame = req->frame;
	kos_ipc_call_block.status = 0;
	result = kos_ipc_raw(KOS_SYSCALL_CALL, &kos_ipc_call_block);
	if (kos_ipc_raw_req.error != 0u) {
		/* Structural refusal: the frame and status were not touched. */
		return result;
	}
	req->frame = kos_ipc_call_block.frame;
	return (int)kos_ipc_call_block.status;
}
