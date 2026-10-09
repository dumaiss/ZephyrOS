/* ZephyrOS libzos: IPC client wrappers (work package 39; the object and
 * namespace wrappers of work package 42a follow the guard below).
 *
 * Owns the SDK-side lookup cache and the low-window argument block every
 * IPC syscall marshals through.  The gate stub (ipcgate.asm) converts
 * the private SDCC convention to the native register ABI, records a
 * carry-set nucleus error so a structural refusal can be told apart from
 * an IPC status, and maps that error to -errno.
 *
 * In the namespace image (ZOS_NS_HANDLES) the lookup pair and its cache are
 * replaced by bind/unbind/ns_enum and the call names a bound object handle;
 * the stage-3 half stays for production until package 42b switches over.
 */

#include "zos.h"

/* The syscall argument blocks live in runtime.asm's fixed
 * _ZOS_SDK_DATA area.  They are declared here with their ABI types so the
 * C marshalling stays checked, while one explicitly linked object owns
 * every gate-visible byte and places it before executable code. */
extern struct zos_ipc_call_req zos_ipc_raw_req;
extern struct zos_ipc_call_args zos_ipc_call_block;
#if ZOS_NS_HANDLES
/* Owned by nsgate.asm (the object/namespace blocks) instead. */
extern struct zos_bind_args zos_bind_block;
extern struct zos_unbind_args zos_unbind_block;
extern struct zos_ns_enum_args zos_ns_enum_block;
#else
extern struct zos_ipc_lookup_args zos_ipc_lookup_block;
#endif

#if !ZOS_NS_HANDLES
/* The lookup cache: a small rolling table so repeated discovery of the
 * same (class, instance) pair costs no syscall.  It is a fast path, never
 * a correctness dependency: the kernel owns the endpoint table, and
 * handles stay valid for the whole boot (services are permanent). */
struct zos_ipc_cache_entry {
	unsigned char class;
	unsigned char instance;
	unsigned char handle;
};

static struct zos_ipc_cache_entry ipc_cache[ZOS_IPC_CACHE_ENTRIES];
static unsigned char ipc_cache_next;
#endif

static int raw(unsigned char number, void *block)
{
	zos_ipc_raw_req.number = number;
	zos_ipc_raw_req.block = block;
	zos_ipc_raw_req.error = 0u;
	return zos_ipc_raw_call(&zos_ipc_raw_req);
}

#if !ZOS_NS_HANDLES
int zos_ipc_lookup(unsigned char class, unsigned char instance)
{
	unsigned char index;
	int result;

	for (index = 0u; index < ZOS_IPC_CACHE_ENTRIES; ++index) {
		if (ipc_cache[index].class == class &&
		    ipc_cache[index].instance == instance) {
			return (int)ipc_cache[index].handle;
		}
	}
	result = zos_ipc_lookup_version(class, instance, (unsigned char *)0);
	if (result <= 0) {
		return result;
	}
	ipc_cache[ipc_cache_next].class = class;
	ipc_cache[ipc_cache_next].instance = instance;
	ipc_cache[ipc_cache_next].handle = (unsigned char)result;
	ipc_cache_next = (unsigned char)((ipc_cache_next + 1u) %
					 ZOS_IPC_CACHE_ENTRIES);
	return result;
}

int zos_ipc_lookup_version(unsigned char class, unsigned char instance,
			   unsigned char *version)
{
	int result;

	zos_ipc_lookup_block.class = class;
	zos_ipc_lookup_block.instance = instance;
	zos_ipc_lookup_block.version = 0u;
	zos_ipc_lookup_block.reserved = 0u;
	result = raw(ZOS_SYSCALL_IPC_LOOKUP, &zos_ipc_lookup_block);
	if (zos_ipc_raw_req.error != 0u) {
		return result;
	}
	if (result == 0) {
		return -ZOS_ENOENT;
	}
	if (version != (unsigned char *)0) {
		*version = zos_ipc_lookup_block.version;
	}
	return result;
}
#endif /* !ZOS_NS_HANDLES */

#if ZOS_NS_HANDLES
/* --- object and namespace wrappers (work package 42a) ------------------ */

/* NUL-terminated length in 1..(ZOS_PATH_MAX-1), or 0xFFFF for a
 * null/empty/over-long path (the same rule the FS wrappers apply). */
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

int zos_ipc_bind(const char *path, unsigned int flags, unsigned char *type)
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
	result = raw(ZOS_SYSCALL_BIND, &zos_bind_block);
	if (zos_ipc_raw_req.error != 0u) {
		return result;
	}
	if (type != (unsigned char *)0) {
		*type = zos_bind_block.type;
	}
	return result;
}

int zos_ipc_unbind(unsigned char handle)
{
	int result;

	zos_unbind_block.handle = handle;
	result = raw(ZOS_SYSCALL_UNBIND, &zos_unbind_block);
	if (zos_ipc_raw_req.error != 0u) {
		return result;
	}
	return 0;
}

int zos_ns_enum(const char *path, unsigned int *cookie,
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
	result = raw(ZOS_SYSCALL_NS_ENUM, &zos_ns_enum_block);
	if (zos_ipc_raw_req.error != 0u) {
		return result;
	}
	*cookie = zos_ns_enum_block.cookie;
	return (int)zos_ns_enum_block.count;
}
#endif /* ZOS_NS_HANDLES */

int zos_ipc_call(unsigned char handle, struct zos_ipc_request *req)
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
	result = raw(ZOS_SYSCALL_IPC_CALL, &zos_ipc_call_block);
	if (zos_ipc_raw_req.error != 0u) {
		/* Structural refusal: the frame and status were not touched. */
		return result;
	}
	req->frame = zos_ipc_call_block.frame;
	return (int)zos_ipc_call_block.status;
}
