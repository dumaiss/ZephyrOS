/* KonaOS SDK: resident-driver IPC wrappers.
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
 *     its direction name the caller side; the driver only ever sees a
 *     nucleus-issued descriptor id in the frame's `desc` field;
 *   - unbind(handle) releases the instance;
 *   - ns_enum pages a namespace directory;
 *   - chdir/getcwd move and read the process CWD.
 *
 * Return convention matches kos/console.h: a non-negative result, or the
 * negated native error code.  An IPC call's status is protocol data: a
 * driver may return its own negative status, and the wrapper passes it
 * through unchanged.
 *
 * The argument blocks live in the SDK's fixed low-data area, linked before
 * executable code inside 0100h-1FFFh.  Ordinary program BSS may extend
 * above that window; the kernel still rejects an argument block outside
 * it with KOS_EINVAL.
 */

#ifndef KOS_SDK_IPC_H
#define KOS_SDK_IPC_H

#include "kos/types.h"
#include "kos/object.h"
/* The frame and descriptor definitions live in the native ABI header of
 * the same name (abi/kos/ipc.h): this SDK header only adds the client
 * wrappers, so it includes it. */
#include "kos/ipc.h"

/* One request for the shared IPC gate stub (the number, the block
 * address and a carry-error flag the stub fills in; the block always
 * lives in this program). */
struct kos_ipc_call_req {
	unsigned char number;
	void         *block;
	unsigned char error; /* 1 when the syscall returned carry-set (-errno) */
};


/* One call request: the protocol frame plus the optional descriptor.  A
 * zero `length` means no descriptor and `buffer` is ignored; `direction`
 * is KOS_IPC_DESC_IN or KOS_IPC_DESC_OUT.  On return `frame` carries the
 * driver's reply frame and `frame.status` its result. */
struct kos_ipc_request {
	struct kos_ipc_frame frame;
	void *buffer;
	unsigned int length;
	unsigned char direction;
};

/* Synchronous call/reply.  Returns the IPC status: the driver's result
 * (zero or positive success, negative -KOS_E* failure) or a nucleus
 * refusal (-KOS_EINVAL bad handle/descriptor, -KOS_EBUSY re-entrant,
 * -KOS_ENOSPC too deep). */
int call(unsigned char handle, struct kos_ipc_request *req);

/* --- object and namespace wrappers -------------------------------------
 *
 * The handle `call` names is a *process-owned object handle*
 * produced by
 * `bind`, never an endpoint number: endpoint numbers stay
 * nucleus-internal, and one handle names one bound instance of one
 * provider.  The two standard console handles (KOS_HANDLE_STDOUT,
 * KOS_HANDLE_STDIN) are bound at process start, so a program that only
 * talks to the console never calls `bind`.
 */

/* Resolve `path` and bind an instance.  Returns the handle (>= 1) with
 * `*type` set to the 8-bit protocol type the kernel resolved, or the
 * negated error (KOS_ENOENT unknown name, KOS_ENODEV a provider this build
 * does not serve, KOS_EISDIR a grouping entry, KOS_EMFILE an exhausted
 * handle table, KOS_EINVAL a malformed path).  There is no expected-type
 * argument: a client that cares checks `*type`. */
int bind(const char *path, unsigned int flags, unsigned char *type);

/* Release the instance `handle` names.  Returns 0 or -KOS_EINVAL; the
 * kernel unbinds every handle a process still owns at exit, so this is
 * only about releasing early. */
int unbind(unsigned char handle);

/* Move the process CWD to `path` (validated by the owning provider) or
 * copy the canonical CWD into `buffer` (`length` bytes including the NUL;
 * the length excluding the NUL is returned, or -KOS_EINVAL). */
int chdir(const char *path);
int getcwd(char *buffer, unsigned int length);

/* Namespace enumeration: write up to `max_entries` `struct kos_ns_entry`
 * records from `cookie` into `out` and return the number written (0 at the
 * end of the stream) with `*cookie` advanced, or the negated error.  A
 * caller pages by passing the returned cookie back until the count is 0. */
int list(const char *path, unsigned int *cookie,
		struct kos_ns_entry *out, unsigned int max_entries);

#endif /* KOS_SDK_IPC_H */
