/* ZephyrOS M9 IOC-FAT filesystem provider (`iocfat.zmod`).
 *
 * Exposes the deployed MCU FatFs FS2 service to VFS through the path-based
 * `zfs_ops` provider ABI (architecture sections 32.12, 32.17 and 32.19).
 * The provider owns the whole FS2 protocol -- commands, replies, payload
 * offsets, status mapping and the READY/DONE bulk sequencing -- and a one
 * slot write-through 512-byte file-data cache.  `drivers/ioc/` stays
 * transport-only: this module consumes it only through the class interface
 * (`zioc_ops`) obtained with `device_find`/`device_interface` at init.
 *
 * Ownership and limits:
 *   - the only accepted mount source is `ioc.0`;
 *   - two file handles and one directory handle, matching the CAPS v1 slot
 *     counts, so provider exhaustion is ZOS_EMFILE;
 *   - paths are packed into 11-byte upper-case 8.3 FCB names; a component
 *     FatFs cannot represent is rejected with ZOS_EINVAL before any IOC
 *     traffic (architecture 32.8; the firmware's own name rules are
 *     mirrored here);
 *   - the resolver is stateless between provider calls: every operation
 *     sends ROOT and then one PUSH per directory component;
 *   - FS2 tokens never escape the provider; VFS handles are private.
 *
 * Size discipline (architecture 32.39, correction-01): one bulk-transfer
 * helper, one 8.3 codec, one directory-open path and table-driven status and
 * open-mode mapping keep the module within the 13-granule budget.  Every
 * automatic object is small; the 512-byte cache and the transfer staging
 * live in module BSS, never on the gate stack.
 *
 * All protocol constants are private to this file.
 */

#include "zephyros/driver.h"
#include "zephyros/errno.h"
#include "zephyros/ioc.h"
#include "zephyros/vfs.h"

/* --- FS2 protocol (private) ------------------------------------------- */

#define FS2_CMD_CAPS        0x30u
#define FS2_CMD_GENERATION  0x31u
#define FS2_CMD_ROOT        0x33u
#define FS2_CMD_PUSH        0x34u
#define FS2_CMD_OPEN_RO     0x35u
#define FS2_CMD_READ        0x36u
#define FS2_CMD_CLOSE       0x37u
#define FS2_CMD_OPENDIR     0x38u
#define FS2_CMD_READDIR     0x39u
#define FS2_CMD_CLOSEDIR    0x3Au
#define FS2_CMD_STAT        0x3Bu
#define FS2_CMD_OPEN_RW     0x3Du
#define FS2_CMD_WRITE       0x3Eu
#define FS2_CMD_SYNC        0x3Fu
#define FS2_CMD_TRUNCATE    0x40u
#define FS2_CMD_UNLINK      0x41u
#define FS2_CMD_RENAME      0x42u
#define FS2_CMD_MKDIR       0x43u
#define FS2_CMD_RMDIR       0x44u
#define FS2_CMD_XFER_STATUS 0x06u

#define FS2_RSP_XFER_STATUS 0x86u

#define FS2_STATUS_OK           0x00u
#define FS2_STATUS_NOT_FOUND    0x40u
#define FS2_STATUS_END          0x41u
#define FS2_STATUS_EXISTS       0x42u
#define FS2_STATUS_BAD_NAME     0x43u
#define FS2_STATUS_READ_ONLY    0x44u
#define FS2_STATUS_NO_SPACE     0x45u
#define FS2_STATUS_NOT_DIR      0x46u
#define FS2_STATUS_IS_DIR       0x47u
#define FS2_STATUS_NO_HANDLE    0x48u
#define FS2_STATUS_STALE        0x49u
#define FS2_STATUS_RANGE        0x4Au
#define FS2_STATUS_NO_MEDIA     0x4Bu
#define FS2_STATUS_IO           0x4Eu

#define FS2_RSP_UNKNOWN_CMD     0xFEu

/* DATA (payload) offsets inside a `struct zioc_frame`.  The `IOC_OFF_*`
 * names in the firmware tree are mailbox offsets; the IOC class driver
 * strips the mailbox header, so these are DATA-relative. */
#define FS2_OFF_CAP_VERSION        0u
#define FS2_OFF_CAP_STATUS_VERSION 1u
#define FS2_OFF_CAP_FLAGS          2u
#define FS2_OFF_CAP_GENERATION     4u
#define FS2_OFF_CAP_CHUNK_MAX      8u
#define FS2_OFF_CAP_TOKEN_BYTES    13u
#define FS2_CAP_REPLY_LEN          14u
#define FS2_GENERATION_REPLY_LEN   4u

#define FS2_OFF_NAME        0u
#define FS2_OFF_OPEN_MODE   0u
#define FS2_OFF_OPEN_NAME   1u
#define FS2_OFF_TOKEN       0u
#define FS2_OFF_OFFSET      2u
#define FS2_OFF_LENGTH      6u
#define FS2_OFF_TRUNC_SIZE  2u
#define FS2_OFF_RENAME_FROM 0u
#define FS2_OFF_RENAME_TO   11u

#define FS2_OFF_OPEN_TOKEN  0u
#define FS2_OFF_OPEN_SIZE   2u
#define FS2_OPEN_REPLY_LEN  7u
#define FS2_DIR_REPLY_LEN   2u

#define FS2_OFF_DIRENT_NAME 0u
#define FS2_OFF_DIRENT_ATTR 11u
#define FS2_OFF_DIRENT_SIZE 12u
#define FS2_DIRENT_REPLY_LEN 16u
#define FS2_ATTR_DIR        0x10u

#define FS2_OFF_STAT_SIZE   0u
#define FS2_OFF_STAT_ATTR   4u
#define FS2_STAT_REPLY_LEN  5u

#define FS2_READY_LEN       8u
#define FS2_OFF_READY_ID    0u
#define FS2_OFF_READY_DIR   1u
#define FS2_OFF_READY_LEN   2u
#define FS2_OFF_READY_OFF   4u
#define FS2_DIR_MCU_TO_Z80  0x00u
#define FS2_DIR_Z80_TO_MCU  0x01u

#define FS2_OFF_DONE_STATUS 1u
#define FS2_DONE_MIN_DATA   2u

/* CAPS v1 required values (work/M9-REFERENCE.md). */
#define FS2_CAP_VERSION        1u
#define FS2_CAP_STATUS_VERSION 1u
#define FS2_CAP_TOKEN_BYTES    2u
#define FS2_CAP_CHUNK_MIN      512u
#define FS2_CAP_READ_ONLY      0x0001u

/* --- provider limits -------------------------------------------------- */

#define IOCFAT_NAME_LEN      11u
#define IOCFAT_FILE_HANDLES  2u
#define IOCFAT_DIR_HANDLES   1u
#define IOCFAT_BLOCK_SIZE    512u
#define IOCFAT_PUSH_MAX      16u
#define IOCFAT_COMPONENT_MAX (IOCFAT_PUSH_MAX + 1u)

/* The file-data cache slot count is a module build setting (architecture
 * section 32.19).  The one-slot value is the project-owner budget decision
 * (correction-01): a second slot would not fit the 13-granule ceiling. */
#ifndef IOCFAT_CACHE_SLOTS
#define IOCFAT_CACHE_SLOTS 1u
#endif

#if (IOCFAT_CACHE_SLOTS != 1u)
#error IOCFAT_CACHE_SLOTS other than 1 exceeds the bank-7 budget
#endif

/* Open decision table entry bits (`iocfat_open_table`). */
#define FS2_OPENF_RO       0x80u /* OPEN_RO (else OPEN_RW)                */
#define FS2_OPENF_TRUNC    0x40u /* UPDATE then FS2_TRUNCATE to zero      */
#define FS2_OPENF_FALLBACK 0x20u /* UPDATE then CREATE_NEW on NOT_FOUND   */
#define FS2_OPENF_INVALID  0xFFu

/* --- instance state --------------------------------------------------- */

#define IOCFAT_H_WRITE 0x01u

struct iocfat_file {
	uint8_t used;
	uint8_t flags;
	uint16_t token;
	uint32_t generation;
};

struct iocfat_dir {
	uint8_t used;
	uint16_t token;
	uint32_t generation;
};

struct iocfat_cache_slot {
	void *owner;
	uint32_t generation;
	uint32_t offset;
	uint16_t valid;
	uint8_t data[IOCFAT_BLOCK_SIZE];
};

struct iocfat_instance {
	struct iocfat_file files[IOCFAT_FILE_HANDLES];
	struct iocfat_dir dirs[IOCFAT_DIR_HANDLES];
	struct iocfat_cache_slot cache[IOCFAT_CACHE_SLOTS];
	uint32_t generation;
	uint8_t mounted;
	uint8_t read_only;
};

static struct iocfat_instance iocfat;

/* Transport state captured at init and used by every provider operation. */
static const struct zioc_ops *iocfat_ioc_ops;
static void *iocfat_ioc_state;
static struct zioc_frame iocfat_request;
static struct zioc_frame iocfat_reply;
static uint8_t iocfat_name[IOCFAT_NAME_LEN];
static uint8_t iocfat_name2[IOCFAT_NAME_LEN];

static void *iocfat_self(void);
static int iocfat_leaf_op(const char *path, uint8_t count, uint8_t command);
static int iocfat_dir_open(const char *path, uint8_t count,
			   struct iocfat_dir **out);
static int iocfat_opendir(void *fs, const char *path, void **dir_out);
static int iocfat_readdir(void *fs, void *dir_handle, struct zfs_dirent *out);
static int iocfat_closedir(void *fs, void *dir_handle);

/* --- little-endian helpers -------------------------------------------- */

static uint16_t iocfat_get16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t iocfat_get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void iocfat_put16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static void iocfat_put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static void iocfat_copy(uint8_t *dst, const uint8_t *src, uint16_t n)
{
	uint16_t i;

	for (i = 0; i < n; ++i) {
		dst[i] = src[i];
	}
}

/* Copy `n` bytes of the cache block starting at `offset` into `dst`.  The
 * source base arrives as a parameter so SDCC emits a word relocation for the
 * global address instead of a byte-fragmented store. */
static void iocfat_copy_from(const uint8_t *src, uint16_t offset, uint8_t *dst,
			     uint16_t n)
{
	uint16_t i;

	for (i = 0; i < n; ++i) {
		dst[i] = src[offset + i];
	}
}

/* --- transport -------------------------------------------------------- */

/* One command exchange.  Returns 0 when the transport completed and the
 * reply is the expected class (or the explicit unknown-class reply, which
 * callers turn into ZOS_ENOTSUP); otherwise a negative transport error.
 * The reply is left in the module-static `iocfat_reply`. */
static int iocfat_exchange(uint8_t type, uint8_t length)
{
	int result;

	iocfat_request.type = type;
	iocfat_request.status = 0u;
	iocfat_request.length = length;
	result = iocfat_ioc_ops->command(iocfat_ioc_state, &iocfat_request,
					 &iocfat_reply);
	if (result != ZIOC_OK) {
		return -ZOS_EIO;
	}
	if (iocfat_reply.type == FS2_RSP_UNKNOWN_CMD) {
		return -ZOS_ENOTSUP;
	}
	if (iocfat_reply.type != (uint8_t)(type | 0x80u)) {
		return -ZOS_EIO;
	}
	return 0;
}

/* FS2 status -> native error, indexed by (status - FS2_STATUS_NOT_FOUND).
 * NO_HANDLE is resolved by the caller (slot exhaustion vs a stale handle);
 * the value here is only a fallback. */
static const uint8_t iocfat_status_table[] = {
	0x05u, /* NOT_FOUND -> ENOENT  */
	0x0Au, /* END       -> EIO    (never mapped here) */
	0x10u, /* EXISTS    -> EEXIST */
	0x02u, /* BAD_NAME  -> EINVAL */
	0x14u, /* READ_ONLY -> EROFS  */
	0x06u, /* NO_SPACE  -> ENOSPC */
	0x11u, /* NOT_DIR   -> ENOTDIR*/
	0x12u, /* IS_DIR    -> EISDIR */
	0x17u, /* NO_HANDLE -> EMFILE (fallback) */
	0x15u, /* STALE     -> ESTALE */
	0x02u, /* RANGE     -> EINVAL */
	0x03u, /* NO_MEDIA  -> ENODEV */
	0x0Au, /* TRANSPORT -> EIO    */
	0x0Au, /* UNKNOWN_WRITE -> EIO */
	0x0Au  /* IO        -> EIO    */
};

static int iocfat_map_status(uint8_t status, uint8_t no_handle_emfile)
{
	const uint8_t *table = iocfat_status_table;

	if (status == FS2_STATUS_OK) {
		return 0;
	}
	if (status == FS2_STATUS_NO_HANDLE) {
		return (no_handle_emfile != 0u) ? -ZOS_EMFILE : -ZOS_ESTALE;
	}
	if (status >= FS2_STATUS_NOT_FOUND && status <= FS2_STATUS_IO) {
		return -(int)table[status - FS2_STATUS_NOT_FOUND];
	}
	return -ZOS_EIO;
}

/* --- cache ------------------------------------------------------------ */

/* The one slot is a single static object, so it is indexed directly. */
static void iocfat_cache_clear(void)
{
	iocfat.cache[0].owner = (void *)0;
	iocfat.cache[0].valid = 0u;
}

static void iocfat_cache_invalidate_all(void)
{
	if (iocfat.cache[0].owner != (void *)0) {
		iocfat_cache_clear();
	}
}

/* Invalidate the slot when it is owned by `file` (write coherence). */
static void iocfat_cache_invalidate_file(void *file)
{
	if (file != (void *)0 && iocfat.cache[0].owner == file) {
		iocfat_cache_clear();
	}
}

/* Invalidate the slot when it is owned by any file other than `keep`.
 * FS2 serves at most two open files, so this is the whole other-file rule. */
static void iocfat_cache_invalidate_other(void *keep)
{
	void *owner = iocfat.cache[0].owner;

	if (owner != (void *)0 && owner != keep) {
		iocfat_cache_clear();
	}
}

/* --- 8.3 codec -------------------------------------------------------- */

/* True when a name byte can appear in an FS2 packed 8.3 name.  Mirrors the
 * firmware's name rules, so a component it rejects never reaches the lane. */
static uint8_t iocfat_legal(uint8_t c)
{
	if (c < 0x21u || c > 0x7Eu) {
		return 0u;
	}
	switch (c) {
	case '"': case '*': case '+': case ',': case ':': case ';':
	case '<': case '=': case '>': case '?': case '[': case '\\':
	case ']': case '|': case '~':
		return 0u;
	default:
		return 1u;
	}
}

/* Pack one component of `scan` (which points at its first byte, with no
 * leading `/`) into `out` (11 bytes, or null to validate only).  Returns the
 * component's source length, or 0xFFFF when it is not representable:
 * empty base, base > 8 or extension > 3, a second dot, a leading dot, or a
 * character FatFs rejects. */
static uint16_t iocfat_pack(const char *scan, uint8_t *out)
{
	uint16_t base = 0;
	uint16_t ext = 8;
	uint16_t i = 0;
	uint8_t seen_dot = 0;

	if (out != (uint8_t *)0) {
		uint8_t k;

		for (k = 0u; k < (uint8_t)IOCFAT_NAME_LEN; ++k) {
			out[k] = ' ';
		}
	}
	for (;;) {
		uint8_t c = (uint8_t)scan[i];

		if (c == '\0' || c == '/') {
			break;
		}
		if (c == '.') {
			if (seen_dot != 0u || base == 0u) {
				return 0xFFFFu;
			}
			seen_dot = 1u;
			++i;
			continue;
		}
		if (!iocfat_legal(c)) {
			return 0xFFFFu;
		}
		if (c >= 'a' && c <= 'z') {
			c = (uint8_t)(c - ('a' - 'A'));
		}
		if (seen_dot == 0u) {
			if (base >= 8u) {
				return 0xFFFFu;
			}
			if (out != (uint8_t *)0) {
				out[base] = c;
			}
			++base;
		} else {
			if (ext >= IOCFAT_NAME_LEN) {
				return 0xFFFFu;
			}
			if (out != (uint8_t *)0) {
				out[ext] = c;
			}
			++ext;
		}
		++i;
	}
	if (base == 0u || (seen_dot != 0u && ext == 8u)) {
		return 0xFFFFu;
	}
	return i;
}

/* The single 8.3 codec: walk `path`, validating every component and packing
 * component `index` (0-based) into `out` (11 bytes, or null when `index` is
 * the 0xFF "pack none" sentinel).  `*count` receives the number of
 * components, 0 for `/`.  Returns 0 or -ZOS_EINVAL; an unrepresentable
 * component is rejected before any IOC traffic. */
static int iocfat_walk(const char *path, uint8_t index, uint8_t *out,
		       uint8_t *count)
{
	const char *scan;
	uint8_t n = 0;

	if (path == (const char *)0 || path[0] != '/') {
		return -ZOS_EINVAL;
	}
	scan = path + 1;
	while (scan[0] != '\0') {
		uint16_t length;

		if (n >= (uint8_t)IOCFAT_COMPONENT_MAX) {
			return -ZOS_EINVAL;
		}
		length = iocfat_pack(scan, (n == index) ? out : (uint8_t *)0);
		if (length == 0xFFFFu) {
			return -ZOS_EINVAL;
		}
		++n;
		scan += length;
		if (scan[0] == '/') {
			if (scan[1] == '\0') {
				return -ZOS_EINVAL; /* no trailing slash */
			}
			++scan;
		} else if (scan[0] != '\0') {
			return -ZOS_EINVAL;
		}
	}
	*count = n;
	return 0;
}

/* Send ROOT, then one PUSH per component.  With `leaf` nonzero the final
 * component is not pushed; it is packed into `iocfat_name` instead.  `count`
 * arrives from iocfat_walk, so every component is already known-valid. */
static int iocfat_resolve(const char *path, uint8_t count, uint8_t leaf)
{
	const char *scan = path + 1;
	uint8_t pack_leaf = (uint8_t)((leaf != 0u && count != 0u) ? 1u : 0u);
	uint8_t pushes = (uint8_t)(count - pack_leaf);
	uint8_t i;
	int result;

	result = iocfat_exchange(FS2_CMD_ROOT, 0u);
	if (result != 0) {
		return result;
	}
	result = iocfat_map_status(iocfat_reply.status, 0u);
	if (result != 0) {
		return result;
	}
	for (i = 0u; i < pushes; ++i) {
		scan += iocfat_pack(scan, iocfat_name) + 1u;
		iocfat_copy(&iocfat_request.data[FS2_OFF_NAME], iocfat_name,
			    IOCFAT_NAME_LEN);
		result = iocfat_exchange(FS2_CMD_PUSH, IOCFAT_NAME_LEN);
		if (result != 0) {
			return result;
		}
		result = iocfat_map_status(iocfat_reply.status, 0u);
		if (result != 0) {
			return result;
		}
	}
	if (pack_leaf != 0u) {
		(void)iocfat_pack(scan, iocfat_name);
	}
	return 0;
}

/* --- handle pools ----------------------------------------------------- */

/* Two file slots and one directory slot, matching CAPS v1.  They are small
 * and fixed, so they are unrolled rather than iterated. */
static struct iocfat_file *iocfat_file_alloc(void)
{
	if (iocfat.files[0].used == 0u) {
		iocfat.files[0].used = 1u;
		return &iocfat.files[0];
	}
	if (iocfat.files[1].used == 0u) {
		iocfat.files[1].used = 1u;
		return &iocfat.files[1];
	}
	return (struct iocfat_file *)0;
}

static struct iocfat_file *iocfat_file_get(void *handle)
{
	if (handle == (void *)&iocfat.files[0]) {
		return (iocfat.files[0].used != 0u) ? &iocfat.files[0]
						    : (struct iocfat_file *)0;
	}
	if (handle == (void *)&iocfat.files[1]) {
		return (iocfat.files[1].used != 0u) ? &iocfat.files[1]
						    : (struct iocfat_file *)0;
	}
	return (struct iocfat_file *)0;
}

static struct iocfat_dir *iocfat_dir_alloc(void)
{
	if (iocfat.dirs[0].used == 0u) {
		iocfat.dirs[0].used = 1u;
		return &iocfat.dirs[0];
	}
	return (struct iocfat_dir *)0;
}

static struct iocfat_dir *iocfat_dir_get(void *handle)
{
	if (handle == (void *)&iocfat.dirs[0] && iocfat.dirs[0].used != 0u) {
		return &iocfat.dirs[0];
	}
	return (struct iocfat_dir *)0;
}

/* Reject a generation change on a live handle: nothing is retried. */
static int iocfat_file_generation(struct iocfat_file *file)
{
	if (file->generation != iocfat.generation) {
		iocfat_cache_invalidate_all();
		return -ZOS_ESTALE;
	}
	return 0;
}

/* --- instance lifecycle ----------------------------------------------- */

static void iocfat_zero_instance(void);

/* Clear the per-mount session.  `read_only` comes from the device's CAPS, so
 * it survives the reset: a write-protected card must be refused before the
 * provider sends anything, not by the firmware's status alone. */
static void iocfat_reset(void)
{
	uint8_t read_only = iocfat.read_only;

	iocfat_zero_instance();
	iocfat.read_only = read_only;
}

static int iocfat_query_generation(void)
{
	int result;

	result = iocfat_exchange(FS2_CMD_GENERATION, 0u);
	if (result != 0) {
		return result;
	}
	if (iocfat_reply.status != FS2_STATUS_OK ||
	    iocfat_reply.length < FS2_GENERATION_REPLY_LEN) {
		int mapped = iocfat_map_status(iocfat_reply.status, 0u);

		return (mapped != 0) ? mapped : -ZOS_EIO;
	}
	iocfat.generation = iocfat_get32(&iocfat_reply.data[0]);
	return 0;
}

/* --- bulk transfer ---------------------------------------------------- */

/* One FS2 data transfer: the FS2_READ/FS2_WRITE command, READY validation
 * (exact length for a write, at most `length` and non-empty for a read, plus
 * direction, nonzero transfer id and the echoed offset) and the matching
 * bulk phase.  A write also awaits its DONE.  Returns the transferred byte
 * count (0 for an end-of-file read), or a negative error. */
static int iocfat_transfer(uint8_t command, uint16_t token, uint32_t offset,
			   uint16_t length, uint8_t *data)
{
	uint16_t ready_len;
	int result;

	iocfat_put16(&iocfat_request.data[FS2_OFF_TOKEN], token);
	iocfat_put32(&iocfat_request.data[FS2_OFF_OFFSET], offset);
	iocfat_put16(&iocfat_request.data[FS2_OFF_LENGTH], length);
	result = iocfat_exchange(command, 8u);
	if (result != 0) {
		return result;
	}
	if (iocfat_reply.status == FS2_STATUS_STALE) {
		iocfat_cache_invalidate_all();
		return -ZOS_ESTALE;
	}
	if (iocfat_reply.status != FS2_STATUS_OK) {
		return iocfat_map_status(iocfat_reply.status, 0u);
	}
	if (iocfat_reply.length != FS2_READY_LEN) {
		return -ZOS_EIO;
	}
	ready_len = iocfat_get16(&iocfat_reply.data[FS2_OFF_READY_LEN]);
	if (command == FS2_CMD_READ) {
		if (ready_len == 0u) {
			return 0; /* end of file: a successful short read */
		}
		if (ready_len > length ||
		    iocfat_reply.data[FS2_OFF_READY_ID] == 0u ||
		    iocfat_reply.data[FS2_OFF_READY_DIR] !=
			    FS2_DIR_MCU_TO_Z80 ||
		    iocfat_get32(&iocfat_reply.data[FS2_OFF_READY_OFF]) !=
			    offset) {
			return -ZOS_EIO;
		}
		if (iocfat_ioc_ops->bulk_read(iocfat_ioc_state, data,
					      ready_len) != ZIOC_OK) {
			return -ZOS_EIO;
		}
		return (int)ready_len;
	}
	if (ready_len != length ||
	    iocfat_reply.data[FS2_OFF_READY_ID] == 0u ||
	    iocfat_reply.data[FS2_OFF_READY_DIR] != FS2_DIR_Z80_TO_MCU ||
	    iocfat_get32(&iocfat_reply.data[FS2_OFF_READY_OFF]) != offset) {
		return -ZOS_EIO;
	}
	if (iocfat_ioc_ops->bulk_write(iocfat_ioc_state, data,
				       length) != ZIOC_OK) {
		return -ZOS_EIO;
	}
	result = iocfat_exchange(FS2_CMD_XFER_STATUS, 0u);
	if (result != 0) {
		return -ZOS_EIO;
	}
	if (iocfat_reply.type != FS2_RSP_XFER_STATUS ||
	    iocfat_reply.length < FS2_DONE_MIN_DATA ||
	    iocfat_reply.data[FS2_OFF_DONE_STATUS] != FS2_STATUS_OK) {
		return -ZOS_EIO;
	}
	return (int)length;
}

/* Read the 512-byte-aligned `block` of `file` into the cache slot.  Returns
 * 0 or a negative error; an EOF block fills with a zero valid count. */
static int iocfat_cache_fill(struct iocfat_file *file, uint32_t block)
{
	int result;

	iocfat_cache_clear();
	result = iocfat_transfer(FS2_CMD_READ, file->token, block,
				 (uint16_t)IOCFAT_BLOCK_SIZE,
				 iocfat.cache[0].data);
	if (result < 0) {
		return result;
	}
	iocfat.cache[0].owner = file;
	iocfat.cache[0].generation = iocfat.generation;
	iocfat.cache[0].offset = block;
	iocfat.cache[0].valid = (uint16_t)result;
	return 0;
}

/* --- zfs_ops: instantiation ------------------------------------------- */

static int iocfat_mount(void *provider_state, const char *source, void **fs)
{
	struct iocfat_dir *dir;
	int result;

	(void)provider_state;
	if (fs == (void **)0 || source == (const char *)0 ||
	    source[0] != 'i' || source[1] != 'o' || source[2] != 'c' ||
	    source[3] != '.' || source[4] != '0' || source[5] != '\0') {
		return -ZOS_EINVAL;
	}
	iocfat_reset();
	result = iocfat_query_generation();
	if (result != 0) {
		return result;
	}
	/* Probe the root: opening `/` must succeed on mounted media; an absent
	 * or FAT-less card fails here (NO_MEDIA maps to ZOS_ENODEV). */
	result = iocfat_dir_open("/", 0u, &dir);
	if (result != 0) {
		return result;
	}
	(void)iocfat_closedir((void *)0, dir);
	iocfat.mounted = 1u;
	*fs = iocfat_self();
	return 0;
}

/* --- zfs_ops: files --------------------------------------------------- */

/* Open decision table, indexed by `(flags >> 1) & 0x0F` = WRITE | CREATE<<1 |
 * EXCL<<2 | TRUNC<<3.  The low two bits are the FS2 OPEN_RW mode
 * (0 UPDATE, 1 CREATE_NEW, 2 CREATE_ALWAYS). */
static const uint8_t iocfat_open_table[16] = {
	FS2_OPENF_RO, 0x00u, FS2_OPENF_INVALID, FS2_OPENF_FALLBACK,
	FS2_OPENF_INVALID, FS2_OPENF_INVALID, FS2_OPENF_INVALID, 0x01u,
	FS2_OPENF_INVALID, FS2_OPENF_TRUNC, FS2_OPENF_INVALID, 0x02u,
	FS2_OPENF_INVALID, FS2_OPENF_INVALID, FS2_OPENF_INVALID, 0x01u
};

/* Resolve the parent and send OPEN_RO/OPEN_RW (plus a truncating TRUNCATE).
 * Reports the FS2 token and size. */
static int iocfat_open_send(const char *path, uint8_t count, uint8_t entry,
			    uint16_t *token_out, uint32_t *size_out)
{
	uint16_t token;
	uint32_t file_size;
	int result;

	result = iocfat_resolve(path, count, 1u); /* leaf in iocfat_name */
	if (result != 0) {
		return result;
	}
	if ((entry & FS2_OPENF_RO) != 0u) {
		iocfat_copy(&iocfat_request.data[FS2_OFF_NAME], iocfat_name,
			    IOCFAT_NAME_LEN);
		result = iocfat_exchange(FS2_CMD_OPEN_RO, IOCFAT_NAME_LEN);
	} else {
		iocfat_request.data[FS2_OFF_OPEN_MODE] = entry & 0x03u;
		iocfat_copy(&iocfat_request.data[FS2_OFF_OPEN_NAME],
			    iocfat_name, IOCFAT_NAME_LEN);
		result = iocfat_exchange(FS2_CMD_OPEN_RW,
					 (uint8_t)(1u + IOCFAT_NAME_LEN));
	}
	if (result != 0) {
		return result;
	}
	if (iocfat_reply.status == FS2_STATUS_STALE) {
		iocfat_cache_invalidate_all();
		return -ZOS_ESTALE;
	}
	result = iocfat_map_status(iocfat_reply.status, 1u);
	if (result != 0) {
		return result;
	}
	if (iocfat_reply.length != FS2_OPEN_REPLY_LEN) {
		return -ZOS_EIO;
	}
	token = iocfat_get16(&iocfat_reply.data[FS2_OFF_OPEN_TOKEN]);
	file_size = iocfat_get32(&iocfat_reply.data[FS2_OFF_OPEN_SIZE]);
	if ((entry & FS2_OPENF_TRUNC) != 0u) {
		/* TRUNC without CREATE: UPDATE, then TRUNCATE to zero. */
		iocfat_put16(&iocfat_request.data[FS2_OFF_TOKEN], token);
		iocfat_put32(&iocfat_request.data[FS2_OFF_TRUNC_SIZE], 0u);
		result = iocfat_exchange(FS2_CMD_TRUNCATE, 6u);
		if (result == 0) {
			result = iocfat_map_status(iocfat_reply.status, 0u);
		}
		if (result != 0) {
			/* Close the just-opened handle so no slot leaks. */
			iocfat_put16(&iocfat_request.data[FS2_OFF_TOKEN],
				     token);
			(void)iocfat_exchange(FS2_CMD_CLOSE, 2u);
			return result;
		}
		file_size = 0u;
	}
	if ((entry & FS2_OPENF_RO) == 0u) {
		/* A create/truncate may have changed any cached block. */
		iocfat_cache_invalidate_all();
	}
	*token_out = token;
	*size_out = file_size;
	return 0;
}

static int iocfat_open(void *fs, const char *path, uint16_t flags,
		       void **file, uint32_t *size)
{
	const uint8_t *table = iocfat_open_table;
	struct iocfat_file *slot;
	uint8_t entry;
	uint8_t count;
	uint8_t read_only;
	uint16_t token = 0u;
	uint32_t file_size = 0u;
	int result;

	(void)fs;
	if (file == (void **)0 || size == (uint32_t *)0) {
		return -ZOS_EINVAL;
	}
	result = iocfat_walk(path, 0xFFu, (uint8_t *)0, &count);
	if (result != 0) {
		return result;
	}
	if (count == 0u) {
		return -ZOS_EISDIR; /* the root is a synthesized directory */
	}
	entry = table[(uint8_t)((flags >> 1) & 0x0Fu)];
	if (entry == FS2_OPENF_INVALID) {
		return -ZOS_EINVAL;
	}
	read_only = (uint8_t)((entry & FS2_OPENF_RO) != 0u ? 1u : 0u);
	if (read_only == 0u && iocfat.read_only != 0u) {
		return -ZOS_EROFS;
	}
	slot = iocfat_file_alloc();
	if (slot == (struct iocfat_file *)0) {
		return -ZOS_EMFILE;
	}
	result = iocfat_open_send(path, count, entry, &token, &file_size);
	if (result == -ZOS_ENOENT && (entry & FS2_OPENF_FALLBACK) != 0u) {
		result = iocfat_open_send(path, count, 0x01u, &token,
					  &file_size);
	}
	if (result != 0) {
		slot->used = 0u;
		return result;
	}
	slot->token = token;
	slot->generation = iocfat.generation;
	slot->flags = (read_only != 0u) ? 0u : IOCFAT_H_WRITE;
	*file = slot;
	*size = file_size;
	return 0;
}

static int iocfat_close(void *fs, void *file)
{
	struct iocfat_file *handle = iocfat_file_get(file);
	int result = 0;

	(void)fs;
	if (handle == (struct iocfat_file *)0) {
		return -ZOS_EINVAL;
	}
	iocfat_cache_invalidate_file(handle);
	if (iocfat_file_generation(handle) != 0) {
		handle->used = 0u;
		return -ZOS_ESTALE;
	}
	/* close is SYNC then CLOSE (architecture 32.27). */
	iocfat_put16(&iocfat_request.data[FS2_OFF_TOKEN], handle->token);
	result = iocfat_exchange(FS2_CMD_SYNC, 2u);
	iocfat_put16(&iocfat_request.data[FS2_OFF_TOKEN], handle->token);
	result = iocfat_exchange(FS2_CMD_CLOSE, 2u);
	if (result == 0 && iocfat_reply.status != FS2_STATUS_OK) {
		result = iocfat_map_status(iocfat_reply.status, 0u);
	}
	handle->used = 0u;
	return result;
}

static int iocfat_read(void *fs, void *file, uint32_t offset, void *buf,
		       uint16_t len)
{
	struct iocfat_file *handle = iocfat_file_get(file);
	uint8_t *dst = (uint8_t *)buf;
	uint32_t confirmed = 0;
	uint16_t remaining = len;

	(void)fs;
	if (handle == (struct iocfat_file *)0 || buf == (void *)0) {
		return -ZOS_EINVAL;
	}
	if (iocfat_file_generation(handle) != 0) {
		return -ZOS_ESTALE;
	}
	while (remaining != 0u) {
		uint32_t block = offset & ~((uint32_t)IOCFAT_BLOCK_SIZE - 1uL);
		uint16_t within = (uint16_t)(offset & 511uL);
		uint16_t available;
		uint16_t n;

		if (iocfat.cache[0].owner != handle ||
		    iocfat.cache[0].generation != iocfat.generation ||
		    iocfat.cache[0].offset != block) {
			int filled = iocfat_cache_fill(handle, block);

			if (filled != 0) {
				return (confirmed != 0u) ? (int)confirmed
							 : filled;
			}
		}
		available = iocfat.cache[0].valid;
		if (within >= available) {
			return (int)confirmed; /* short read / EOF */
		}
		n = (uint16_t)(available - within);
		if (n > remaining) {
			n = remaining;
		}
		iocfat_copy_from(iocfat.cache[0].data, within, &dst[confirmed], n);
		confirmed += n;
		offset += n;
		remaining = (uint16_t)(remaining - n);
	}
	return (int)confirmed;
}

static int iocfat_write(void *fs, void *file, uint32_t offset,
			const void *buf, uint16_t len)
{
	struct iocfat_file *handle = iocfat_file_get(file);
	const uint8_t *src = (const uint8_t *)buf;
	uint32_t confirmed = 0;

	(void)fs;
	if (handle == (struct iocfat_file *)0 || buf == (const void *)0) {
		return -ZOS_EINVAL;
	}
	if (iocfat_file_generation(handle) != 0) {
		return -ZOS_ESTALE;
	}
	if (iocfat.read_only != 0u || (handle->flags & IOCFAT_H_WRITE) == 0u) {
		return -ZOS_EROFS;
	}
	while (confirmed < (uint32_t)len) {
		uint16_t chunk = (uint16_t)((uint32_t)len - confirmed);
		int result;

		if (chunk > IOCFAT_BLOCK_SIZE) {
			chunk = (uint16_t)IOCFAT_BLOCK_SIZE;
		}
		result = iocfat_transfer(FS2_CMD_WRITE, handle->token,
					 offset + confirmed, chunk,
					 (uint8_t *)&src[confirmed]);
		if (result < 0) {
			return result;
		}
		/* Write-through: only a confirmed DONE reaches here. */
		iocfat_cache_invalidate_file(handle);
		iocfat_cache_invalidate_other(handle);
		confirmed += chunk;
	}
	return 0;
}

static int iocfat_sync(void *fs, void *file)
{
	struct iocfat_file *handle = iocfat_file_get(file);
	int result;

	(void)fs;
	if (handle == (struct iocfat_file *)0) {
		return -ZOS_EINVAL;
	}
	if (iocfat_file_generation(handle) != 0) {
		return -ZOS_ESTALE;
	}
	iocfat_put16(&iocfat_request.data[FS2_OFF_TOKEN], handle->token);
	result = iocfat_exchange(FS2_CMD_SYNC, 2u);
	if (result != 0) {
		return result;
	}
	return iocfat_map_status(iocfat_reply.status, 0u);
}

/* --- zfs_ops: metadata ------------------------------------------------ */

static int iocfat_stat(void *fs, const char *path, struct zfs_stat *out)
{
	uint8_t count;
	uint32_t size;
	int result;

	(void)fs;
	if (out == (struct zfs_stat *)0) {
		return -ZOS_EINVAL;
	}
	result = iocfat_walk(path, 0xFFu, (uint8_t *)0, &count);
	if (result != 0) {
		return result;
	}
	size = 0u;
	out->type = ZFS_TYPE_DIR; /* `/` is a synthesized directory */
	if (count != 0u) {
		result = iocfat_leaf_op(path, count, FS2_CMD_STAT);
		if (result != 0) {
			return result;
		}
		if (iocfat_reply.length != FS2_STAT_REPLY_LEN) {
			return -ZOS_EIO;
		}
		if ((iocfat_reply.data[FS2_OFF_STAT_ATTR] & FS2_ATTR_DIR) == 0u) {
			out->type = ZFS_TYPE_FILE;
			size = iocfat_get32(&iocfat_reply.data[FS2_OFF_STAT_SIZE]);
		}
	}
	out->attributes = 0u;
	out->reserved = 0u;
	out->size = size;
	out->ctime = ZFS_TIME_UNKNOWN;
	out->mtime = ZFS_TIME_UNKNOWN;
	return 0;
}

/* --- zfs_ops: directories --------------------------------------------- */

/* Allocate the directory handle, resolve the directory, OPENDIR and store
 * its token.  Returns 0 or a negative error. */
static int iocfat_dir_open(const char *path, uint8_t count,
			   struct iocfat_dir **out)
{
	struct iocfat_dir *dir = iocfat_dir_alloc();
	int result;

	if (dir == (struct iocfat_dir *)0) {
		return -ZOS_EMFILE;
	}
	result = iocfat_resolve(path, count, 0u);
	if (result != 0) {
		dir->used = 0u;
		return result;
	}
	result = iocfat_exchange(FS2_CMD_OPENDIR, 0u);
	if (result != 0) {
		dir->used = 0u;
		return result;
	}
	if (iocfat_reply.status == FS2_STATUS_STALE) {
		dir->used = 0u;
		iocfat_cache_invalidate_all();
		return -ZOS_ESTALE;
	}
	result = iocfat_map_status(iocfat_reply.status, 1u);
	if (result != 0) {
		dir->used = 0u;
		return result;
	}
	if (iocfat_reply.length != FS2_DIR_REPLY_LEN) {
		dir->used = 0u;
		return -ZOS_EIO;
	}
	dir->token = iocfat_get16(&iocfat_reply.data[0]);
	dir->generation = iocfat.generation;
	*out = dir;
	return 0;
}

static int iocfat_opendir(void *fs, const char *path, void **dir_out)
{
	uint8_t count;
	int result;

	(void)fs;
	if (dir_out == (void **)0) {
		return -ZOS_EINVAL;
	}
	result = iocfat_walk(path, 0xFFu, (uint8_t *)0, &count);
	if (result != 0) {
		return result;
	}
	if (count > (uint8_t)IOCFAT_PUSH_MAX) {
		return -ZOS_EINVAL; /* the firmware resolver is 16 deep */
	}
	return iocfat_dir_open(path, count, (struct iocfat_dir **)dir_out);
}

/* Unpack an 11-byte packed name into `NAME.EXT` (no trailing dot).  Packed
 * names carry no interior spaces, so a space ends the significant run. */
static uint16_t iocfat_unpack(const uint8_t *packed, char *out)
{
	uint8_t n = 0;
	uint8_t i;

	for (i = 0u; i < 8u && packed[i] != ' '; ++i) {
		out[n++] = (char)packed[i];
	}
	if (packed[8] != ' ') {
		out[n++] = '.';
		for (i = 8u; i < 11u && packed[i] != ' '; ++i) {
			out[n++] = (char)packed[i];
		}
	}
	out[n] = '\0';
	return (uint16_t)(n + 1u);
}

static int iocfat_readdir(void *fs, void *dir_handle,
			  struct zfs_dirent *out)
{
	struct iocfat_dir *dir = iocfat_dir_get(dir_handle);
	int result;

	(void)fs;
	if (dir == (struct iocfat_dir *)0 || out == (struct zfs_dirent *)0) {
		return -ZOS_EINVAL;
	}
	if (dir->generation != iocfat.generation) {
		iocfat_cache_invalidate_all();
		return -ZOS_ESTALE;
	}
	iocfat_put16(&iocfat_request.data[FS2_OFF_TOKEN], dir->token);
	result = iocfat_exchange(FS2_CMD_READDIR, 2u);
	if (result != 0) {
		return result;
	}
	if (iocfat_reply.status == FS2_STATUS_END) {
		return 0;
	}
	if (iocfat_reply.status == FS2_STATUS_STALE) {
		iocfat_cache_invalidate_all();
		return -ZOS_ESTALE;
	}
	if (iocfat_reply.status != FS2_STATUS_OK) {
		return iocfat_map_status(iocfat_reply.status, 0u);
	}
	if (iocfat_reply.length != FS2_DIRENT_REPLY_LEN) {
		return -ZOS_EIO;
	}
	out->type = ((iocfat_reply.data[FS2_OFF_DIRENT_ATTR] & FS2_ATTR_DIR)
			     != 0u) ? ZFS_TYPE_DIR : ZFS_TYPE_FILE;
	out->attributes = 0u;
	out->size = (out->type == ZFS_TYPE_DIR)
			    ? 0u
			    : iocfat_get32(&iocfat_reply.data[FS2_OFF_DIRENT_SIZE]);
	out->ctime = ZFS_TIME_UNKNOWN;
	out->mtime = ZFS_TIME_UNKNOWN;
	out->name_length = iocfat_unpack(
		&iocfat_reply.data[FS2_OFF_DIRENT_NAME], out->name);
	if (out->name_length < 2u) {
		return -ZOS_EIO;
	}
	return 1;
}

static int iocfat_closedir(void *fs, void *dir_handle)
{
	struct iocfat_dir *dir = iocfat_dir_get(dir_handle);
	int result = 0;

	(void)fs;
	if (dir == (struct iocfat_dir *)0) {
		return -ZOS_EINVAL;
	}
	if (dir->generation == iocfat.generation) {
		iocfat_put16(&iocfat_request.data[FS2_OFF_TOKEN], dir->token);
		result = iocfat_exchange(FS2_CMD_CLOSEDIR, 2u);
		if (result == 0 && iocfat_reply.status != FS2_STATUS_OK) {
			result = iocfat_map_status(iocfat_reply.status, 0u);
		}
	}
	dir->used = 0u;
	return result;
}

/* --- zfs_ops: namespace mutations ------------------------------------- */

/* Resolve the parent, pack the leaf name and send a namespace command
 * (STAT/MKDIR/RMDIR/UNLINK).  Returns 0 or a negative native error. */
static int iocfat_leaf_op(const char *path, uint8_t count, uint8_t command)
{
	int result = iocfat_resolve(path, count, 1u);

	if (result != 0) {
		return result;
	}
	iocfat_copy(&iocfat_request.data[FS2_OFF_NAME], iocfat_name,
		    IOCFAT_NAME_LEN);
	result = iocfat_exchange(command, IOCFAT_NAME_LEN);
	if (result != 0) {
		return result;
	}
	if (iocfat_reply.status == FS2_STATUS_STALE) {
		iocfat_cache_invalidate_all();
		return -ZOS_ESTALE;
	}
	return iocfat_map_status(iocfat_reply.status, 0u);
}

static int iocfat_mkdir(void *fs, const char *path)
{
	uint8_t count;
	int result;

	(void)fs;
	if (iocfat.read_only != 0u) {
		return -ZOS_EROFS;
	}
	result = iocfat_walk(path, 0xFFu, (uint8_t *)0, &count);
	if (result != 0) {
		return result;
	}
	if (count == 0u) {
		return -ZOS_EEXIST;
	}
	return iocfat_leaf_op(path, count, FS2_CMD_MKDIR);
}

static int iocfat_unlink(void *fs, const char *path)
{
	uint8_t count;
	int result;

	(void)fs;
	if (iocfat.read_only != 0u) {
		return -ZOS_EROFS;
	}
	result = iocfat_walk(path, 0xFFu, (uint8_t *)0, &count);
	if (result != 0) {
		return result;
	}
	if (count == 0u) {
		return -ZOS_EBUSY;
	}
	result = iocfat_leaf_op(path, count, FS2_CMD_UNLINK);
	if (result == 0) {
		/* FS2 closes every open handle on unlink; no cached byte can
		 * be trusted afterwards. */
		iocfat_cache_invalidate_all();
	}
	return result;
}

/* Emptiness check: open the directory through the shared path and enumerate
 * one entry.  The single directory slot makes a directory this provider
 * already has open report ZOS_EMFILE. */
static int iocfat_dir_is_empty(const char *path, uint8_t count)
{
	struct iocfat_dir *dir;
	struct zfs_dirent entry;
	int result = iocfat_dir_open(path, count, &dir);

	if (result != 0) {
		return result;
	}
	result = iocfat_readdir((void *)0, dir, &entry);
	(void)iocfat_closedir((void *)0, dir);
	if (result < 0) {
		return result;
	}
	return (result == 1) ? -ZOS_ENOTEMPTY : 0;
}

static int iocfat_rmdir(void *fs, const char *path)
{
	uint8_t count;
	int result;

	(void)fs;
	if (iocfat.read_only != 0u) {
		return -ZOS_EROFS;
	}
	result = iocfat_walk(path, 0xFFu, (uint8_t *)0, &count);
	if (result != 0) {
		return result;
	}
	if (count == 0u) {
		return -ZOS_EBUSY;
	}
	if (count > (uint8_t)IOCFAT_PUSH_MAX) {
		return -ZOS_EINVAL;
	}
	result = iocfat_dir_is_empty(path, count);
	if (result != 0) {
		return result;
	}
	/* Re-resolve the parent: the resolver is not persisted. */
	return iocfat_leaf_op(path, count, FS2_CMD_RMDIR);
}

/* True when both paths name the same parent (every component before the
 * last).  FS2 RENAME is same-directory only. */
static uint8_t iocfat_same_parent(const char *a, uint8_t na,
				  const char *b, uint8_t nb)
{
	uint8_t i;

	if (na != nb) {
		return 0u;
	}
	for (i = 0u; i + 1u < na; ++i) {
		uint8_t k;
		uint8_t count;

		(void)iocfat_walk(a, i, iocfat_name, &count);
		(void)iocfat_walk(b, i, iocfat_name2, &count);
		for (k = 0u; k < (uint8_t)IOCFAT_NAME_LEN; ++k) {
			if (iocfat_name[k] != iocfat_name2[k]) {
				return 0u;
			}
		}
	}
	return 1u;
}

static int iocfat_rename(void *fs, const char *oldpath, const char *newpath)
{
	uint8_t nold;
	uint8_t nnew;
	int result;

	(void)fs;
	if (iocfat.read_only != 0u) {
		return -ZOS_EROFS;
	}
	result = iocfat_walk(oldpath, 0xFFu, (uint8_t *)0, &nold);
	if (result != 0) {
		return result;
	}
	result = iocfat_walk(newpath, 0xFFu, (uint8_t *)0, &nnew);
	if (result != 0) {
		return result;
	}
	if (nold == 0u || nnew == 0u) {
		return -ZOS_EBUSY;
	}
	if (!iocfat_same_parent(oldpath, nold, newpath, nnew)) {
		return -ZOS_EXDEV;
	}
	result = iocfat_resolve(oldpath, nold, 1u); /* old leaf in iocfat_name */
	if (result != 0) {
		return result;
	}
	iocfat_copy(&iocfat_request.data[FS2_OFF_RENAME_FROM], iocfat_name,
		    IOCFAT_NAME_LEN);
	(void)iocfat_walk(newpath, (uint8_t)(nnew - 1u), iocfat_name2, &nnew);
	iocfat_copy(&iocfat_request.data[FS2_OFF_RENAME_TO], iocfat_name2,
		    IOCFAT_NAME_LEN);
	result = iocfat_exchange(FS2_CMD_RENAME,
				 (uint8_t)(2u * IOCFAT_NAME_LEN));
	if (result != 0) {
		return result;
	}
	iocfat_cache_invalidate_all();
	return iocfat_map_status(iocfat_reply.status, 0u);
}

static const struct zfs_ops iocfat_ops = {
	iocfat_mount,
	iocfat_open,
	iocfat_close,
	iocfat_read,
	iocfat_write,
	iocfat_sync,
	iocfat_stat,
	iocfat_opendir,
	iocfat_readdir,
	iocfat_closedir,
	iocfat_mkdir,
	iocfat_rmdir,
	iocfat_unlink,
	iocfat_rename
};

static const struct zfs_provider iocfat_provider = {
	"iocfat",
	&iocfat_ops,
	(void *)0
};

/* --- module entry ----------------------------------------------------- */

/* Address of the single provider instance, returned rather than stored so
 * SDCC emits a word relocation instead of a byte-fragmented address store. */
static void *iocfat_self(void)
{
	return (void *)&iocfat;
}

static void iocfat_zero_instance(void)
{
	uint8_t *p = (uint8_t *)iocfat_self();
	uint16_t i;

	for (i = 0u; i < (uint16_t)sizeof(struct iocfat_instance); ++i) {
		p[i] = 0u;
	}
}

int iocfat_init(const struct zdriver_api *api)
{
	zdev_handle_t handle;
	const void *class_ops = (const void *)0;
	void *ioc_state = (void *)0;
	const struct zioc_ops *ops;
	uint16_t chunk_max;
	uint16_t flags;
	int result;

	if (api == (const struct zdriver_api *)0) {
		return ZDRIVER_FAIL;
	}
	iocfat_zero_instance();
	handle = api->device_find(ZDEV_IOC,
				  ZCAP_IOC_COMMAND | ZCAP_IOC_BULK_READ |
				  ZCAP_IOC_BULK_WRITE, 0u);
	if (handle == ZDEV_HANDLE_INVALID) {
		return -ZOS_ENODEV;
	}
	if (api->device_interface(handle, ZDEV_IOC, &class_ops, &ioc_state) !=
	    ZDEV_OK) {
		return -ZOS_ENODEV;
	}
	ops = (const struct zioc_ops *)class_ops;
	if (ops == (const struct zioc_ops *)0 || ops->command == 0 ||
	    ops->bulk_read == 0 || ops->bulk_write == 0) {
		return -ZOS_ENODEV;
	}
	iocfat_ioc_ops = ops;
	iocfat_ioc_state = ioc_state;

	/* CAPS v1: version, 2-byte tokens and a 512-byte chunk. */
	result = iocfat_exchange(FS2_CMD_CAPS, 0u);
	if (result != 0) {
		return result;
	}
	if (iocfat_reply.status != FS2_STATUS_OK ||
	    iocfat_reply.length < FS2_CAP_REPLY_LEN) {
		return -ZOS_EIO;
	}
	if (iocfat_reply.data[FS2_OFF_CAP_VERSION] != FS2_CAP_VERSION ||
	    iocfat_reply.data[FS2_OFF_CAP_STATUS_VERSION] !=
		    FS2_CAP_STATUS_VERSION ||
	    iocfat_reply.data[FS2_OFF_CAP_TOKEN_BYTES] !=
		    FS2_CAP_TOKEN_BYTES) {
		return -ZOS_ENOTSUP;
	}
	chunk_max = iocfat_get16(&iocfat_reply.data[FS2_OFF_CAP_CHUNK_MAX]);
	if (chunk_max < FS2_CAP_CHUNK_MIN) {
		return -ZOS_ENOTSUP;
	}
	flags = iocfat_get16(&iocfat_reply.data[FS2_OFF_CAP_FLAGS]);
	iocfat.read_only =
		(uint8_t)((flags & FS2_CAP_READ_ONLY) != 0u ? 1u : 0u);
	iocfat.generation =
		iocfat_get32(&iocfat_reply.data[FS2_OFF_CAP_GENERATION]);

	return api->fs_register(&iocfat_provider);
}
