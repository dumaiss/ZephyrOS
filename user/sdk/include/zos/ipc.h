/* ZephyrOS SDK: resident-service IPC wrappers.
 *
 * C-facing wrappers over the pinned object syscalls (06h-0Bh, docs/abi.md
 * section 12) and the SDK-side conventions for them:
 *
 *   - bind(path, flags) resolves a namespace name and returns a handle plus
 *     the 8-bit protocol type the kernel resolved (there is no expected-type
 *     argument and no lookup: endpoint numbers stay nucleus-internal);
 *   - call(handle, request) marshals the 32-byte frame and at most one
 *     buffer descriptor through the SDK's low-window argument block.  The
 *     descriptor convention is ABI: the buffer address, its length and
 *     its direction name the caller side; the service only ever sees a
 *     nucleus-issued descriptor id in the frame's `desc` field;
 *   - unbind(handle) releases the instance;
 *   - ns_enum pages a namespace directory;
 *   - chdir/getcwd move and read the process CWD.
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
#include "zos/object.h"
/* The frame and descriptor definitions live in the native ABI header of
 * the same name (abi/zos/ipc.h): this SDK header only adds the client
 * wrappers, so it includes it. */
#include "zos/ipc.h"

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

/* --- object and namespace wrappers -------------------------------------
 *
 * The handle `zos_ipc_call` names is a *process-owned object handle*
 * produced by
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

/* Move the process CWD to `path` (validated by the owning provider) or
 * copy the canonical CWD into `buffer` (`length` bytes including the NUL;
 * the length excluding the NUL is returned, or -ZOS_EINVAL). */
int zos_chdir(const char *path);
int zos_getcwd(char *buffer, unsigned int length);

/* Namespace enumeration: write up to `max_entries` `struct zos_ns_entry`
 * records from `cookie` into `out` and return the number written (0 at the
 * end of the stream) with `*cookie` advanced, or the negated error.  A
 * caller pages by passing the returned cookie back until the count is 0. */
int zos_ns_enum(const char *path, unsigned int *cookie,
		struct zos_ns_entry *out, unsigned int max_entries);

#endif /* ZOS_SDK_IPC_H */
