/* KonaOS native object and namespace ABI (public native ABI).
 *
 * The public object lifecycle of architecture 4.9 and 4.10:
 *
 *   bind(path, flags) -> handle, type   resolve a namespace name, bind an instance
 *   call(dependency_ref, op, frame)     synchronous protocol operation
 *   unbind(handle)                      release the bound instance
 *
 * A bound handle carries one 8-bit **protocol type** (the numbers below);
 * there is no expected-type argument, so `bind` reports the type it
 * actually resolved and a caller that cares checks it.  Handles are
 * process-owned, opaque and small: they are neither addresses, bank
 * numbers nor endpoint numbers (endpoint numbers stay nucleus-internal).
 * One global owner-tagged handle table serves every process.
 *
 * Standard handles: every process starts with console input and output
 * already bound to `/dev/console` at the fixed numbers below, inherited
 * across SYS_EXEC; `bind` never returns either number, and the standard
 * numbers are absent when the build selected no console driver.
 *
 * Namespace enumeration (architecture 4.10) is a batched VFS operation
 * routed to the owning namespace provider.  `ns_enum` writes as many
 * `struct kos_ns_entry` records as fit the caller's buffer and returns the
 * next cookie, so a caller pages with `cookie` until zero entries arrive.
 * An entry's `container` flag is namespace metadata (further path
 * components may be traversed below the entry), not an object type.
 *
 * Argument-block placement: blocks live in the caller window,
 * 0100h-1FFFh, exactly like every other native syscall family; the path
 * fields are `{pointer, length}` pairs of 1..127 non-NUL bytes in the
 * caller's buffer window (0100h-DFFFh: a path is ordinary caller data, so
 * a literal or a stack buffer both qualify), resolved against the
 * caller's CWD; `out` is a caller-bank address in the same window,
 * written only through the checked bank-copy gate.
 *
 * Native syscalls (docs/abi.md section 6):
 *
 *   RST 30h, A = KOS_SYSCALL_BIND, HL = &struct kos_bind_args
 *       carry clear: HL = handle (>= 1), args->type filled with the 8-bit
 *       protocol type; carry set: A = KOS_E* (KOS_ENOENT unknown name,
 *       KOS_ENODEV a provider this build does not serve, KOS_EISDIR a
 *       container, KOS_EINVAL a bad block/path/flags, KOS_EMFILE/ENOSPC
 *       an exhausted handle table).
 *
 *   RST 30h, A = KOS_SYSCALL_UNBIND, HL = &struct kos_unbind_args
 *       carry clear: HL = 0 and the instance was released; carry set:
 *       A = KOS_EINVAL (bad handle, not this process' handle, or a
 *       descriptor kind that is not a bound object).
 *
 *   RST 30h, A = KOS_SYSCALL_NS_ENUM, HL = &struct kos_ns_enum_args
 *       carry clear: HL = the number of entries written (0 at end of
 *       stream) and args->cookie the next cookie; carry set: A = KOS_E*
 *       (KOS_ENOENT bad path, KOS_ENODEV, KOS_ENOTDIR an entry below a
 *       non-container, KOS_EINVAL a bad block).
 *
 *   RST 30h, A = KOS_SYSCALL_CHDIR, HL = &struct kos_chdir_args
 *       carry clear: HL = 0 and the caller's CWD is the canonical path;
 *       carry set: A = KOS_E* (KOS_ENOENT, KOS_ENOTDIR, KOS_ENODEV,
 *       KOS_EINVAL a bad block or an over-long path).
 *
 *   RST 30h, A = KOS_SYSCALL_GETCWD, HL = &struct kos_getcwd_args
 *       carry clear: HL = the path length in bytes excluding the NUL and
 *       the caller's buffer holds the path; carry set: A = KOS_EINVAL
 *       (bad block or a buffer too small).
 */

#ifndef KOS_OBJECT_H
#define KOS_OBJECT_H

#include "kos/types.h"

/* --- native syscall numbers (docs/abi.md section 6) -------------------- */

/* 06h and 07h reuse the two numbers the stage-3 IPC ABI pinned; the
 * lookup syscall and its SDK cache are gone, so 06h is
 * `bind` and 07h is `call` on an object handle.  08h/09h are the rest of
 * the object family and 0Ah/0Bh the namespace family. */
#define KOS_SYSCALL_BIND    0x06u
#define KOS_SYSCALL_CALL    0x07u
#define KOS_SYSCALL_UNBIND  0x08u
#define KOS_SYSCALL_NS_ENUM 0x09u
#define KOS_SYSCALL_CHDIR   0x0Au
#define KOS_SYSCALL_GETCWD  0x0Bu

/* --- 8-bit protocol types ------------------------------------------------ */

/* For the device families a protocol type and the driver class share a
 * number; FILE and the filesystem-namespace protocol are not device
 * classes and continue the sequence.  The two numberings are still
 * separate: class HID serves type INPUT, and the FILE class serves both
 * FILE and FS. */
#define KOS_TYPE_SERIAL  1u
#define KOS_TYPE_VIDEO   2u
#define KOS_TYPE_BLOCK   3u
#define KOS_TYPE_INPUT   4u
#define KOS_TYPE_IOC     5u
#define KOS_TYPE_CONSOLE 6u
#define KOS_TYPE_FILE    7u
#define KOS_TYPE_FS      8u
#define KOS_TYPE_SOUND   9u
#define KOS_TYPE_TIMER   10u

/* --- standard handle numbers -------------------------------------------- */

/* Console output (stdout) and input (stdin): bound to `/dev/console` at
 * process start when the selected build publishes one, and inherited by a
 * child process.  `bind` never returns either number. */
#define KOS_HANDLE_STDOUT 1u
#define KOS_HANDLE_STDIN  2u

/* The first number `bind` hands out; handles 1..2 are the standard ones. */
#define KOS_HANDLE_FIRST_BOUND 3u

/* --- namespace enumeration ---------------------------------------------- */

/* Entry name field: 24 bytes carries any ROMFS name (ROMFS_NAME_SIZE,
 * formats/include/romfs.h) and every `/dev` role name.  `name_length`
 * counts the terminating NUL. */
#define KOS_NS_NAME_MAX   24u
#define KOS_NS_ENTRY_SIZE 28u

#define KOS_NS_ENTRY_CONTAINER 0x01u /* further components may follow */

/* DEVICE bind flag: the caller asks for the object exclusively, so it may
 * reprogram the device beyond its protocol while it holds the handle.  The
 * bind fails with KOS_EBUSY when another handle already holds it that way,
 * and with KOS_ENOTSUP when the endpoint declares no exclusive support. */
#define KOS_BIND_EXCLUSIVE 0x8000u

struct kos_ns_entry {
	uint8_t type;        /* protocol type, 0 when not bindable    */
	uint8_t flags;       /* KOS_NS_ENTRY_*                        */
	uint8_t name_length; /* bytes incl. the NUL, 1..KOS_NS_NAME_MAX */
	uint8_t reserved;    /* 0                                     */
	char    name[KOS_NS_NAME_MAX]; /* NUL-terminated, zero-padded  */
};

/* --- argument blocks ----------------------------------------------------- */

/* 06h BIND {path, length, flags, type, reserved}: `type` and `reserved` are
 * outputs (reserved stays 0) and the handle is returned in HL. */
struct kos_bind_args {
	uint16_t path;
	uint16_t length;
	uint16_t flags;    /* provider-interpreted; KFS_O_* for FILE binds,
			    * KOS_BIND_EXCLUSIVE for DEVICE binds      */
	uint8_t  type;     /* out: the bound object's protocol type       */
	uint8_t  reserved; /* out: 0                                      */
};

/* 08h UNBIND {handle}. */
struct kos_unbind_args {
	uint8_t handle;
};

/* 09h NS_ENUM {path, length, cookie, out, out_size, count}: `cookie` is
 * read as the start index and written back as the next cookie; `count` is
 * the output count (also returned in HL). */
struct kos_ns_enum_args {
	uint16_t path;
	uint16_t length;
	uint16_t cookie;
	uint16_t out;      /* struct kos_ns_entry destination array */
	uint16_t out_size; /* bytes available at `out`              */
	uint16_t count;    /* out: entries written                  */
};

/* 0Ah CHDIR {path, length}. */
struct kos_chdir_args {
	uint16_t path;
	uint16_t length;
};

/* 0Bh GETCWD {buffer, length}: `buffer` is a caller-bank address in the
 * 0100h-DFFFh buffer window and `length` the bytes available there,
 * *including* the terminating NUL. */
struct kos_getcwd_args {
	uint16_t buffer;
	uint16_t length;
};

#if defined(__SDCC)
_Static_assert(sizeof(struct kos_ns_entry) == KOS_NS_ENTRY_SIZE,
	       "namespace entry must be 28 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_ns_entry, name) == 4,
	       "namespace entry name offset");
_Static_assert(sizeof(struct kos_bind_args) == 8,
	       "bind argument block must be 8 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_bind_args, type) == 6,
	       "bind type offset");
_Static_assert(sizeof(struct kos_unbind_args) == 1,
	       "unbind argument block must be 1 packed byte");
_Static_assert(sizeof(struct kos_ns_enum_args) == 12,
	       "ns_enum argument block must be 12 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_ns_enum_args, cookie) == 4,
	       "ns_enum cookie offset");
_Static_assert(__builtin_offsetof(struct kos_ns_enum_args, count) == 10,
	       "ns_enum count offset");
_Static_assert(sizeof(struct kos_chdir_args) == 4,
	       "chdir argument block must be 4 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_chdir_args, path) == 0,
	       "chdir path offset");
_Static_assert(sizeof(struct kos_getcwd_args) == 4,
	       "getcwd argument block must be 4 packed bytes");
_Static_assert(__builtin_offsetof(struct kos_getcwd_args, buffer) == 0,
	       "getcwd buffer offset");
#endif

#endif /* KOS_OBJECT_H */
