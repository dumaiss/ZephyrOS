/* ZephyrOS libzos: object and namespace client wrappers.
 *
 * Owns the low-window argument blocks every object syscall marshals
 * through (bind, unbind, ns_enum, chdir, getcwd; the call block is the
 * stage-3 one, unchanged) and the one raw gate stub.  There is no lookup
 * and no cache: a program names the object it wants with its namespace
 * path, and endpoint numbers never reach it.
 */

#include "zos.h"

#include "zos_internal.h"

/* The syscall argument blocks live in the fixed _ZOS_SDK_DATA area
 * (runtime.asm and nsgate.asm).  They are declared here with their ABI
 * types so the C marshalling stays checked, while the explicitly linked
 * objects own every gate-visible byte and place it before executable
 * code. */
extern struct zos_ipc_call_req zos_ipc_raw_req;
extern struct zos_ipc_call_args zos_ipc_call_block;
extern struct zos_bind_args zos_bind_block;
extern struct zos_unbind_args zos_unbind_block;
extern struct zos_ns_enum_args zos_ns_enum_block;
extern struct zos_chdir_args zos_chdir_block;
extern struct zos_getcwd_args zos_getcwd_block;

int zos_ipc_raw(unsigned char number, void *block)
{
	zos_ipc_raw_req.number = number;
	zos_ipc_raw_req.block = block;
	zos_ipc_raw_req.error = 0u;
	return zos_ipc_raw_call(&zos_ipc_raw_req);
}

/* NUL-terminated length in 1..(ZOS_PATH_MAX-1), or 0xFFFF for a
 * null/empty/over-long path (the rule every path wrapper applies). */
static unsigned int src_path_len(const char *path)
{
	unsigned int n = 0;

	if (path == (const char *)0) {
		return 0xFFFFu;
	}
	while (path[n] != '\0') {
		if (n >= (unsigned int)(ZOS_PATH_MAX - 1u)) {
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
		return -ZOS_EINVAL;
	}
	zos_bind_block.path = (unsigned int)path;
	zos_bind_block.length = length;
	zos_bind_block.flags = flags;
	zos_bind_block.type = 0u;
	zos_bind_block.reserved = 0u;
	result = zos_ipc_raw(ZOS_SYSCALL_BIND, &zos_bind_block);
	if (zos_ipc_raw_req.error != 0u) {
		return result;
	}
	if (type != (unsigned char *)0) {
		*type = zos_bind_block.type;
	}
	return result;
}

int unbind(unsigned char handle)
{
	int result;

	zos_unbind_block.handle = handle;
	result = zos_ipc_raw(ZOS_SYSCALL_UNBIND, &zos_unbind_block);
	if (zos_ipc_raw_req.error != 0u) {
		return result;
	}
	return 0;
}

int list(const char *path, unsigned int *cookie,
		struct zos_ns_entry *out, unsigned int max_entries)
{
	unsigned int length = src_path_len(path);
	unsigned int out_size;
	int result;

	if (length == 0xFFFFu || cookie == (unsigned int *)0 ||
	    out == (struct zos_ns_entry *)0 || max_entries == 0u) {
		return -ZOS_EINVAL;
	}
	out_size = max_entries * (unsigned int)sizeof(struct zos_ns_entry);
	zos_ns_enum_block.path = (unsigned int)path;
	zos_ns_enum_block.length = length;
	zos_ns_enum_block.cookie = *cookie;
	zos_ns_enum_block.out = (unsigned int)out;
	zos_ns_enum_block.out_size = out_size;
	zos_ns_enum_block.count = 0u;
	result = zos_ipc_raw(ZOS_SYSCALL_NS_ENUM, &zos_ns_enum_block);
	if (zos_ipc_raw_req.error != 0u) {
		return result;
	}
	*cookie = zos_ns_enum_block.cookie;
	return (int)zos_ns_enum_block.count;
}

int chdir(const char *path)
{
	unsigned int length = src_path_len(path);
	int result;

	if (length == 0xFFFFu) {
		return -ZOS_EINVAL;
	}
	zos_chdir_block.path = (unsigned int)path;
	zos_chdir_block.length = length;
	result = zos_ipc_raw(ZOS_SYSCALL_CHDIR, &zos_chdir_block);
	if (zos_ipc_raw_req.error != 0u) {
		return result;
	}
	return 0;
}

int getcwd(char *buffer, unsigned int length)
{
	int result;

	if (buffer == (char *)0 || length == 0u) {
		return -ZOS_EINVAL;
	}
	zos_getcwd_block.buffer = (unsigned int)buffer;
	zos_getcwd_block.length = (unsigned int)length;
	result = zos_ipc_raw(ZOS_SYSCALL_GETCWD, &zos_getcwd_block);
	if (zos_ipc_raw_req.error != 0u) {
		return result;
	}
	return result;
}

int call(unsigned char handle, struct zos_ipc_request *req)
{
	int result;

	if (req == (struct zos_ipc_request *)0) {
		return -ZOS_EINVAL;
	}
	zos_ipc_call_block.handle = handle;
	zos_ipc_call_block.op = req->frame.op;
	zos_ipc_call_block.flags = req->frame.flags;
	zos_ipc_call_block.frame_length = ZOS_IPC_FRAME_SIZE;
	zos_ipc_call_block.buffer = (unsigned int)req->buffer;
	zos_ipc_call_block.length = req->length;
	zos_ipc_call_block.direction = req->direction;
	zos_ipc_call_block.reserved = 0u;
	zos_ipc_call_block.frame = req->frame;
	zos_ipc_call_block.status = 0;
	result = zos_ipc_raw(ZOS_SYSCALL_CALL, &zos_ipc_call_block);
	if (zos_ipc_raw_req.error != 0u) {
		/* Structural refusal: the frame and status were not touched. */
		return result;
	}
	req->frame = zos_ipc_call_block.frame;
	return (int)zos_ipc_call_block.status;
}
