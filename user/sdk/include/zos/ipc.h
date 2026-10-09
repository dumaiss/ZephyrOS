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
 * The argument blocks live in the SDK's fixed low-data area, linked before
 * executable code inside 0100h-1FFFh.  Ordinary program BSS may extend
 * above that window; the kernel still rejects an argument block outside
 * it with ZOS_EINVAL.
 */

#ifndef ZOS_SDK_IPC_H
#define ZOS_SDK_IPC_H

#include "zos/types.h"
#include "zos/ipc.h"
#include "zos/object.h"

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
 * (ZOS_ENOENT when no endpoint matches).  Results are cached.
 *
 * The lookup pair is the stage-3 half of this header: the build that
 * defines ZOS_NS_HANDLES (the namespace acceptance image, work package
 * 42a) has no lookup at all, so `ipc.c` compiles both wrappers and the
 * cache out and the programs of that image name object handles from
 * `zos_ipc_bind` instead. */
#if !ZOS_NS_HANDLES
int zos_ipc_lookup(unsigned char class, unsigned char instance);

/* Discovery with the registered version, uncached (the cache holds
 * handles only, so a version query is always one syscall). */
int zos_ipc_lookup_version(unsigned char class, unsigned char instance,
			   unsigned char *version);
#endif /* !ZOS_NS_HANDLES */

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

#if ZOS_NS_HANDLES
/* --- object and namespace wrappers (work package 42a) ------------------
 *
 * In the namespace model (architecture 36.13, 36.14) the handle
 * `zos_ipc_call` names is a *process-owned object handle* produced by
 * `zos_ipc_bind`, never an endpoint number: endpoint numbers stay
 * nucleus-internal, and one handle names one bound instance of one
 * provider.  The two standard console handles (ZOS_HANDLE_STDOUT,
 * ZOS_HANDLE_STDIN) are bound at process start, so a program that only
 * talks to the console never calls `zos_ipc_bind`.
 */

/* Resolve `path` and bind an instance.  Returns the handle (>= 1) with
 * `*type` set to the 8-bit protocol type the kernel resolved, or the
 * negated error (ZOS_ENOENT unknown name, ZOS_ENODEV a provider this build
 * does not serve, ZOS_EISDIR a grouping entry, ZOS_EMFILE an exhausted
 * handle table, ZOS_EINVAL a malformed path).  There is no expected-type
 * argument: a client that cares checks `*type`. */
int zos_ipc_bind(const char *path, unsigned int flags, unsigned char *type);

/* Release the instance `handle` names.  Returns 0 or -ZOS_EINVAL; the
 * kernel unbinds every handle a process still owns at exit, so this is
 * only about releasing early. */
int zos_ipc_unbind(unsigned char handle);

/* Namespace enumeration: write up to `max_entries` `struct zos_ns_entry`
 * records from `cookie` into `out` and return the number written (0 at the
 * end of the stream) with `*cookie` advanced, or the negated error.  A
 * caller pages by passing the returned cookie back until the count is 0. */
int zos_ns_enum(const char *path, unsigned int *cookie,
		struct zos_ns_entry *out, unsigned int max_entries);

#endif /* ZOS_NS_HANDLES */

#endif /* ZOS_SDK_IPC_H */
