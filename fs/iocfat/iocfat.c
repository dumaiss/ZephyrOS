/* ZephyrOS IOC-FAT resident filesystem driver (packages 30, 42b).
 *
 * The deployed MCU FatFs FS2 service behind the M10 file language
 * (architecture 36.13): this driver implements the FILE protocol
 * (`bind`/`call`/`unbind` on `/FS/...`) and the filesystem-namespace
 * protocol (`stat`, `mkdir`, `rmdir`, `unlink`, `rename`, enumeration) of
 * the `/FS` root.  The nucleus FS provider routes and forwards; it holds no
 * FAT/FS2 semantics, and this driver owns pathname meaning, file creation,
 * mutation, enumeration and backend state below `/FS`.
 *
 * Placement (architecture 36.6, 36.11): the driver is host-linked at its
 * final bank-6 addresses and its IOC transport dependency is host-resolved
 * (`depends: [{class: 5, instance: 0}]`), so it never looks a peer up at
 * run time.  Every buffer it names in a copy is `SERVICE_IO`
 * (8000h-BFFFh), and the FS2 cache and the transfer staging live there.
 * Driver-to-driver data movement is direct: the IOC service's staged reply
 * frame and bulk buffer are bank-6 memory, so one same-domain copy reaches
 * them (36.8).
 *
 * Ownership and limits:
 *   - two file handles and one directory handle, matching the CAPS v1 slot
 *     counts, so provider exhaustion is ZOS_EMFILE;
 *   - paths are packed into 11-byte upper-case 8.3 FCB names; a component
 *     FatFs cannot represent is rejected with ZOS_EINVAL before any IOC
 *     traffic (the firmware's own name rules are mirrored here);
 *   - the resolver is stateless between operations: every operation sends
 *     ROOT and then one PUSH per directory component;
 *   - enumeration is stateless too: the cookie is the entry index, and a
 *     batch re-opens the directory and skips to it;
 *   - FS2 tokens never escape the driver; a bound file's identity is its
 *     slot index plus one, which the nucleus records as the handle's object
 *     (`payload[0]` of every FILE operation).
 *
 * Nucleus transport (abi/zos/protocol.h): name operations receive their path
 * in a nucleus request buffer named by `payload[0..1]` - the fixed-core VFS
 * workspace, which this driver's window always maps - and enumeration also
 * receives the caller's entry array as the call descriptor.  A card that is
 * absent or unreadable does not fail the driver's init (a required driver
 * that cannot initialize is a boot failure, and `/SYSTEM` must stay
 * usable): every operation then answers -ZOS_ENODEV.
 *
 * All protocol constants are private to this file.
 */

#include "zephyros/errno.h"
#include "zephyros/ipc.h"
#include "zephyros/ioc.h"

#include "zos/fs.h"
#include "zos/object.h"
#include "zos/protocol.h"

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

/* --- instance state --------------------------------------------------- */

#define IOCFAT_NAME_LEN      11u
#define IOCFAT_FILE_HANDLES  2u
#define IOCFAT_DIR_HANDLES   1u
#define IOCFAT_BLOCK_SIZE    512u
#define IOCFAT_PUSH_MAX      16u
#define IOCFAT_COMPONENT_MAX (IOCFAT_PUSH_MAX + 1u)

/* The driver's one dependency: the IOC transport, manifest position 0. */
#define IOCFAT_DEP_IOC 0u

/* Bound object identities presented to the nucleus: file slot i is object
 * i + 1; 0 is the filesystem-namespace root (no instance).  The directory
 * slot is internal to one enumeration call. */
#define IOCFAT_FILE_OBJECT_MAX ((uint8_t)IOCFAT_FILE_HANDLES)

/* Open decision table entry bits (`iocfat_open_table`). */
#define FS2_OPENF_RO       0x80u /* OPEN_RO (else OPEN_RW)                */
#define FS2_OPENF_TRUNC    0x40u /* UPDATE then FS2_TRUNCATE to zero      */
#define FS2_OPENF_FALLBACK 0x20u /* UPDATE then CREATE_NEW on NOT_FOUND   */
#define FS2_OPENF_INVALID  0xFFu

#define IOCFAT_H_WRITE 0x01u

struct iocfat_file {
	uint8_t used;
	uint8_t flags;
	uint16_t token;
	uint32_t generation;
	uint32_t offset; /* the bound instance's position */
	uint32_t size;
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
	struct iocfat_cache_slot cache[1];
	uint32_t generation;
	uint8_t mounted;
	uint8_t read_only;
};

static struct iocfat_instance iocfat;

/* Transport state captured at init. */
static const struct ipc_service_api *iocfat_api;
static uint8_t iocfat_ioc;   /* the host-resolved IOC endpoint               */
static uint8_t iocfat_ready; /* CAPS and the root probe succeeded            */
static struct zos_ipc_frame iocfat_frame;
static struct zioc_frame iocfat_request;
static struct zioc_frame iocfat_reply;
static uint8_t iocfat_name[IOCFAT_NAME_LEN];
static uint8_t iocfat_name2[IOCFAT_NAME_LEN];
/* Transfer staging and the enumeration entry, in SERVICE_IO (8000h-BFFFh). */
static uint8_t iocfat_batch[IOCFAT_BLOCK_SIZE];
static uint16_t iocfat_batch_address;

static void iocfat_zero_instance(void);
static int iocfat_leaf_op(const char *path, uint8_t count, uint8_t command);
static int iocfat_dir_open(const char *path, uint8_t count,
			   struct iocfat_dir **out);
static int iocfat_dir_read(struct iocfat_dir *dir, struct zfs_dirent *out);
static int iocfat_dir_close(struct iocfat_dir *dir);
static int iocfat_file_open(const char *path, uint16_t flags, uint8_t *object,
			    uint32_t *size);
static int iocfat_file_close(uint8_t object);
static struct iocfat_file *iocfat_file_get(uint8_t object);

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

/* Copy `n` bytes of `src` starting at `offset` into `dst` starting at
 * `at`.  Both bases arrive as parameters so SDCC emits a word relocation
 * for the global address instead of a byte-fragmented store. */
static void iocfat_copy_range(const uint8_t *src, uint16_t offset,
			      uint8_t *dst, uint16_t at, uint16_t n)
{
	uint16_t i;

	for (i = 0; i < n; ++i) {
		dst[at + i] = src[offset + i];
	}
}

static uint16_t iocfat_addr(const uint8_t *payload);
/* --- transport -------------------------------------------------------- */

/* Address a nucleus request-buffer payload word pair names (the path or the
 * reply staging the nucleus wrote for this call). */
static uint16_t iocfat_addr(const uint8_t *payload)
{
	return (uint16_t)((uint16_t)payload[0] |
			  ((uint16_t)payload[1] << 8));
}

/* One command exchange with the resident IOC service.  The request travels
 * in the 32-byte protocol frame; the reply frame the service staged is
 * bank-6 memory this driver reaches directly (one same-domain copy, 36.8).
 * Returns 0 or a negative transport error; the reply is left in
 * `iocfat_reply`. */
static int iocfat_exchange(uint8_t type, uint8_t length)
{
	const uint8_t *src;
	uint16_t address;

	/* Own-array bound: `iocfat_request.data` is indexed with the request
	 * length this driver passes (never above the frame field). */
	if (length > (uint8_t)ZIOC_PROTOCOL_REQUEST_MAX) {
		return -ZOS_EINVAL;
	}
	iocfat_frame.op = ZIOC_OP_COMMAND;
	iocfat_frame.flags = 0u;
	iocfat_frame.desc = ZOS_IPC_DESC_NONE;
	iocfat_frame.arg0 = type;
	iocfat_frame.arg1 = length;
	iocfat_copy(iocfat_frame.payload, iocfat_request.data,
		    (uint16_t)length);
	if (iocfat_api->call(iocfat_ioc, ZIOC_OP_COMMAND,
			     &iocfat_frame) != 0) {
		return -ZOS_EIO;
	}
	/* The reply frame lives in the service's staged slot: a plain bank-6
	 * access, because only bank 6 is mapped while this driver runs. */
	address = iocfat_addr(iocfat_frame.payload);
	src = (const uint8_t *)address;
	iocfat_reply.type = src[0];
	iocfat_reply.status = src[1];
	iocfat_reply.length = src[2];
	/* Own-array bound: the FS2 DATA field is 26 bytes. */
	if (iocfat_reply.length > (uint8_t)ZIOC_PROTOCOL_FRAME_DATA) {
		return -ZOS_EIO;
	}
	/* Own-array bound: the reply length is the field checked above, and
	 * the FS2 DATA field is 26 bytes. */
	iocfat_copy(iocfat_reply.data, src + 3, (uint16_t)iocfat_reply.length);
	iocfat_frame.arg0 = iocfat_reply.type;
	iocfat_frame.arg1 = iocfat_reply.length;
	if (iocfat_reply.type == FS2_RSP_UNKNOWN_CMD) {
		return -ZOS_ENOTSUP;
	}
	if (iocfat_reply.type != (uint8_t)(type | 0x80u)) {
		return -ZOS_EIO;
	}
	return 0;
}

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
 * component `index` (0-based) into `out` (11 bytes, or null when the caller
 * wants only the walk).  `*count` receives the number of components, or a
 * negative error. */
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
 * component is not pushed; it is packed into `iocfat_name` instead, and
 * `count` names the number of components the path has. */
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
 * and fixed, so they are unrolled rather than iterated; a file slot's object
 * identity is its index plus one. */
static uint8_t iocfat_file_alloc(void)
{
	if (iocfat.files[0].used == 0u) {
		iocfat.files[0].used = 1u;
		return 1u;
	}
	if (iocfat.files[1].used == 0u) {
		iocfat.files[1].used = 1u;
		return 2u;
	}
	return 0u;
}

static struct iocfat_file *iocfat_file_get(uint8_t object)
{
	if (object == 1u && iocfat.files[0].used != 0u) {
		return &iocfat.files[0];
	}
	if (object == 2u && iocfat.files[1].used != 0u) {
		return &iocfat.files[1];
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

static int iocfat_file_generation(struct iocfat_file *file)
{
	if (file->generation != iocfat.generation) {
		iocfat_cache_invalidate_all();
		return -ZOS_ESTALE;
	}
	return 0;
}

/* --- bulk transfer ---------------------------------------------------- */

/* One FS2 data transfer: the FS2_READ/FS2_WRITE command, READY validation
 * (exact length for a write, at most `length` and non-empty for a read,
 * plus direction, nonzero transfer id and the echoed offset) and the
 * matching bulk phase.  A write also awaits its DONE.  `data` is this
 * driver's SERVICE_IO buffer (the cache slot or the transfer staging).
 * Returns the transferred byte count (0 for an end-of-file read), or a
 * negative error. */
static int iocfat_transfer(uint8_t command, uint16_t token, uint32_t offset,
			   uint16_t length, uint8_t *data)
{
	uint16_t bulk;
	uint16_t ready_len;
	uint16_t i;
	int result;

	iocfat_put16(&iocfat_request.data[FS2_OFF_TOKEN], token);
	iocfat_put32(&iocfat_request.data[FS2_OFF_OFFSET], offset);
	iocfat_put16(&iocfat_request.data[FS2_OFF_LENGTH], length);
	result = iocfat_exchange(command, 8u);
	if (result != 0) {
		return result;
	}
	/* The staged bulk slot the service named in the COMMAND reply. */
	bulk = iocfat_addr(&iocfat_frame.payload[3]);
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
		const uint8_t *src;

		if (ready_len == 0u) {
			return 0; /* end of file: a successful short read */
		}
		/* Own-array bound: the copy below fills the caller's block. */
		if (ready_len > length ||
		    ready_len > (uint16_t)IOCFAT_BLOCK_SIZE ||
		    iocfat_reply.data[FS2_OFF_READY_ID] == 0u ||
		    iocfat_reply.data[FS2_OFF_READY_DIR] !=
			    FS2_DIR_MCU_TO_Z80 ||
		    iocfat_get32(&iocfat_reply.data[FS2_OFF_READY_OFF]) !=
			    offset) {
			return -ZOS_EIO;
		}
		iocfat_frame.op = ZIOC_OP_BULK_READ;
		iocfat_frame.flags = 0u;
		iocfat_frame.desc = ZOS_IPC_DESC_NONE;
		iocfat_frame.arg0 = 0u;
		iocfat_frame.arg1 = ready_len;
		if (iocfat_api->call(iocfat_ioc, ZIOC_OP_BULK_READ,
				     &iocfat_frame) != (int16_t)ready_len) {
			return -ZOS_EIO;
		}
		bulk = iocfat_addr(iocfat_frame.payload);
		src = (const uint8_t *)bulk;
		for (i = 0u; i < ready_len; ++i) {
			data[i] = src[i];
		}
		return (int)ready_len;
	}
	if (ready_len != length ||
	    ready_len > (uint16_t)IOCFAT_BLOCK_SIZE ||
	    iocfat_reply.data[FS2_OFF_READY_ID] == 0u ||
	    iocfat_reply.data[FS2_OFF_READY_DIR] != FS2_DIR_Z80_TO_MCU ||
	    iocfat_get32(&iocfat_reply.data[FS2_OFF_READY_OFF]) != offset) {
		return -ZOS_EIO;
	}
	/* The staged form: the service reads what this driver placed in its
	 * bulk buffer, which the COMMAND reply named. */
	{
		uint8_t *dst = (uint8_t *)bulk;

		for (i = 0u; i < length; ++i) {
			dst[i] = data[i];
		}
	}
	/* The transfer length is the frame's `arg1`: the COMMAND exchange
	 * left the command's own data length there, so it is set again for
	 * the bulk call (the read branch does the same with `ready_len`). */
	iocfat_frame.desc = ZOS_IPC_DESC_NONE;
	iocfat_frame.arg0 = 0u;
	iocfat_frame.arg1 = length;
	if (iocfat_api->call(iocfat_ioc, ZIOC_OP_BULK_WRITE,
			     &iocfat_frame) != (int16_t)length) {
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
/* --- files ------------------------------------------------------------ */

/* Allocate the slot, resolve the parent, send OPEN_RO/OPEN_RW (plus a
 * truncating TRUNCATE) and record the instance.  Returns 0 or a negative
 * error; *object receives the identity the nucleus stores. */
static int iocfat_file_open(const char *path, uint16_t flags, uint8_t *object,
			    uint32_t *size)
{
	const uint8_t *table = iocfat_open_table;
	struct iocfat_file *slot;
	uint8_t entry;
	uint8_t count = 0u;
	uint8_t read_only;
	uint8_t id;
	uint16_t token = 0u;
	uint32_t file_size = 0u;
	int result;

	result = iocfat_walk(path, 0xFFu, (uint8_t *)0, &count);
	if (result != 0) {
		return result;
	}
	if (count == 0u) {
		return -ZOS_EISDIR; /* the root is a directory */
	}
	/* Own-array bound: the table has 16 entries and the index is masked. */
	{
		const uint8_t *decision = iocfat_open_table;

		entry = decision[(uint8_t)((flags >> 1) & 0x0Fu)];
	}
	if (entry == FS2_OPENF_INVALID) {
		return -ZOS_EINVAL;
	}
	read_only = (uint8_t)((entry & FS2_OPENF_RO) != 0u ? 1u : 0u);
	if (read_only == 0u && iocfat.read_only != 0u) {
		return -ZOS_EROFS;
	}
	id = iocfat_file_alloc();
	if (id == 0u) {
		return -ZOS_EMFILE;
	}
	slot = iocfat_file_get(id);
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
	slot->offset = 0u;
	slot->size = file_size;
	*object = id;
	*size = file_size;
	return 0;
}

static int iocfat_file_close(uint8_t object)
{
	struct iocfat_file *handle = iocfat_file_get(object);
	int result = 0;

	if (handle == (struct iocfat_file *)0) {
		return -ZOS_EINVAL;
	}
	iocfat_cache_invalidate_file(handle);
	if (iocfat_file_generation(handle) != 0) {
		handle->used = 0u;
		return -ZOS_ESTALE;
	}
	/* close is SYNC then CLOSE. */
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

/* Read `len` bytes at the instance's position into this driver's
 * `iocfat_batch` staging and advance the position.  Returns the count or a
 * negative error. */
static int iocfat_file_read(struct iocfat_file *handle, uint16_t len)
{
	uint32_t offset = handle->offset;
	uint32_t confirmed = 0u;
	uint16_t remaining = len;

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
			break; /* short read / end of file */
		}
		n = (uint16_t)(available - within);
		if (n > remaining) {
			n = remaining;
		}
		/* Own-array bound: the copy runs inside the cache block.
		 * The chunk lands at `confirmed`: a run that crosses a
		 * block boundary appends its next chunk instead of
		 * overwriting the one before it. */
		iocfat_copy_range(iocfat.cache[0].data, within, iocfat_batch,
				  (uint16_t)confirmed, n);
		confirmed += n;
		offset += n;
		remaining = (uint16_t)(remaining - n);
	}
	handle->offset = offset;
	return (int)confirmed;
}

/* Write the staged `chunk` bytes at the instance's position.  Returns 0 or a
 * negative error. */
static int iocfat_file_write(struct iocfat_file *handle, uint16_t chunk)
{
	int result;

	if (iocfat_file_generation(handle) != 0) {
		return -ZOS_ESTALE;
	}
	if (iocfat.read_only != 0u || (handle->flags & IOCFAT_H_WRITE) == 0u) {
		return -ZOS_EROFS;
	}
	result = iocfat_transfer(FS2_CMD_WRITE, handle->token,
				 handle->offset, chunk, iocfat_batch);
	if (result < 0) {
		return result;
	}
	/* Write-through: only a confirmed DONE reaches here. */
	iocfat_cache_invalidate_file(handle);
	iocfat_cache_invalidate_other(handle);
	handle->offset += chunk;
	return 0;
}

static int iocfat_file_sync(struct iocfat_file *handle)
{
	int result;

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

/* --- metadata --------------------------------------------------------- */

static int iocfat_stat_path(const char *path, struct zfs_stat *out)
{
	uint8_t count = 0u;
	uint32_t size;
	int result;

	result = iocfat_walk(path, 0xFFu, (uint8_t *)0, &count);
	if (result != 0) {
		return result;
	}
	size = 0u;
	out->type = ZFS_TYPE_DIR; /* `/` is the filesystem root */
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

/* --- enumeration ------------------------------------------------------ */

/* Allocate the directory slot, resolve the directory, OPENDIR and store its
 * token.  Returns 0 or a negative error. */
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

/* One directory entry, or 0 at end of stream, or a negative error. */
static int iocfat_dir_read(struct iocfat_dir *dir, struct zfs_dirent *out)
{
	int result;

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

static int iocfat_dir_close(struct iocfat_dir *dir)
{
	int result = 0;

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

/* --- namespace mutations ---------------------------------------------- */

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

static int iocfat_mkdir_path(const char *path)
{
	uint8_t count = 0u;
	int result;

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

static int iocfat_unlink_path(const char *path)
{
	uint8_t count = 0u;
	int result;

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
 * one entry.  The single directory slot makes a directory this driver
 * already has open report ZOS_EMFILE. */
static int iocfat_dir_is_empty(const char *path, uint8_t count)
{
	struct iocfat_dir *dir;
	struct zfs_dirent entry;
	int result = iocfat_dir_open(path, count, &dir);

	if (result != 0) {
		return result;
	}
	result = iocfat_dir_read(dir, &entry);
	(void)iocfat_dir_close(dir);
	if (result < 0) {
		return result;
	}
	return (result == 1) ? -ZOS_ENOTEMPTY : 0;
}

static int iocfat_rmdir_path(const char *path)
{
	uint8_t count = 0u;
	int result;

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
		uint8_t unused;

		(void)iocfat_walk(a, i, iocfat_name, &unused);
		(void)iocfat_walk(b, i, iocfat_name2, &unused);
		for (k = 0u; k < (uint8_t)IOCFAT_NAME_LEN; ++k) {
			if (iocfat_name[k] != iocfat_name2[k]) {
				return 0u;
			}
		}
	}
	return 1u;
}

static int iocfat_rename_path(const char *oldpath, const char *newpath)
{
	uint8_t nold = 0u;
	uint8_t nnew = 0u;
	int result;

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

/* --- the driver dispatch ---------------------------------------------- */

/* ZFILE_OP_READ: `arg1` bytes at the instance's position into the caller's
 * buffer at `arg0`.  The descriptor is required: the nucleus always issues
 * one, and this driver names no caller address. */
static int16_t iocfat_read_op(struct zos_ipc_frame *frame)
{
	struct iocfat_file *file = iocfat_file_get(frame->payload[0]);
	uint16_t length = frame->arg1;
	uint16_t offset = frame->arg0;
	uint16_t done = 0u;

	if (file == (struct iocfat_file *)0 ||
	    frame->desc == ZOS_IPC_DESC_NONE) {
		return -ZOS_EINVAL;
	}
	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);
		int result;

		if (chunk > (uint16_t)IOCFAT_BLOCK_SIZE) {
			chunk = (uint16_t)IOCFAT_BLOCK_SIZE;
		}
		result = iocfat_file_read(file, chunk);
		if (result < 0) {
			return (done != 0u) ? (int16_t)done : (int16_t)result;
		}
		if (result == 0) {
			break; /* end of file: a short successful read */
		}
		if (iocfat_api->copy_out(frame->desc,
					 (uint16_t)(offset + done),
					 iocfat_batch_address,
					 (uint16_t)result) < 0) {
			return (done != 0u) ? (int16_t)done
					    : (int16_t)-ZOS_EINVAL;
		}
		done = (uint16_t)(done + (uint16_t)result);
		if ((uint16_t)result < chunk) {
			break;
		}
	}
	return (int16_t)done;
}

/* ZFILE_OP_WRITE: `arg1` bytes from the caller's buffer at `arg0`. */
static int16_t iocfat_write_op(struct zos_ipc_frame *frame)
{
	struct iocfat_file *file = iocfat_file_get(frame->payload[0]);
	uint16_t length = frame->arg1;
	uint16_t offset = frame->arg0;
	uint16_t done = 0u;

	if (file == (struct iocfat_file *)0 ||
	    frame->desc == ZOS_IPC_DESC_NONE) {
		return -ZOS_EINVAL;
	}
	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);
		int result;

		if (chunk > (uint16_t)IOCFAT_BLOCK_SIZE) {
			chunk = (uint16_t)IOCFAT_BLOCK_SIZE;
		}
		if (iocfat_api->copy_in(frame->desc,
					(uint16_t)(offset + done),
					iocfat_batch_address, chunk) < 0) {
			return (done != 0u) ? (int16_t)done
					    : (int16_t)-ZOS_EINVAL;
		}
		result = iocfat_file_write(file, chunk);
		if (result < 0) {
			return (done != 0u) ? (int16_t)done : (int16_t)result;
		}
		done = (uint16_t)(done + chunk);
	}
	return (int16_t)done;
}

/* ZFILE_OP_SEEK: `arg0` = whence, `payload[1..4]` = the signed offset; the
 * reply carries the new position in the same four bytes. */
static int16_t iocfat_seek_op(struct zos_ipc_frame *frame)
{
	struct iocfat_file *file = iocfat_file_get(frame->payload[0]);
	int32_t base;
	int32_t target;

	if (file == (struct iocfat_file *)0) {
		return -ZOS_EINVAL;
	}
	if (frame->arg0 == ZFS_SEEK_SET) {
		base = 0;
	} else if (frame->arg0 == ZFS_SEEK_CUR) {
		base = (int32_t)file->offset;
	} else if (frame->arg0 == ZFS_SEEK_END) {
		base = (int32_t)file->size;
	} else {
		return -ZOS_EINVAL;
	}
	target = base + (int32_t)iocfat_get32(&frame->payload[1]);
	if (target < 0) {
		return -ZOS_EINVAL;
	}
	file->offset = (uint32_t)target;
	iocfat_put32(&frame->payload[1], file->offset);
	return 0;
}

/* ZFILE_OP_META: the reply payload holds a packed `struct zfs_stat`. */
static int16_t iocfat_meta_op(struct zos_ipc_frame *frame)
{
	struct iocfat_file *file = iocfat_file_get(frame->payload[0]);
	struct zfs_stat st;
	const uint8_t *src;
	uint8_t i;

	if (file == (struct iocfat_file *)0) {
		return -ZOS_EINVAL;
	}
	st.type = ZFS_TYPE_FILE;
	st.attributes = 0u;
	st.reserved = 0u;
	st.size = file->size;
	st.ctime = ZFS_TIME_UNKNOWN;
	st.mtime = ZFS_TIME_UNKNOWN;
	src = (const uint8_t *)&st;
	for (i = 0u; i < (uint8_t)sizeof(st); ++i) {
		frame->payload[i] = src[i];
	}
	return 0;
}

/* ZFILE_OP_SYNC: flush the instance. */
static int16_t iocfat_sync_op(struct zos_ipc_frame *frame)
{
	struct iocfat_file *file = iocfat_file_get(frame->payload[0]);

	if (file == (struct iocfat_file *)0) {
		return -ZOS_EINVAL;
	}
	return (int16_t)iocfat_file_sync(file);
}

/* ZFS_OP_OPEN: the path is a nucleus request buffer the FS provider staged
 * (`payload[0..1]`), the flags are `payload[2..3]`; the reply payload holds
 * the instance identity. */
static int16_t iocfat_open_op(struct zos_ipc_frame *frame)
{
	const char *path = (const char *)iocfat_addr(frame->payload);
	uint16_t flags = (uint16_t)((uint16_t)frame->payload[2] |
				    ((uint16_t)frame->payload[3] << 8));
	uint8_t object = 0u;
	uint32_t size = 0u;
	int result = iocfat_file_open(path, flags, &object, &size);

	if (result != 0) {
		return (int16_t)result;
	}
	frame->payload[0] = object;
	return 0;
}

/* ZFS_OP_CLOSE: release the instance `payload[0]` names. */
static int16_t iocfat_close_op(struct zos_ipc_frame *frame)
{
	return (int16_t)iocfat_file_close(frame->payload[0]);
}

/* ZFS_OP_STAT: the path's metadata goes back in the reply payload. */
static int16_t iocfat_stat_op(struct zos_ipc_frame *frame)
{
	const char *path = (const char *)iocfat_addr(frame->payload);
	struct zfs_stat st;
	const uint8_t *src;
	uint8_t i;
	int result = iocfat_stat_path(path, &st);

	if (result != 0) {
		return (int16_t)result;
	}
	src = (const uint8_t *)&st;
	for (i = 0u; i < (uint8_t)sizeof(st); ++i) {
		frame->payload[i] = src[i];
	}
	return 0;
}

/* ZFS_OP_ENUM: batch entries into the caller's array (the call descriptor),
 * resuming at `payload[2..3]` (the cookie = entry index) and replying the
 * next cookie there. */
static int16_t iocfat_enum_op(struct zos_ipc_frame *frame)
{
	const char *path = (const char *)iocfat_addr(frame->payload);
	uint16_t cookie = (uint16_t)((uint16_t)frame->payload[2] |
				     ((uint16_t)frame->payload[3] << 8));
	uint16_t out_size = (uint16_t)((uint16_t)frame->payload[4] |
				       ((uint16_t)frame->payload[5] << 8));
	struct iocfat_dir *dir;
	struct zos_ns_entry entry;
	uint16_t slot = 0u;
	uint16_t skip = 0u;
	uint16_t written = 0u;
	uint8_t count = 0u;
	int result;

	if (frame->desc == ZOS_IPC_DESC_NONE ||
	    out_size < ZOS_NS_ENTRY_SIZE) {
		return -ZOS_EINVAL;
	}
	result = iocfat_walk(path, 0xFFu, (uint8_t *)0, &count);
	if (result != 0) {
		return (int16_t)result;
	}
	if (count > (uint8_t)IOCFAT_PUSH_MAX) {
		return -ZOS_EINVAL;
	}
	result = iocfat_dir_open(path, count, &dir);
	if (result != 0) {
		return (int16_t)result;
	}
	/* The cookie is a stateless entry index: skip what the caller has. */
	while (skip < cookie) {
		struct zfs_dirent seen;
		int read = iocfat_dir_read(dir, &seen);

		if (read <= 0) {
			cookie = 0u;
			(void)iocfat_dir_close(dir);
			frame->payload[2] = 0u;
			frame->payload[3] = 0u;
			return 0;
		}
		++skip;
	}
	while ((uint16_t)(slot + ZOS_NS_ENTRY_SIZE) <= out_size) {
		struct zfs_dirent file;
		uint8_t i;

		result = iocfat_dir_read(dir, &file);
		if (result <= 0) {
			break; /* end of stream */
		}
		entry.type = (file.type == ZFS_TYPE_DIR) ? 0u
							 : ZOS_TYPE_FILE;
		entry.flags = (file.type == ZFS_TYPE_DIR)
				      ? ZOS_NS_ENTRY_CONTAINER : 0u;
		entry.name_length = (uint8_t)file.name_length;
		entry.reserved = 0u;
		for (i = 0u; i < (uint8_t)ZOS_NS_NAME_MAX; ++i) {
			entry.name[i] = (i < (uint8_t)file.name_length)
						? file.name[i] : '\0';
		}
		iocfat_copy(iocfat_batch, (const uint8_t *)&entry,
			    (uint16_t)ZOS_NS_ENTRY_SIZE);
		if (iocfat_api->copy_out(frame->desc, slot,
					 iocfat_batch_address,
					 (uint16_t)ZOS_NS_ENTRY_SIZE) < 0) {
			break;
		}
		slot = (uint16_t)(slot + ZOS_NS_ENTRY_SIZE);
		++written;
		++skip;
		++cookie;
	}
	(void)iocfat_dir_close(dir);
	frame->payload[2] = (uint8_t)cookie;
	frame->payload[3] = (uint8_t)(cookie >> 8);
	return (int16_t)written;
}

int16_t iocfat_dispatch(struct zos_ipc_frame *frame)
{
	if (iocfat_ready == 0u) {
		return -ZOS_ENODEV; /* absent or unreadable card */
	}
	if (frame->flags != 0u) {
		return -ZOS_EINVAL; /* the operations define no flags */
	}
	switch (frame->op) {
	case ZFILE_OP_READ:
		return iocfat_read_op(frame);
	case ZFILE_OP_WRITE:
		return iocfat_write_op(frame);
	case ZFILE_OP_SEEK:
		return iocfat_seek_op(frame);
	case ZFILE_OP_META:
		return iocfat_meta_op(frame);
	case ZFILE_OP_SYNC:
		return iocfat_sync_op(frame);
	case ZFS_OP_OPEN:
		return iocfat_open_op(frame);
	case ZFS_OP_CLOSE:
		return iocfat_close_op(frame);
	case ZFS_OP_STAT:
		return iocfat_stat_op(frame);
	case ZFS_OP_MKDIR:
		return (int16_t)iocfat_mkdir_path(
			(const char *)iocfat_addr(frame->payload));
	case ZFS_OP_RMDIR:
		return (int16_t)iocfat_rmdir_path(
			(const char *)iocfat_addr(frame->payload));
	case ZFS_OP_UNLINK:
		return (int16_t)iocfat_unlink_path(
			(const char *)iocfat_addr(frame->payload));
	case ZFS_OP_RENAME:
		return (int16_t)iocfat_rename_path(
			(const char *)iocfat_addr(frame->payload),
			(const char *)iocfat_addr(&frame->payload[2]));
	case ZFS_OP_ENUM:
		return iocfat_enum_op(frame);
	default:
		return -ZOS_EINVAL;
	}
}

/* --- driver entry ----------------------------------------------------- */

static void iocfat_zero_instance(void)
{
	uint8_t *p = (uint8_t *)&iocfat;
	uint16_t i;

	for (i = 0u; i < (uint16_t)sizeof(struct iocfat_instance); ++i) {
		p[i] = 0u;
	}
}

void iocfat_init(const struct ipc_service_boot *boot)
{
	uint16_t chunk_max;
	uint16_t flags;
	int result;

	iocfat_api = boot->api;
	iocfat_ioc = boot->deps[IOCFAT_DEP_IOC];
	iocfat_batch_address = (uint16_t)iocfat_batch;
	iocfat_ready = 0u;
	iocfat_zero_instance();
	if (iocfat_ioc == 0u) {
		return;
	}

	/* CAPS v1: version, 2-byte tokens and a 512-byte chunk. */
	result = iocfat_exchange(FS2_CMD_CAPS, 0u);
	if (result != 0 || iocfat_reply.status != FS2_STATUS_OK ||
	    iocfat_reply.length < FS2_CAP_REPLY_LEN) {
		return;
	}
	if (iocfat_reply.data[FS2_OFF_CAP_VERSION] != FS2_CAP_VERSION ||
	    iocfat_reply.data[FS2_OFF_CAP_STATUS_VERSION] !=
		    FS2_CAP_STATUS_VERSION ||
	    iocfat_reply.data[FS2_OFF_CAP_TOKEN_BYTES] !=
		    FS2_CAP_TOKEN_BYTES) {
		return;
	}
	chunk_max = iocfat_get16(&iocfat_reply.data[FS2_OFF_CAP_CHUNK_MAX]);
	if (chunk_max < FS2_CAP_CHUNK_MIN ||
	    chunk_max > (uint16_t)IOCFAT_BLOCK_SIZE) {
		return;
	}
	flags = iocfat_get16(&iocfat_reply.data[FS2_OFF_CAP_FLAGS]);
	iocfat.read_only =
		(uint8_t)((flags & FS2_CAP_READ_ONLY) != 0u ? 1u : 0u);
	iocfat.generation =
		iocfat_get32(&iocfat_reply.data[FS2_OFF_CAP_GENERATION]);

	/* Probe the root: it must be openable on mounted media.  An absent
	 * or FAT-less card stops here, and every operation then reports
	 * -ZOS_ENODEV while `/SYSTEM` and the console stay usable. */
	{
		struct iocfat_dir *dir;

		result = iocfat_dir_open("/", 0u, &dir);
		if (result != 0) {
			return;
		}
		(void)iocfat_dir_close(dir);
	}
	iocfat.mounted = 1u;
	iocfat_ready = 1u;
}
