/* ZephyrOS native object and namespace ABI (public native ABI, package 42a).
 *
 * The public object lifecycle of architecture 36.7 and 36.13:
 *
 *   bind(path, flags) -> handle, type   resolve a namespace name, bind an instance
 *   call(handle, op, frame)             synchronous protocol operation
 *   unbind(handle)                      release the bound instance
 *
 * A bound handle carries one 8-bit **protocol type** (the numbers below);
 * there is no expected-type argument, so `bind` reports the type it
 * actually resolved and a caller that cares checks it.  Handles are
 * process-owned, opaque and small: they are neither addresses, bank
 * numbers nor endpoint numbers (endpoint numbers stay nucleus-internal).
 * One global owner-tagged handle table serves every process; the M9 FS_*
 * descriptors share it until package 42b.
 *
 * Standard handles: every process starts with console input and output
 * already bound to `/dev/console` at the fixed numbers below, inherited
 * across SYS_EXEC; `bind` never returns either number, and the standard
 * numbers are absent when the build selected no console driver.
 *
 * Namespace enumeration (architecture 36.13) is a batched VFS operation
 * routed to the owning namespace provider.  `ns_enum` writes as many
 * `struct zos_ns_entry` records as fit the caller's buffer and returns the
 * next cookie, so a caller pages with `cookie` until zero entries arrive.
 * An entry's `container` flag is namespace metadata (further path
 * components may be traversed below the entry), not an object type.
 *
 * Argument-block placement: blocks live in the caller window,
 * 0100h-1FFFh, exactly like every other native syscall family; the path
 * fields are `{pointer, length}` pairs of 1..127 non-NUL caller-window
 * bytes resolved against the caller's CWD, and `out` is a caller-bank
 * address in the 0100h-DFFFh buffer window, written only through the
 * checked bank-copy gate.
 *
 * Native syscalls (docs/abi.md section 12):
 *
 *   RST 30h, A = ZOS_SYSCALL_BIND, HL = &struct zos_bind_args
 *       carry clear: HL = handle (>= 1), args->type filled with the 8-bit
 *       protocol type; carry set: A = ZOS_E* (ZOS_ENOENT unknown name,
 *       ZOS_ENODEV a provider this build does not serve, ZOS_EISDIR a
 *       container, ZOS_EINVAL a bad block/path/flags, ZOS_EMFILE/ENOSPC
 *       an exhausted handle table).
 *
 *   RST 30h, A = ZOS_SYSCALL_UNBIND, HL = &struct zos_unbind_args
 *       carry clear: HL = 0 and the instance was released; carry set:
 *       A = ZOS_EINVAL (bad handle, not this process' handle, or a
 *       descriptor kind that is not a bound object).
 *
 *   RST 30h, A = ZOS_SYSCALL_NS_ENUM, HL = &struct zos_ns_enum_args
 *       carry clear: HL = the number of entries written (0 at end of
 *       stream) and args->cookie the next cookie; carry set: A = ZOS_E*
 *       (ZOS_ENOENT bad path, ZOS_ENODEV, ZOS_ENOTDIR an entry below a
 *       non-container, ZOS_EINVAL a bad block).
 */

#ifndef ZOS_OBJECT_H
#define ZOS_OBJECT_H

#include "zos/types.h"

/* Build selection (work package 42a, correction 01).  The root router, the
 * DEVICE and ROMFS providers, the typed handle table, bind/call/unbind and
 * ns_enum link in the `namespace-selftest` acceptance image; production
 * adopts them in package 42b, when the M9 `FS_*` front end, the M9 VFS
 * machinery and the IOC-FAT arena leave bank 7.  Production builds do not
 * define the macro, and every source that carries both behaviours guards
 * the new half with `#if ZOS_NS_HANDLES`. */
#ifndef ZOS_NS_HANDLES
#define ZOS_NS_HANDLES 0
#endif

/* --- native syscall numbers (docs/abi.md section 12) -------------------- */

/* 06h and 07h are the numbers the stage-3 IPC ABI pinned
 * (abi/zos/ipc.h ZOS_SYSCALL_IPC_LOOKUP / ZOS_SYSCALL_IPC_CALL).  From 42b
 * 06h is `bind` and 07h is `call` on an object handle, and the lookup
 * syscall disappears with the SDK lookup cache; until then production
 * keeps the stage-3 pair and the acceptance image builds this family. */
#define ZOS_SYSCALL_BIND    0x06u
#define ZOS_SYSCALL_CALL    0x07u
#define ZOS_SYSCALL_UNBIND  0x08u
#define ZOS_SYSCALL_NS_ENUM 0x09u

/* --- 8-bit protocol types ------------------------------------------------ */

/* Device protocols keep the pinned M8 class slots (include/zephyros/device.h
 * ZDEV_*), so a protocol type and a service class coincide for the device
 * families; FILE and the filesystem-namespace protocol are not device
 * classes and continue the sequence. */
#define ZOS_TYPE_SERIAL  1u
#define ZOS_TYPE_VIDEO   2u
#define ZOS_TYPE_BLOCK   3u
#define ZOS_TYPE_INPUT   4u
#define ZOS_TYPE_IOC     5u
#define ZOS_TYPE_CONSOLE 6u
#define ZOS_TYPE_FILE    7u
#define ZOS_TYPE_FS      8u
#define ZOS_TYPE_SOUND   9u
#define ZOS_TYPE_TIMER   10u

/* --- standard handle numbers -------------------------------------------- */

/* Console output (stdout) and input (stdin): bound to `/dev/console` at
 * process start when the selected build publishes one, and inherited by a
 * child process.  `bind` never returns either number. */
#define ZOS_HANDLE_STDOUT 1u
#define ZOS_HANDLE_STDIN  2u

/* The first number `bind` hands out; handles 1..2 are the standard ones. */
#define ZOS_HANDLE_FIRST_BOUND 3u

/* --- namespace enumeration ---------------------------------------------- */

/* Entry name field: 24 bytes carries any ROMFS name (ROMFS_NAME_SIZE,
 * include/zephyros/romfs.h) and every `/dev` role name.  `name_length`
 * counts the terminating NUL. */
#define ZOS_NS_NAME_MAX   24u
#define ZOS_NS_ENTRY_SIZE 28u

#define ZOS_NS_ENTRY_CONTAINER 0x01u /* further components may follow */

struct zos_ns_entry {
	uint8_t type;        /* protocol type, 0 when not bindable    */
	uint8_t flags;       /* ZOS_NS_ENTRY_*                        */
	uint8_t name_length; /* bytes incl. the NUL, 1..ZOS_NS_NAME_MAX */
	uint8_t reserved;    /* 0                                     */
	char    name[ZOS_NS_NAME_MAX]; /* NUL-terminated, zero-padded  */
};

/* --- argument blocks ----------------------------------------------------- */

/* 06h BIND {path, length, flags, type, reserved}: `type` and `reserved` are
 * outputs (reserved stays 0) and the handle is returned in HL. */
struct zos_bind_args {
	uint16_t path;
	uint16_t length;
	uint16_t flags;    /* provider-interpreted; ZFS_O_* for FILE binds */
	uint8_t  type;     /* out: the bound object's protocol type       */
	uint8_t  reserved; /* out: 0                                      */
};

/* 08h UNBIND {handle}. */
struct zos_unbind_args {
	uint8_t handle;
};

/* 09h NS_ENUM {path, length, cookie, out, out_size, count}: `cookie` is
 * read as the start index and written back as the next cookie; `count` is
 * the output count (also returned in HL). */
struct zos_ns_enum_args {
	uint16_t path;
	uint16_t length;
	uint16_t cookie;
	uint16_t out;      /* struct zos_ns_entry destination array */
	uint16_t out_size; /* bytes available at `out`              */
	uint16_t count;    /* out: entries written                  */
};

#if defined(__SDCC)
_Static_assert(sizeof(struct zos_ns_entry) == ZOS_NS_ENTRY_SIZE,
	       "namespace entry must be 28 packed bytes");
_Static_assert(__builtin_offsetof(struct zos_ns_entry, name) == 4,
	       "namespace entry name offset");
_Static_assert(sizeof(struct zos_bind_args) == 8,
	       "bind argument block must be 8 packed bytes");
_Static_assert(__builtin_offsetof(struct zos_bind_args, type) == 6,
	       "bind type offset");
_Static_assert(sizeof(struct zos_unbind_args) == 1,
	       "unbind argument block must be 1 packed byte");
_Static_assert(sizeof(struct zos_ns_enum_args) == 12,
	       "ns_enum argument block must be 12 packed bytes");
_Static_assert(__builtin_offsetof(struct zos_ns_enum_args, cookie) == 4,
	       "ns_enum cookie offset");
_Static_assert(__builtin_offsetof(struct zos_ns_enum_args, count) == 10,
	       "ns_enum count offset");
#endif

#endif /* ZOS_OBJECT_H */
