/* ZephyrOS libzos: IPC client wrappers (work package 39).
 *
 * Owns the SDK-side lookup cache and the low-window argument block every
 * IPC syscall marshals through.  The gate stub (ipcgate.asm) converts
 * the private SDCC convention to the native register ABI, records a
 * carry-set nucleus error so a structural refusal can be told apart from
 * an IPC status, and maps that error to -errno.
 */

#include "zos.h"

/* The syscall argument blocks live in runtime.asm's fixed
 * _ZOS_SDK_DATA area.  They are declared here with their ABI types so the
 * C marshalling stays checked, while one explicitly linked object owns
 * every gate-visible byte and places it before executable code. */
extern struct zos_ipc_call_req zos_ipc_raw_req;
extern struct zos_ipc_lookup_args zos_ipc_lookup_block;
extern struct zos_ipc_call_args zos_ipc_call_block;

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

static int raw(unsigned char number, void *block)
{
	zos_ipc_raw_req.number = number;
	zos_ipc_raw_req.block = block;
	zos_ipc_raw_req.error = 0u;
	return zos_ipc_raw_call(&zos_ipc_raw_req);
}

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
