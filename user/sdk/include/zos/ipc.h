/* ZephyrOS SDK: resident-service IPC wrappers.
 *
 * C-facing wrappers over the pinned IPC syscalls (06h/07h, docs/abi.md
 * section 12) and the SDK-side conventions for them:
 *
 *   - lookup(class, instance) returns the endpoint handle and caches it:
 *     discovery stays a fixed (class, instance) query, and a repeat costs
 *     no syscall;
 *   - call(handle, request) marshals the 32-byte frame and at most one
 *     buffer descriptor through the SDK's low-window argument block.  The
 *     descriptor convention is ABI: the buffer address, its length and
 *     its direction name the caller side; the service only ever sees a
 *     nucleus-issued descriptor id in the frame's `desc` field.
 *
 * Return convention matches zos/console.h: a non-negative result, or the
 * negated native error code.  An IPC call's status is protocol data: a
 * service may return its own negative status, and the wrapper passes it
 * through unchanged.
 *
 * The argument block lives in the program's BSS next to the shared
 * scratch: the ZEX link layout places _DATA near the image end, inside
 * the syscall argument window 0100h-1FFFh, and the kernel rejects a block
 * outside that window with ZOS_EINVAL.
 */

#ifndef ZOS_SDK_IPC_H
#define ZOS_SDK_IPC_H

#include "zos/types.h"
#include "zos/ipc.h"

/* The lookup cache: (class, instance) entries, newest replaces oldest. */
/* Lookup cache depth: the nucleus endpoint table holds at most eight
 * entries (kernel/ipc/ipc.c IPC_ENDPOINT_MAX), so a depth of eight makes
 * a repeated discovery of any live endpoint a cache hit. */
#define ZOS_IPC_CACHE_ENTRIES 8u

/* One request for the shared IPC gate stub (the number, the block
 * address and a carry-error flag the stub fills in; the block always
 * lives in this program). */
struct zos_ipc_call_req {
	unsigned char number;
	void         *block;
	unsigned char error; /* 1 when the syscall returned carry-set (-errno) */
};

/* Low-level gate stub (ipcgate.asm): one RST 30h with the request's
 * number and block.  Carry-set nucleus errors map to -errno. */
int zos_ipc_raw_call(struct zos_ipc_call_req *req);

/* Discovery.  Returns the endpoint handle (>0) or the negated error
 * (ZOS_ENOENT when no endpoint matches).  Results are cached. */
int zos_ipc_lookup(unsigned char class, unsigned char instance);

/* Discovery with the registered version, uncached (the cache holds
 * handles only, so a version query is always one syscall). */
int zos_ipc_lookup_version(unsigned char class, unsigned char instance,
			   unsigned char *version);

/* One call request: the protocol frame plus the optional descriptor.  A
 * zero `length` means no descriptor and `buffer` is ignored; `direction`
 * is ZOS_IPC_DESC_IN or ZOS_IPC_DESC_OUT.  On return `frame` carries the
 * service's reply frame and `frame.status` its result. */
struct zos_ipc_request {
	struct zos_ipc_frame frame;
	void *buffer;
	unsigned int length;
	unsigned char direction;
};

/* Synchronous call/reply.  Returns the IPC status: the service's result
 * (zero or positive success, negative -ZOS_E* failure) or a nucleus
 * refusal (-ZOS_EINVAL bad handle/descriptor, -ZOS_EBUSY re-entrant,
 * -ZOS_ENOSPC too deep). */
int zos_ipc_call(unsigned char handle, struct zos_ipc_request *req);

#endif /* ZOS_SDK_IPC_H */
