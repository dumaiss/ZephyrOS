/* ZephyrOS M10 VFS root router and static namespace providers (kernel-private).
 *
 * Package 42a (architecture 36.7 and 36.13): the VFS owns the global root
 * `/` and routes a normalized global path, ASCII case-insensitively, by
 * its first component to exactly three static providers:
 *
 *   /FS       -> the FS provider (nucleus routing to the build's
 *                filesystem driver; package 42b - ENODEV until then)
 *   /SYSTEM   -> the ROMFS provider over kernel/romfs (available before
 *                any resident driver)
 *   /dev      -> the DEVICE provider over the host-built publication
 *                table the resident loader installs
 *
 * There is no mount table, no provider registration and no root merge on
 * this path; the M9 mount/provider machinery it replaces is retired in
 * package 42c.  A provider owns everything below its root.
 *
 * The handle side lives in kernel/vfs/process.c (the extended M9 pool);
 * this module resolves names to objects and fills or releases the pool
 * entries.  `vfs_ns_file_dispatch()` is the in-process FILE endpoint the
 * ROMFS provider binds; the IPC substrate installs it as a nucleus
 * endpoint, so a call reaches it through the same handle-call path as a
 * bank-6 endpoint (architecture 36.13).
 */

#ifndef ZEPHYROS_KERNEL_VFS_NS_H
#define ZEPHYROS_KERNEL_VFS_NS_H

#include "zos/object.h"
#include "zephyros/ipc.h"

#include "kernel/ipc/ipc.h"
#include "kernel/vfs/vfs.h"

/* Publication table capacity: the production build declares five roles
 * (console, input/keyboard, video, serial/0, ioc), which expand to seven
 * rows with the grouping entries; an acceptance set adds its synthetic
 * names.  The loader refuses a set that overflows the table. */
#define VFS_NS_PUB_MAX 12u

/* Publication name field: "/input/keyboard" is the longest role above, so
 * the table stores the flat provider-relative names in 16 bytes. */
#define VFS_NS_NAME_MAX 16u

/* The in-process FILE endpoint's number: nucleus endpoints live above the
 * host-assigned driver numbers (1..N), which the packer bounds by the
 * nucleus table size.  Endpoint numbers stay nucleus-internal. */
#define VFS_NS_FILE_ENDPOINT IPC_EP_NUCLEUS_BASE

struct vfs_ns_publication {
	char    name[VFS_NS_NAME_MAX]; /* provider-relative, e.g. "/console" */
	uint8_t type;                  /* ZOS_TYPE_*, 0 on a container       */
	uint8_t number;                /* endpoint number, 0 on a container  */
	uint8_t object;                /* provider object identity           */
	uint8_t flags;                 /* ZOS_NS_ENTRY_CONTAINER on a grouping row */
};

/* One-time nucleus setup (the in-process FILE endpoint) plus the empty
 * start for the provider tables; called from vfs_init().  Returns 0 or a
 * negative ZOS_E* value. */
int vfs_ns_init(void);

/* Discard every host-built publication (vfs_init(), and the acceptance
 * replays that re-initialize the nucleus). */
void vfs_ns_reset(void);

/* Install one host-built `/dev` publication (the resident loader, before
 * any driver code runs).  The loader already checked the record against
 * the container; the checks here protect the nucleus table: capacity, a
 * bounded printable name beginning with '/', a legal protocol type and a
 * nonzero endpoint.  Returns 0 or -ZOS_EINVAL/-ZOS_ENOSPC. */
int vfs_ns_install_publication(const char *name, uint8_t type, uint8_t number,
			       uint8_t object);

/* Resolve the normalized absolute path `norm` and bind an instance for
 * `pid`, storing the 8-bit protocol type in *type and returning the handle
 * number (>= 1) or a negative ZOS_E* value.  `flags` is the caller's
 * 16-bit bind flags, interpreted by the selected provider. */
int vfs_ns_bind(uint8_t pid, const char *norm, uint16_t flags, uint8_t *type);

/* Release the instance the handle names for `pid`.  Returns 0 or
 * -ZOS_EINVAL. */
int vfs_ns_unbind(uint8_t pid, uint8_t handle);

/* The handle's endpoint and object identity for SYS_CALL, or a negative
 * ZOS_E* value when the handle is not a bound object. */
int vfs_ns_resolve(uint8_t pid, uint8_t handle, uint8_t *endpoint,
		   uint8_t *object);

/* Bind the process `pid`'s two standard handles to `/dev/console` (absent
 * when the build publishes none).  Called once at process start. */
void vfs_ns_standard_handles(uint8_t pid);

/* Namespace enumeration for SYS_NS_ENUM: write as many `struct
 * zos_ns_entry` records as fit `out_size` bytes to the caller buffer
 * (bank `bank`, address `out`) starting at `*cookie`, returning the count
 * written and updating `*cookie`, or a negative ZOS_E* value. */
int vfs_ns_enum(uint8_t bank, const char *norm, uint16_t *cookie,
		uint16_t out, uint16_t out_size, uint16_t *count);

/* --- the ROMFS FILE provider (kernel/vfs/ns_file.c) ------------------- */

/* Bind the ROMFS entry `name` (provider-relative, e.g. "/MONITOR.ZEX") as
 * a FILE instance for `pid`.  Returns the handle or a negative ZOS_E*
 * value; `flags` carries the ZFS_O_* open flags. */
int vfs_ns_file_bind(uint8_t pid, const char *name, uint16_t flags,
		     uint8_t *type);

/* The in-process FILE endpoint's dispatch (installed by vfs_ns_init());
 * the nucleus calls it through the ordinary handle-call path.  `handle`
 * is the caller's handle number, `object` the publication identity (unused
 * by ROMFS: the instance is the handle entry). */
int16_t vfs_ns_file_dispatch(uint8_t handle, uint8_t object,
			     struct zos_ipc_frame *frame);

#endif /* ZEPHYROS_KERNEL_VFS_NS_H */
