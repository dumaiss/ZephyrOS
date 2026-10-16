/* KonaOS IOC-FAT resident filesystem driver.
 *
 * The deployed MCU FatFs FS2 driver behind the file language
 * (architecture 4.10): this driver implements the FILE protocol
 * (`bind`/`call`/`unbind` on `/fs/...`) and the filesystem-namespace
 * protocol (`stat`, `mkdir`, `rmdir`, `unlink`, `rename`, enumeration) of
 * the `/fs` root.  The nucleus FS provider routes and forwards; it holds no
 * FAT/FS2 semantics, and this driver owns pathname meaning, file creation,
 * mutation, enumeration and backend state below `/fs`.
 *
 * Placement (architecture 5.2, 5.3): the driver is host-linked at its
 * final bank-6 addresses and its IOC transport dependency is host-resolved
 * (`depends: [{class: 5, instance: 0}]`), so it never looks a peer up at
 * run time.  Every buffer it names in a copy is `DRIVER_IO`
 * (8000h-BFFFh), and the FS2 cache and the transfer staging live there.
 * Driver-to-driver data movement is direct: the IOC driver's staged reply
 * frame and bulk buffer are bank-6 memory, so one same-domain copy reaches
 * them (4.9).
 *
 * Ownership and limits:
 *   - two file handles and one directory handle, matching the CAPS v1 slot
 *     counts, so provider exhaustion is KOS_EMFILE;
 *   - paths are packed into 11-byte upper-case 8.3 FCB names; a component
 *     FatFs cannot represent is rejected with KOS_EINVAL before any IOC
 *     traffic (the firmware's own name rules are mirrored here);
 *   - the resolver is stateless between operations: every operation sends
 *     ROOT and then one PUSH per directory component;
 *   - enumeration is stateless too: the cookie is the entry index, and a
 *     batch re-opens the directory and skips to it;
 *   - FS2 tokens never escape the driver; a bound file's identity is its
 *     slot index plus one, which the nucleus records as the handle's object
 *     (`payload[0]` of every FILE operation).
 *
 * Nucleus transport (abi/kos/protocol.h): name operations receive their path
 * in a nucleus request buffer named by `payload[0..1]` - the fixed-core VFS
 * workspace, which this driver's window always maps - and enumeration also
 * receives the caller's entry array as the call descriptor.  A card that is
 * absent or unreadable does not fail the driver's init (a required driver
 * that cannot initialize is a boot failure, and `/system` must stay
 * usable): every operation then answers (-KOS_ENODEV), the one documented
 * error for "`/fs` has no media" (docs/devices.md) - `bind`, `stat`,
 * `ns_enum`, `chdir` below `/fs` and the FILE protocol alike, whatever
 * status the real IOC returns for a command it can answer from cache
 * (`iocfat_media_readable`, iocfat_init and the root `KFS_OP_STAT`).
 *
 * All protocol constants are private to this file.
 */

#include "kos/errno.h"
#include "driver.h"
#include "ioc.h"

#include "kos/fs.h"
#include "kos/object.h"
#include "kos/protocol.h"

/* The read-path phase instrument: defined only in a KOS_IOC_PHASES build
 * (the measurement image), and the header's own note owns the clock. */
#include "ioc_phases.h"

/* --- FS2 protocol (private) ------------------------------------------- */

#define FS2_CMD_CAPS        0x30u
#define FS2_CMD_GENERATION  0x31u
#define FS2_CMD_RESET       0x32u
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

/* DATA (payload) offsets inside a `struct kioc_frame`.  The `IOC_OFF_*`
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

/* CAPS v1 required values. */
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
	uint8_t key;     /* cache identity: a file record, or uncached      */
	uint32_t next;   /* where the previous read ended (read-ahead cue)  */
};

struct iocfat_dir {
	uint8_t used;
	uint16_t token;
	uint32_t generation;
};

/* --- the block cache -------------------------------------------------
 * Blocks of files opened read-only that fit it whole, kept in this
 * driver's bulk extent
 * (iocfat_cache.asm: bank 6's boot/bulk tier, reached only through the
 * nucleus's bulk_read/bulk_write): IOCFAT_CACHE_SLOTS slots of one block,
 * then IOCFAT_CACHE_FILES path records.  A file's identity is its path as
 * opened; the record also keeps the size its last open reported, and a
 * different size at a later open drops its blocks.  Every change made
 * through this driver - an open for writing, a write, an unlink or rename
 * - and every stale-media answer drops the whole cache, so an alias (the
 * same file under another spelling) can never serve old bytes.  Reads go
 * through the ring below, IOCFAT_RING_BLOCKS consecutive blocks of one
 * file in DRIVER_IO, which the bulk receive fills and copy_out reads. */
#if KOS_IOC_PHASES
/* The measurement build carries its stamps in the same hot tier and reads
 * one block per miss (KFILE_PHASE_CTL_SINGLE), so its ring is two. */
#define IOCFAT_RING_BLOCKS    2u
#else
#define IOCFAT_RING_BLOCKS    4u
#endif
#define IOCFAT_CACHE_SLOTS    32u
#define IOCFAT_CACHE_FILES    8u
#define IOCFAT_CACHE_PATH     128u
#define IOCFAT_CACHE_PATHS_AT ((uint16_t)(IOCFAT_CACHE_SLOTS * \
					  IOCFAT_BLOCK_SIZE))
/* Keys: 1..IOCFAT_CACHE_FILES a file record, IOCFAT_KEY_UNCACHED | slot an
 * instance whose blocks stay in the ring only, 0 nothing. */
#define IOCFAT_KEY_UNCACHED   0x80u
#define IOCFAT_SLOT_NONE      0xFFu

struct iocfat_cfile {
	uint8_t length; /* the path's length; 0 = free */
	uint16_t hash;
	uint32_t size;
	uint16_t stamp;
};

struct iocfat_cslot {
	uint8_t key;    /* the file record's key; 0 = free */
	uint16_t valid;
	uint32_t block; /* the block's file offset */
	uint16_t stamp;
};

struct iocfat_instance {
	struct iocfat_file files[IOCFAT_FILE_HANDLES];
	struct iocfat_dir dirs[IOCFAT_DIR_HANDLES];
	uint32_t generation;
	uint8_t mounted;
	uint8_t read_only;
};

static struct iocfat_instance iocfat;

/* Transport state captured at init. */
static const struct driver_api *iocfat_api;
static uint8_t iocfat_ioc;   /* the host-resolved IOC endpoint               */
static uint8_t iocfat_ready; /* CAPS and the root probe succeeded            */
static struct kos_ipc_frame iocfat_frame;
static struct kioc_frame iocfat_request;
static struct kioc_frame iocfat_reply;
static uint8_t iocfat_name[IOCFAT_NAME_LEN];
static uint8_t iocfat_name2[IOCFAT_NAME_LEN];
/* The read-ahead ring, also the write and enumeration staging, in
 * DRIVER_IO (8000h-BFFFh); its address and the path buffer's are captured
 * once, as words. */
static uint8_t iocfat_ring[IOCFAT_RING_BLOCKS * IOCFAT_BLOCK_SIZE];
static uint16_t iocfat_ring_address;
static uint16_t iocfat_ring_valid_table[IOCFAT_RING_BLOCKS];
static uint16_t *iocfat_ring_valid; /* captured at init, as the tables below */
static uint32_t iocfat_ring_first; /* the first block's file offset */
static uint8_t iocfat_ring_count;
static uint8_t iocfat_ring_key;    /* 0 = empty */
static struct iocfat_cfile iocfat_cfile_table[IOCFAT_CACHE_FILES];
static struct iocfat_cslot iocfat_cslot_table[IOCFAT_CACHE_SLOTS];
static uint16_t iocfat_clock;
static uint8_t iocfat_pathbuf_storage[IOCFAT_CACHE_PATH];
static uint16_t iocfat_pathbuf_address;
/* The tables through pointers captured at init: SDCC computes an indexed
 * static array's element address a byte at a time, the relocation form
 * the container packer refuses. */
static struct iocfat_cfile *iocfat_cfiles;
static struct iocfat_cslot *iocfat_cslots;
static uint8_t *iocfat_pathbuf;
static uint8_t *iocfat_ring_bytes;

#if KOS_IOC_PHASES
/* --- read-path phase instrument (drivers/include/ioc_phases.h) --------
 * The measurement build's own table - the phases this driver owns (entry,
 * cache answer, staging copy, the copy back to the caller, return) - and
 * the IOC driver's table, whose bank-6 address every command reply carries
 * in `payload[5..6]` beside the staged frame and the bulk buffer.  The
 * delimiters are here: this driver knows when a read begins, so it clears
 * both masks then (a cache hit then has no transport phases at all), and
 * KFILE_OP_PHASES merges the peer's stamps into this one.  No shipped
 * image compiles a byte of this. */
static struct kfile_phases iocfat_phases;
static uint16_t iocfat_phase_peer;
/* KFILE_PHASE_CTL_SINGLE as the last fetch set it. */
static uint8_t iocfat_phase_single;

/* One stamp into this driver's table. */
static void iocfat_phase_note(uint8_t phase)
{
	ioc_phases_stamp(&iocfat_phases, phase);
}

/* A read begins: drop this driver's stamps and the peer's, so the table
 * the fetch op reports holds this read's steps and no earlier one's.  The
 * peer's table is bank-6 memory this driver reaches directly (it is the
 * same mapping the staged reply frame arrives through). */
static void iocfat_phase_begin(void)
{
	ioc_phases_clear(&iocfat_phases);
	if (iocfat_phase_peer != 0u) {
		((struct kfile_phases *)iocfat_phase_peer)->valid = 0u;
	}
}
#endif

static void iocfat_zero_instance(void);
static int iocfat_leaf_op(const char *path, uint8_t count, uint8_t command);
static int iocfat_dir_open(const char *path, uint8_t count,
			   struct iocfat_dir **out);
static int iocfat_dir_read(struct iocfat_dir *dir, struct kfs_dirent *out);
static int iocfat_dir_close(struct iocfat_dir *dir);
static int iocfat_media_readable(void);
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
	kos_block_copy(dst, src, n);
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

/* One command exchange with the resident IOC driver, as operation `op`
 * (KIOC_OP_COMMAND, or KIOC_OP_COMMAND_READ with the bulk receive in the
 * same call).  The request travels in the 32-byte protocol frame; the
 * reply frame the driver staged is bank-6 memory this driver reaches
 * directly (one same-domain copy, 4.9).  Returns the call's value (0, or
 * the bulk length) or a negative transport error; the reply is left in
 * `iocfat_reply`. */
static int iocfat_exchange_op(uint8_t op, uint8_t type, uint8_t length)
{
	const uint8_t *src;
	uint16_t address;
	int16_t called;

	/* Own-array bound: `iocfat_request.data` is indexed with the request
	 * length this driver passes (never above the frame field). */
	if (length > (uint8_t)KIOC_PROTOCOL_REQUEST_MAX) {
		return (-KOS_EINVAL);
	}
	iocfat_frame.op = op;
	iocfat_frame.flags = 0u;
	iocfat_frame.desc = KOS_IPC_DESC_NONE;
	iocfat_frame.arg0 = type;
	iocfat_frame.arg1 = length;
	iocfat_copy(iocfat_frame.payload, iocfat_request.data,
		    (uint16_t)length);
	called = iocfat_api->call(iocfat_ioc, op, &iocfat_frame);
	if (called < 0 || (op == KIOC_OP_COMMAND && called != 0)) {
		return (-KOS_EIO);
	}
	/* The reply frame lives in the driver's staged slot: a plain bank-6
	 * access, because only bank 6 is mapped while this driver runs. */
	address = iocfat_addr(iocfat_frame.payload);
	src = (const uint8_t *)address;
#if KOS_IOC_PHASES
	/* The measurement build's handshake, beside the staged frame: where
	 * the IOC driver's phase table is (drivers/include/ioc_phases.h). */
	iocfat_phase_peer = iocfat_addr(&iocfat_frame.payload[5]);
#endif
	iocfat_reply.type = src[0];
	iocfat_reply.status = src[1];
	iocfat_reply.length = src[2];
	/* Own-array bound: the FS2 DATA field is 26 bytes. */
	if (iocfat_reply.length > (uint8_t)KIOC_PROTOCOL_FRAME_DATA) {
		return (-KOS_EIO);
	}
	/* Own-array bound: the reply length is the field checked above, and
	 * the FS2 DATA field is 26 bytes. */
	iocfat_copy(iocfat_reply.data, src + 3, (uint16_t)iocfat_reply.length);
	iocfat_frame.arg0 = iocfat_reply.type;
	iocfat_frame.arg1 = iocfat_reply.length;
	if (iocfat_reply.type == FS2_RSP_UNKNOWN_CMD) {
		return (-KOS_ENOTSUP);
	}
	if (iocfat_reply.type != (uint8_t)(type | 0x80u)) {
		return (-KOS_EIO);
	}
	return (int)called;
}

/* A plain command exchange (KIOC_OP_COMMAND): 0 or a negative error. */
static int iocfat_exchange(uint8_t type, uint8_t length)
{
	return iocfat_exchange_op(KIOC_OP_COMMAND, type, length);
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
		return (no_handle_emfile != 0u) ? (-KOS_EMFILE) : (-KOS_ESTALE);
	}
	if (status >= FS2_STATUS_NOT_FOUND && status <= FS2_STATUS_IO) {
		return -(int)table[status - FS2_STATUS_NOT_FOUND];
	}
	return (-KOS_EIO);
}

/* --- cache ------------------------------------------------------------ */

/* Forget every cached block, every file record and the ring.  The open
 * instances keep their place but read through the ring only until they
 * are opened again. */
static void iocfat_cache_drop_all(void)
{
	uint8_t i;

	for (i = 0u; i < (uint8_t)IOCFAT_CACHE_FILES; ++i) {
		iocfat_cfiles[i].length = 0u;
	}
	for (i = 0u; i < (uint8_t)IOCFAT_CACHE_SLOTS; ++i) {
		iocfat_cslots[i].key = 0u;
	}
	for (i = 0u; i < (uint8_t)IOCFAT_FILE_HANDLES; ++i) {
		iocfat.files[i].key = (uint8_t)(IOCFAT_KEY_UNCACHED | i);
	}
	iocfat_ring_key = 0u;
	iocfat_ring_count = 0u;
}

/* Forget one file record's blocks (its record stays). */
static void iocfat_cache_drop_key(uint8_t key)
{
	uint8_t i;

	for (i = 0u; i < (uint8_t)IOCFAT_CACHE_SLOTS; ++i) {
		if (iocfat_cslots[i].key == key) {
			iocfat_cslots[i].key = 0u;
		}
	}
	if (iocfat_ring_key == key) {
		iocfat_ring_key = 0u;
	}
}

static uint16_t iocfat_tick(void)
{
	return ++iocfat_clock;
}

/* The slot caching `block` of `key`, or IOCFAT_SLOT_NONE. */
static uint8_t iocfat_cache_find(uint8_t key, uint32_t block)
{
	uint8_t i;

	for (i = 0u; i < (uint8_t)IOCFAT_CACHE_SLOTS; ++i) {
		if (iocfat_cslots[i].key == key &&
		    iocfat_cslots[i].block == block) {
			return i;
		}
	}
	return IOCFAT_SLOT_NONE;
}

/* A free slot, else the least recently used one. */
static uint8_t iocfat_cache_victim(void)
{
	uint8_t i;
	uint8_t best = 0u;

	for (i = 0u; i < (uint8_t)IOCFAT_CACHE_SLOTS; ++i) {
		if (iocfat_cslots[i].key == 0u) {
			return i;
		}
		if ((uint16_t)(iocfat_clock - iocfat_cslots[i].stamp) >
		    (uint16_t)(iocfat_clock - iocfat_cslots[best].stamp)) {
			best = i;
		}
	}
	return best;
}

/* The file record for `path` as a read-only open saw it (`size` bytes),
 * found or made; its key, or an uncached key for `slot` when the path does
 * not fit a record or the extent refuses it. */
static uint8_t iocfat_cache_file(const char *path, uint32_t size, uint8_t slot)
{
	uint8_t length = 0u;
	uint8_t i;
	uint8_t pick = 0u;
	uint16_t hash = 0u;

	while (path[length] != '\0') {
		if (++length == (uint8_t)IOCFAT_CACHE_PATH) {
			return (uint8_t)(IOCFAT_KEY_UNCACHED | slot);
		}
	}
	if (length == 0u) {
		return (uint8_t)(IOCFAT_KEY_UNCACHED | slot);
	}
	kos_block_copy(iocfat_pathbuf, path, length);
	for (i = 0u; i < length; ++i) {
		hash = (uint16_t)((uint16_t)(hash << 5) + hash + iocfat_pathbuf[i]);
	}
	/* The candidate records are checked byte for byte: the stored path is
	 * read into the ring's last block, so the ring is given up first. */
	for (i = 0u; i < (uint8_t)IOCFAT_CACHE_FILES; ++i) {
		struct iocfat_cfile *record = &iocfat_cfiles[i];
		uint8_t j;

		if (record->length != length || record->hash != hash) {
			continue;
		}
		iocfat_ring_key = 0u;
		if (iocfat_api->bulk_read((uint16_t)(IOCFAT_CACHE_PATHS_AT +
						      (uint16_t)i *
						      IOCFAT_CACHE_PATH),
					  (uint16_t)(iocfat_ring_address +
						     (IOCFAT_RING_BLOCKS - 1u) *
						     IOCFAT_BLOCK_SIZE),
					  length) < 0) {
			continue;
		}
		for (j = 0u; j < length; ++j) {
			if (iocfat_ring_bytes[(IOCFAT_RING_BLOCKS - 1u) *
					      IOCFAT_BLOCK_SIZE + j] !=
			    iocfat_pathbuf[j]) {
				break;
			}
		}
		if (j != length) {
			continue;
		}
		if (record->size != size) {
			iocfat_cache_drop_key((uint8_t)(i + 1u));
			record->size = size;
		}
		record->stamp = iocfat_tick();
		return (uint8_t)(i + 1u);
	}
	for (i = 0u; i < (uint8_t)IOCFAT_CACHE_FILES; ++i) {
		if (iocfat_cfiles[i].length == 0u) {
			pick = i;
			break;
		}
		if ((uint16_t)(iocfat_clock - iocfat_cfiles[i].stamp) >
		    (uint16_t)(iocfat_clock - iocfat_cfiles[pick].stamp)) {
			pick = i;
		}
	}
	iocfat_cache_drop_key((uint8_t)(pick + 1u));
	iocfat_cfiles[pick].length = 0u;
	if (iocfat_api->bulk_write((uint16_t)(IOCFAT_CACHE_PATHS_AT +
					       (uint16_t)pick *
					       IOCFAT_CACHE_PATH),
				   iocfat_pathbuf_address, length) < 0) {
		return (uint8_t)(IOCFAT_KEY_UNCACHED | slot);
	}
	iocfat_cfiles[pick].length = length;
	iocfat_cfiles[pick].hash = hash;
	iocfat_cfiles[pick].size = size;
	iocfat_cfiles[pick].stamp = iocfat_tick();
	return (uint8_t)(pick + 1u);
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
		return (-KOS_EINVAL);
	}
	scan = path + 1;
	while (scan[0] != '\0') {
		uint16_t length;

		if (n >= (uint8_t)IOCFAT_COMPONENT_MAX) {
			return (-KOS_EINVAL);
		}
		length = iocfat_pack(scan, (n == index) ? out : (uint8_t *)0);
		if (length == 0xFFFFu) {
			return (-KOS_EINVAL);
		}
		++n;
		scan += length;
		if (scan[0] == '/') {
			if (scan[1] == '\0') {
				return (-KOS_EINVAL); /* no trailing slash */
			}
			++scan;
		} else if (scan[0] != '\0') {
			return (-KOS_EINVAL);
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
		iocfat_cache_drop_all();
		return (-KOS_ESTALE);
	}
	return 0;
}

/* --- bulk transfer ---------------------------------------------------- */

/* One FS2 data transfer: the FS2_READ/FS2_WRITE command, READY validation
 * (exact length for a write, at most `length` and non-empty for a read,
 * plus direction, nonzero transfer id and the echoed offset) and the
 * matching bulk phase.  A write also awaits its DONE.  `data` is this
 * driver's DRIVER_IO buffer (the cache slot or the transfer staging), and
 * the IOC driver's bulk loop moves the payload straight into or out of it
 * (KIOC_OP_BULK_READ/WRITE's direct form): no staging copy.  Returns the
 * transferred byte count (0 for an end-of-file read), or a negative error. */
static int iocfat_transfer(uint8_t command, uint16_t token, uint32_t offset,
			   uint16_t length, uint8_t *data)
{
	uint16_t ready_len;
	int result;
	int bulk = 0;

	iocfat_put16(&iocfat_request.data[FS2_OFF_TOKEN], token);
	iocfat_put32(&iocfat_request.data[FS2_OFF_OFFSET], offset);
	iocfat_put16(&iocfat_request.data[FS2_OFF_LENGTH], length);
	if (command == FS2_CMD_READ) {
		/* One IOC call: the command, and the bulk receive its READY
		 * admits, straight into `data`.  The READY checks below then
		 * judge the reply; a failed one reports EIO as before, the
		 * bytes having landed in this driver's own ring. */
		iocfat_frame.payload[KIOC_COMMAND_READ_BUFFER] =
			(uint8_t)(uint16_t)data;
		iocfat_frame.payload[KIOC_COMMAND_READ_BUFFER + 1u] =
			(uint8_t)((uint16_t)data >> 8);
		bulk = iocfat_exchange_op(KIOC_OP_COMMAND_READ, command, 8u);
		result = (bulk < 0) ? bulk : 0;
	} else {
		result = iocfat_exchange(command, 8u);
	}
	if (result != 0) {
		return result;
	}
	if (iocfat_reply.status == FS2_STATUS_STALE) {
		iocfat_cache_drop_all();
		return (-KOS_ESTALE);
	}
	if (iocfat_reply.status != FS2_STATUS_OK) {
		return iocfat_map_status(iocfat_reply.status, 0u);
	}
	if (iocfat_reply.length != FS2_READY_LEN) {
		return (-KOS_EIO);
	}
	ready_len = iocfat_get16(&iocfat_reply.data[FS2_OFF_READY_LEN]);
	if (command == FS2_CMD_READ) {
		if (ready_len == 0u) {
			/* End of file: a successful short read, and no bulk
			 * phase was admitted for it. */
			return (bulk == 0) ? 0 : (-KOS_EIO);
		}
		/* Own-array bound: the bulk read filled the caller's block. */
		if (ready_len > length ||
		    ready_len > (uint16_t)IOCFAT_BLOCK_SIZE ||
		    iocfat_reply.data[FS2_OFF_READY_ID] == 0u ||
		    iocfat_reply.data[FS2_OFF_READY_DIR] !=
			    FS2_DIR_MCU_TO_Z80 ||
		    iocfat_get32(&iocfat_reply.data[FS2_OFF_READY_OFF]) !=
			    offset ||
		    bulk != (int)ready_len) {
			return (-KOS_EIO);
		}
		return (int)ready_len;
	}
	/* The direct form's buffer for the write's bulk phase. */
	iocfat_frame.payload[0] = (uint8_t)(uint16_t)data;
	iocfat_frame.payload[1] = (uint8_t)((uint16_t)data >> 8);
	if (ready_len != length ||
	    ready_len > (uint16_t)IOCFAT_BLOCK_SIZE ||
	    iocfat_reply.data[FS2_OFF_READY_ID] == 0u ||
	    iocfat_reply.data[FS2_OFF_READY_DIR] != FS2_DIR_Z80_TO_MCU ||
	    iocfat_get32(&iocfat_reply.data[FS2_OFF_READY_OFF]) != offset) {
		return (-KOS_EIO);
	}
	/* The transfer length is the frame's `arg1`: the COMMAND exchange
	 * left the command's own data length there, so it is set again for
	 * the bulk call (the read branch does the same with `ready_len`). */
	iocfat_frame.desc = KOS_IPC_DESC_NONE;
	iocfat_frame.arg0 = 0u;
	iocfat_frame.arg1 = length;
	if (iocfat_api->call(iocfat_ioc, KIOC_OP_BULK_WRITE,
			     &iocfat_frame) != (int16_t)length) {
		return (-KOS_EIO);
	}
	result = iocfat_exchange(FS2_CMD_XFER_STATUS, 0u);
	if (result != 0) {
		return (-KOS_EIO);
	}
	if (iocfat_reply.type != FS2_RSP_XFER_STATUS ||
	    iocfat_reply.length < FS2_DONE_MIN_DATA ||
	    iocfat_reply.data[FS2_OFF_DONE_STATUS] != FS2_STATUS_OK) {
		return (-KOS_EIO);
	}
	return (int)length;
}

/* Fill the ring with up to `count` consecutive blocks of `file` from the
 * 512-byte-aligned offset `block`: each from its cache slot when it has
 * one, else from the card, kept in a slot when the file is cached.  Stops
 * after a short (end-of-file) block.  Returns 0 or a negative error; a
 * failure after the first block keeps the blocks already in the ring. */
static int iocfat_ring_fill(struct iocfat_file *file, uint32_t block,
			    uint8_t count)
{
	uint8_t cached = (uint8_t)((file->key & IOCFAT_KEY_UNCACHED) == 0u);
	uint8_t i;
	uint32_t at = block;
	uint16_t dst = iocfat_ring_address;

	iocfat_ring_key = 0u;
	iocfat_ring_count = 0u;
	for (i = 0u; i < count; ++i, at += IOCFAT_BLOCK_SIZE,
	     dst = (uint16_t)(dst + IOCFAT_BLOCK_SIZE)) {
		uint8_t slot = (cached != 0u) ? iocfat_cache_find(file->key, at)
					      : IOCFAT_SLOT_NONE;
		uint16_t valid = 0u;

		if (slot != IOCFAT_SLOT_NONE) {
			valid = iocfat_cslots[slot].valid;
			if (iocfat_api->bulk_read((uint16_t)slot *
						  IOCFAT_BLOCK_SIZE, dst,
						  valid) < 0) {
				iocfat_cslots[slot].key = 0u;
				slot = IOCFAT_SLOT_NONE;
			} else {
				iocfat_cslots[slot].stamp = iocfat_tick();
			}
		}
		if (slot == IOCFAT_SLOT_NONE) {
			int result = iocfat_transfer(FS2_CMD_READ, file->token,
						     at,
						     (uint16_t)IOCFAT_BLOCK_SIZE,
						     (uint8_t *)dst);

			if (result < 0) {
				if (i == 0u) {
					return result;
				}
				break;
			}
			valid = (uint16_t)result;
			if (cached != 0u && valid != 0u) {
				slot = iocfat_cache_victim();
				iocfat_cslots[slot].key = 0u;
				if (iocfat_api->bulk_write((uint16_t)slot *
							   IOCFAT_BLOCK_SIZE,
							   dst, valid) >= 0) {
					iocfat_cslots[slot].key = file->key;
					iocfat_cslots[slot].block = at;
					iocfat_cslots[slot].valid = valid;
					iocfat_cslots[slot].stamp =
						iocfat_tick();
				}
			}
		}
		iocfat_ring_valid[i] = valid;
		iocfat_ring_count = (uint8_t)(i + 1u);
		iocfat_ring_key = file->key;
		iocfat_ring_first = block;
		if (valid < (uint16_t)IOCFAT_BLOCK_SIZE) {
			break;
		}
	}
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
		iocfat_cache_drop_all();
		return (-KOS_ESTALE);
	}
	result = iocfat_map_status(iocfat_reply.status, 1u);
	if (result != 0) {
		return result;
	}
	if (iocfat_reply.length != FS2_OPEN_REPLY_LEN) {
		return (-KOS_EIO);
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
		iocfat_cache_drop_all();
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
		return (-KOS_EISDIR); /* the root is a directory */
	}
	/* Own-array bound: the table has 16 entries and the index is masked. */
	{
		const uint8_t *decision = iocfat_open_table;

		entry = decision[(uint8_t)((flags >> 1) & 0x0Fu)];
	}
	if (entry == FS2_OPENF_INVALID) {
		return (-KOS_EINVAL);
	}
	read_only = (uint8_t)((entry & FS2_OPENF_RO) != 0u ? 1u : 0u);
	if (read_only == 0u && iocfat.read_only != 0u) {
		return (-KOS_EROFS);
	}
	id = iocfat_file_alloc();
	if (id == 0u) {
		return (-KOS_EMFILE);
	}
	slot = iocfat_file_get(id);
	result = iocfat_open_send(path, count, entry, &token, &file_size);
	if (result == (-KOS_ENOENT) && (entry & FS2_OPENF_FALLBACK) != 0u) {
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
	slot->next = 0u;
	/* A read-only open of a file the cache can hold whole joins it
	 * under its path.  A larger file would only cycle through the slots
	 * (a stream is read once), paying the store and the slot scans on
	 * every block for no later hit, so it reads through the ring alone,
	 * as a writable open does - that one has just dropped the cache
	 * (iocfat_open_send). */
	slot->key = (read_only != 0u &&
		     file_size <= (uint32_t)IOCFAT_CACHE_SLOTS *
				  IOCFAT_BLOCK_SIZE)
		? iocfat_cache_file(path, file_size, (uint8_t)(id - 1u))
		: (uint8_t)(IOCFAT_KEY_UNCACHED | (uint8_t)(id - 1u));
	*object = id;
	*size = file_size;
	return 0;
}

static int iocfat_file_close(uint8_t object)
{
	struct iocfat_file *handle = iocfat_file_get(object);
	int result = 0;

	if (handle == (struct iocfat_file *)0) {
		return (-KOS_EINVAL);
	}
	/* A cached file's blocks outlive the instance; an uncached
	 * instance's ring would be read by the next open of its slot. */
	if (iocfat_ring_key == handle->key &&
	    (handle->key & IOCFAT_KEY_UNCACHED) != 0u) {
		iocfat_ring_key = 0u;
	}
	if (iocfat_file_generation(handle) != 0) {
		handle->used = 0u;
		return (-KOS_ESTALE);
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

/* Read `len` bytes at the instance's position into the caller's buffer
 * (descriptor `desc`, from offset `at`) and advance the position past what
 * was delivered.  The bytes come from the ring, one copy_out per run of
 * consecutive blocks; a position the ring does not hold refills it - the
 * blocks the request needs, or the whole ring when the read continues
 * where the previous one ended (read-ahead).  Returns the count or a
 * negative error. */
static int iocfat_file_read(struct iocfat_file *handle, uint16_t len,
			    uint8_t desc, uint16_t at)
{
	uint32_t offset = handle->offset;
	uint16_t confirmed = 0u;
	uint16_t remaining = len;

	if (iocfat_file_generation(handle) != 0) {
		return (-KOS_ESTALE);
	}
	while (remaining != 0u) {
		uint32_t block = offset & ~((uint32_t)IOCFAT_BLOCK_SIZE - 1uL);
		uint16_t within = (uint16_t)offset & 511u;
		/* The block's place in the ring, in 16 bits: the ring spans
		 * IOCFAT_RING_BLOCKS blocks, so a held block is less than
		 * 2 KiB past its first. */
		uint32_t into = block - iocfat_ring_first;
		uint8_t held = (uint8_t)(iocfat_ring_key == handle->key &&
					 block >= iocfat_ring_first &&
					 (uint16_t)(into >> 16) == 0u &&
					 ((uint16_t)into >> 9) <
						 (uint16_t)iocfat_ring_count);
		uint8_t index;
		uint16_t run;
		uint16_t n;

#if KOS_IOC_PHASES
		/* The same test the fill below branches on, stamped before
		 * it so the phase is the decision, whatever it was. */
		iocfat_phases.flags = (uint8_t)
			((held != 0u) ? KFILE_PHASE_FLAG_CACHE_HIT : 0u);
		iocfat_phase_note(KFILE_PHASE_CACHE);
#endif
		if (held == 0u) {
			uint32_t left;
			uint16_t need = (uint16_t)((uint16_t)(within + remaining +
							      IOCFAT_BLOCK_SIZE -
							      1u) >> 9);
			uint8_t count = (need > (uint16_t)IOCFAT_RING_BLOCKS)
				? (uint8_t)IOCFAT_RING_BLOCKS : (uint8_t)need;
			int filled;

			if (block == (handle->next &
				      ~((uint32_t)IOCFAT_BLOCK_SIZE - 1uL)) ||
			    block == ((handle->next +
				       IOCFAT_BLOCK_SIZE) &
				      ~((uint32_t)IOCFAT_BLOCK_SIZE - 1uL))) {
				count = (uint8_t)IOCFAT_RING_BLOCKS;
			}
#if KOS_IOC_PHASES
			if (iocfat_phase_single != 0u) {
				count = (need > (uint16_t)IOCFAT_RING_BLOCKS)
					? (uint8_t)IOCFAT_RING_BLOCKS
					: (uint8_t)need;
			}
#endif
			if (handle->size <= block) {
				break; /* end of file */
			}
			/* Blocks left to the end of the file: a byte move and a
			 * shift, not a 32-bit division. */
			left = handle->size - block + IOCFAT_BLOCK_SIZE - 1u;
			if ((uint16_t)(left >> 16) == 0u &&
			    ((uint16_t)left >> 9) < (uint16_t)count) {
				count = (uint8_t)((uint16_t)left >> 9);
			}
			filled = iocfat_ring_fill(handle, block, count);
			if (filled != 0) {
				handle->offset = offset;
				return (confirmed != 0u) ? (int)confirmed
							 : filled;
			}
			if (iocfat_ring_count == 0u) {
				break;
			}
		}
		/* The run from `offset`: this block's bytes, then every
		 * following full block the ring holds. */
		into = block - iocfat_ring_first;
		index = (uint8_t)((uint16_t)into >> 9);
		if (within >= iocfat_ring_valid[index]) {
			break; /* short read / end of file */
		}
		run = (uint16_t)(iocfat_ring_valid[index] - within);
		while (run < remaining &&
		       iocfat_ring_valid[index] == (uint16_t)IOCFAT_BLOCK_SIZE &&
		       (uint8_t)(index + 1u) < iocfat_ring_count) {
			++index;
			run = (uint16_t)(run + iocfat_ring_valid[index]);
		}
		n = (run < remaining) ? run : remaining;
#if KOS_IOC_PHASES
		/* The payload is in this driver's buffer (the same bytes the
		 * bulk phase above, or the cache, delivered). */
		iocfat_phase_note(KFILE_PHASE_COPY);
#endif
		if (iocfat_api->copy_out(desc, (uint16_t)(at + confirmed),
					 (uint16_t)(iocfat_ring_address +
						    (uint16_t)into + within),
					 n) < 0) {
			handle->offset = offset;
			return (confirmed != 0u) ? (int)confirmed
						 : (-KOS_EINVAL);
		}
#if KOS_IOC_PHASES
		/* The caller's bytes are in its buffer: the copy path itself
		 * is the nucleus's (4.9), and the caller knows where its
		 * buffer is; the fetch op leaves that field to it. */
		iocfat_phase_note(KFILE_PHASE_CALLER);
#endif
		confirmed = (uint16_t)(confirmed + n);
		offset += n;
		remaining = (uint16_t)(remaining - n);
		if (n < run || iocfat_ring_valid[index] ==
				       (uint16_t)IOCFAT_BLOCK_SIZE) {
			continue;
		}
		break; /* the run ended in a short block: end of file */
	}
	handle->offset = offset;
	handle->next = offset;
	return (int)confirmed;
}

/* Write the staged `chunk` bytes at the instance's position.  Returns 0 or a
 * negative error. */
static int iocfat_file_write(struct iocfat_file *handle, uint16_t chunk)
{
	int result;

	if (iocfat_file_generation(handle) != 0) {
		return (-KOS_ESTALE);
	}
	if (iocfat.read_only != 0u || (handle->flags & IOCFAT_H_WRITE) == 0u) {
		return (-KOS_EROFS);
	}
	result = iocfat_transfer(FS2_CMD_WRITE, handle->token,
				 handle->offset, chunk, iocfat_ring);
	/* Write-through, and the staging was the ring: whatever the DONE
	 * said, no cached byte is trusted afterwards. */
	iocfat_cache_drop_all();
	if (result < 0) {
		return result;
	}
	handle->offset += chunk;
	return 0;
}

static int iocfat_file_sync(struct iocfat_file *handle)
{
	int result;

	if (iocfat_file_generation(handle) != 0) {
		return (-KOS_ESTALE);
	}
	iocfat_put16(&iocfat_request.data[FS2_OFF_TOKEN], handle->token);
	result = iocfat_exchange(FS2_CMD_SYNC, 2u);
	if (result != 0) {
		return result;
	}
	return iocfat_map_status(iocfat_reply.status, 0u);
}

/* --- metadata --------------------------------------------------------- */

static int iocfat_stat_path(const char *path, struct kfs_stat *out)
{
	uint8_t count = 0u;
	uint32_t size;
	int result;

	result = iocfat_walk(path, 0xFFu, (uint8_t *)0, &count);
	if (result != 0) {
		return result;
	}
	if (count == 0u) {
		/* The root is answered from the driver's own knowledge, so
		 * prove the medium first: with no card `stat("/fs")` and
		 * `chdir("/fs")` must report the same documented error as
		 * every other operation (-KOS_ENODEV, iocfat_dispatch).  A
		 * busy directory slot means an enumeration is reading the
		 * medium, which proves it just as well. */
		result = iocfat_media_readable();
		if (result < 0 && result != -KOS_EMFILE) {
			return result;
		}
	}
	size = 0u;
	out->type = KFS_TYPE_DIR; /* `/` is the filesystem root */
	if (count != 0u) {
		result = iocfat_leaf_op(path, count, FS2_CMD_STAT);
		if (result != 0) {
			return result;
		}
		if (iocfat_reply.length != FS2_STAT_REPLY_LEN) {
			return (-KOS_EIO);
		}
		if ((iocfat_reply.data[FS2_OFF_STAT_ATTR] & FS2_ATTR_DIR) == 0u) {
			out->type = KFS_TYPE_FILE;
			size = iocfat_get32(&iocfat_reply.data[FS2_OFF_STAT_SIZE]);
		}
	}
	out->attributes = 0u;
	out->reserved = 0u;
	out->size = size;
	out->ctime = KFS_TIME_UNKNOWN;
	out->mtime = KFS_TIME_UNKNOWN;
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
		return (-KOS_EMFILE);
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
		iocfat_cache_drop_all();
		return (-KOS_ESTALE);
	}
	result = iocfat_map_status(iocfat_reply.status, 1u);
	if (result != 0) {
		dir->used = 0u;
		return result;
	}
	if (iocfat_reply.length != FS2_DIR_REPLY_LEN) {
		dir->used = 0u;
		return (-KOS_EIO);
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
static int iocfat_dir_read(struct iocfat_dir *dir, struct kfs_dirent *out)
{
	int result;

	if (dir->generation != iocfat.generation) {
		iocfat_cache_drop_all();
		return (-KOS_ESTALE);
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
		iocfat_cache_drop_all();
		return (-KOS_ESTALE);
	}
	if (iocfat_reply.status != FS2_STATUS_OK) {
		return iocfat_map_status(iocfat_reply.status, 0u);
	}
	if (iocfat_reply.length != FS2_DIRENT_REPLY_LEN) {
		return (-KOS_EIO);
	}
	out->type = ((iocfat_reply.data[FS2_OFF_DIRENT_ATTR] & FS2_ATTR_DIR)
			     != 0u) ? KFS_TYPE_DIR : KFS_TYPE_FILE;
	out->attributes = 0u;
	out->size = (out->type == KFS_TYPE_DIR)
			    ? 0u
			    : iocfat_get32(&iocfat_reply.data[FS2_OFF_DIRENT_SIZE]);
	out->ctime = KFS_TIME_UNKNOWN;
	out->mtime = KFS_TIME_UNKNOWN;
	out->name_length = iocfat_unpack(
		&iocfat_reply.data[FS2_OFF_DIRENT_NAME], out->name);
	if (out->name_length < 2u) {
		return (-KOS_EIO);
	}
	return 1;
}

/* Prove the medium with the one command that reads it: open the root and
 * read one entry.  OPENDIR alone proves nothing - the deployed firmware
 * answers it for "/" from its mount context without touching the card, so
 * a volume whose card is gone still opens a root and fails only at the
 * first directory read.  Returns 0 when the volume answered (an entry or
 * the end), else the mapped error. */
static int iocfat_media_readable(void)
{
	struct iocfat_dir *dir;
	struct kfs_dirent entry;
	int result = iocfat_dir_open("/", 0u, &dir);

	if (result != 0) {
		return result;
	}
	result = iocfat_dir_read(dir, &entry);
	(void)iocfat_dir_close(dir);
	/* One entry and the end are both proof; only a negative code is a
	 * failure (NO_MEDIA for the absent card). */
	return (result < 0) ? result : 0;
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
		iocfat_cache_drop_all();
		return (-KOS_ESTALE);
	}
	return iocfat_map_status(iocfat_reply.status, 0u);
}

static int iocfat_mkdir_path(const char *path)
{
	uint8_t count = 0u;
	int result;

	if (iocfat.read_only != 0u) {
		return (-KOS_EROFS);
	}
	result = iocfat_walk(path, 0xFFu, (uint8_t *)0, &count);
	if (result != 0) {
		return result;
	}
	if (count == 0u) {
		return (-KOS_EEXIST);
	}
	return iocfat_leaf_op(path, count, FS2_CMD_MKDIR);
}

static int iocfat_unlink_path(const char *path)
{
	uint8_t count = 0u;
	int result;

	if (iocfat.read_only != 0u) {
		return (-KOS_EROFS);
	}
	result = iocfat_walk(path, 0xFFu, (uint8_t *)0, &count);
	if (result != 0) {
		return result;
	}
	if (count == 0u) {
		return (-KOS_EBUSY);
	}
	result = iocfat_leaf_op(path, count, FS2_CMD_UNLINK);
	if (result == 0) {
		/* FS2 closes every open handle on unlink; no cached byte can
		 * be trusted afterwards. */
		iocfat_cache_drop_all();
	}
	return result;
}

/* Emptiness check: open the directory through the shared path and enumerate
 * one entry.  The single directory slot makes a directory this driver
 * already has open report KOS_EMFILE. */
static int iocfat_dir_is_empty(const char *path, uint8_t count)
{
	struct iocfat_dir *dir;
	struct kfs_dirent entry;
	int result = iocfat_dir_open(path, count, &dir);

	if (result != 0) {
		return result;
	}
	result = iocfat_dir_read(dir, &entry);
	(void)iocfat_dir_close(dir);
	if (result < 0) {
		return result;
	}
	return (result == 1) ? (-KOS_ENOTEMPTY) : 0;
}

static int iocfat_rmdir_path(const char *path)
{
	uint8_t count = 0u;
	int result;

	if (iocfat.read_only != 0u) {
		return (-KOS_EROFS);
	}
	result = iocfat_walk(path, 0xFFu, (uint8_t *)0, &count);
	if (result != 0) {
		return result;
	}
	if (count == 0u) {
		return (-KOS_EBUSY);
	}
	if (count > (uint8_t)IOCFAT_PUSH_MAX) {
		return (-KOS_EINVAL);
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
		return (-KOS_EROFS);
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
		return (-KOS_EBUSY);
	}
	if (!iocfat_same_parent(oldpath, nold, newpath, nnew)) {
		return (-KOS_EXDEV);
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
	iocfat_cache_drop_all();
	return iocfat_map_status(iocfat_reply.status, 0u);
}

/* --- the driver dispatch ---------------------------------------------- */

/* KFILE_OP_READ: `arg1` bytes at the instance's position into the caller's
 * buffer at `arg0`.  The descriptor is required: the nucleus always issues
 * one, and this driver names no caller address. */
static int16_t iocfat_read_op(struct kos_ipc_frame *frame)
{
	struct iocfat_file *file = iocfat_file_get(frame->payload[0]);
	int result;

#if KOS_IOC_PHASES
	/* The read begins: this is the delimiter between one block's steps
	 * and the previous operation's (drivers/include/ioc_phases.h). */
	iocfat_phase_begin();
	iocfat_phase_note(KFILE_PHASE_ENTERED);
#endif
	if (file == (struct iocfat_file *)0 ||
	    frame->desc == KOS_IPC_DESC_NONE) {
		return (-KOS_EINVAL);
	}
	/* A short count is end of file or an error after some bytes. */
	result = iocfat_file_read(file, frame->arg1, frame->desc,
				  frame->arg0);
#if KOS_IOC_PHASES
	/* The call's own end (the success exit; an error return leaves the
	 * steps it reached). */
	if (result >= 0) {
		iocfat_phase_note(KFILE_PHASE_RETURN);
	}
#endif
	return (int16_t)result;
}

/* KFILE_OP_WRITE: `arg1` bytes from the caller's buffer at `arg0`. */
static int16_t iocfat_write_op(struct kos_ipc_frame *frame)
{
	struct iocfat_file *file = iocfat_file_get(frame->payload[0]);
	uint16_t length = frame->arg1;
	uint16_t offset = frame->arg0;
	uint16_t done = 0u;

	if (file == (struct iocfat_file *)0 ||
	    frame->desc == KOS_IPC_DESC_NONE) {
		return (-KOS_EINVAL);
	}
	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);
		int result;

		if (chunk > (uint16_t)IOCFAT_BLOCK_SIZE) {
			chunk = (uint16_t)IOCFAT_BLOCK_SIZE;
		}
		iocfat_ring_key = 0u;
		if (iocfat_api->copy_in(frame->desc,
					(uint16_t)(offset + done),
					iocfat_ring_address, chunk) < 0) {
			return (done != 0u) ? (int16_t)done
					    : (int16_t)-KOS_EINVAL;
		}
		result = iocfat_file_write(file, chunk);
		if (result < 0) {
			return (done != 0u) ? (int16_t)done : (int16_t)result;
		}
		done = (uint16_t)(done + chunk);
	}
	return (int16_t)done;
}

/* KFILE_OP_SEEK: `arg0` = whence, `payload[1..4]` = the signed offset; the
 * reply carries the new position in the same four bytes. */
static int16_t iocfat_seek_op(struct kos_ipc_frame *frame)
{
	struct iocfat_file *file = iocfat_file_get(frame->payload[0]);
	int32_t base;
	int32_t target;

	if (file == (struct iocfat_file *)0) {
		return (-KOS_EINVAL);
	}
	if (frame->arg0 == KFS_SEEK_SET) {
		base = 0;
	} else if (frame->arg0 == KFS_SEEK_CUR) {
		base = (int32_t)file->offset;
	} else if (frame->arg0 == KFS_SEEK_END) {
		base = (int32_t)file->size;
	} else {
		return (-KOS_EINVAL);
	}
	target = base + (int32_t)iocfat_get32(&frame->payload[1]);
	if (target < 0) {
		return (-KOS_EINVAL);
	}
	file->offset = (uint32_t)target;
	iocfat_put32(&frame->payload[1], file->offset);
	return 0;
}

/* KFILE_OP_META: the reply payload holds a packed `struct kfs_stat`. */
static int16_t iocfat_meta_op(struct kos_ipc_frame *frame)
{
	struct iocfat_file *file = iocfat_file_get(frame->payload[0]);
	struct kfs_stat st;

	if (file == (struct iocfat_file *)0) {
		return (-KOS_EINVAL);
	}
	st.type = KFS_TYPE_FILE;
	st.attributes = 0u;
	st.reserved = 0u;
	st.size = file->size;
	st.ctime = KFS_TIME_UNKNOWN;
	st.mtime = KFS_TIME_UNKNOWN;
	kos_block_copy(frame->payload, &st, sizeof(st));
	return 0;
}

/* KFILE_OP_SYNC: flush the instance. */
static int16_t iocfat_sync_op(struct kos_ipc_frame *frame)
{
	struct iocfat_file *file = iocfat_file_get(frame->payload[0]);

	if (file == (struct iocfat_file *)0) {
		return (-KOS_EINVAL);
	}
	return (int16_t)iocfat_file_sync(file);
}

#if KOS_IOC_PHASES
/* KFILE_OP_PHASES: the last block read's stamps, this driver's own merged
 * with the IOC driver's.  Each driver stamps the phases it owns, and the
 * peer's mask was cleared when this read began, so a cache hit simply has
 * no transport phases; the reply payload carries the packed struct, as for
 * KFILE_OP_META.  Merging is idempotent, so the fetch may be repeated. */
static int16_t iocfat_phases_op(struct kos_ipc_frame *frame)
{
	/* The merge runs on the reply payload itself, through pointers: the
	 * two bases arrive as parameters (so SDCC emits word relocations for
	 * the global addresses) and the per-phase accesses go through the payload copy, so SDCC never
	 * computes the global table's address a byte at a time - the form
	 * the container packer rejects. */
	struct kfile_phases *merged = (struct kfile_phases *)frame->payload;
	uint8_t i;

	if ((frame->arg0 & KFILE_PHASE_CTL_COLD) != 0u) {
		iocfat_cache_drop_all();
	}
	iocfat_phase_single = (uint8_t)((frame->arg0 &
					 KFILE_PHASE_CTL_SINGLE) != 0u);
	iocfat_copy(frame->payload, (const uint8_t *)&iocfat_phases,
		    (uint16_t)sizeof(struct kfile_phases));
	if (iocfat_phase_peer != 0u) {
		const struct kfile_phases *peer =
			(const struct kfile_phases *)iocfat_phase_peer;
		uint16_t valid = merged->valid;

		for (i = 0u; i < (uint8_t)KFILE_PHASE_COUNT; ++i) {
			if ((peer->valid & (uint16_t)(1u << i)) != 0u) {
				merged->elapsed_1[i] = peer->elapsed_1[i];
				merged->elapsed_2[i] = peer->elapsed_2[i];
				valid |= (uint16_t)(1u << i);
			}
		}
		merged->valid = valid;
	}
	return 0;
}
#endif

/* KFS_OP_OPEN: the path is a nucleus request buffer the FS provider staged
 * (`payload[0..1]`), the flags are `payload[2..3]`; the reply payload holds
 * the instance identity. */
static int16_t iocfat_open_op(struct kos_ipc_frame *frame)
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

/* KFS_OP_CLOSE: release the instance `payload[0]` names. */
static int16_t iocfat_close_op(struct kos_ipc_frame *frame)
{
	return (int16_t)iocfat_file_close(frame->payload[0]);
}

/* KFS_OP_STAT: the path's metadata goes back in the reply payload. */
static int16_t iocfat_stat_op(struct kos_ipc_frame *frame)
{
	const char *path = (const char *)iocfat_addr(frame->payload);
	struct kfs_stat st;
	int result = iocfat_stat_path(path, &st);

	if (result != 0) {
		return (int16_t)result;
	}
	kos_block_copy(frame->payload, &st, sizeof(st));
	return 0;
}

/* KFS_OP_ENUM: batch entries into the caller's array (the call descriptor),
 * resuming at `payload[2..3]` (the cookie = entry index) and replying the
 * next cookie there. */
static int16_t iocfat_enum_op(struct kos_ipc_frame *frame)
{
	const char *path = (const char *)iocfat_addr(frame->payload);
	uint16_t cookie = (uint16_t)((uint16_t)frame->payload[2] |
				     ((uint16_t)frame->payload[3] << 8));
	uint16_t out_size = (uint16_t)((uint16_t)frame->payload[4] |
				       ((uint16_t)frame->payload[5] << 8));
	struct iocfat_dir *dir;
	struct kos_ns_entry entry;
	uint16_t slot = 0u;
	uint16_t skip = 0u;
	uint16_t written = 0u;
	uint8_t count = 0u;
	int result;

	if (frame->desc == KOS_IPC_DESC_NONE ||
	    out_size < KOS_NS_ENTRY_SIZE) {
		return (-KOS_EINVAL);
	}
	result = iocfat_walk(path, 0xFFu, (uint8_t *)0, &count);
	if (result != 0) {
		return (int16_t)result;
	}
	if (count > (uint8_t)IOCFAT_PUSH_MAX) {
		return (-KOS_EINVAL);
	}
	result = iocfat_dir_open(path, count, &dir);
	if (result != 0) {
		return (int16_t)result;
	}
	/* The cookie is a stateless entry index: skip what the caller has. */
	while (skip < cookie) {
		struct kfs_dirent seen;
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
	while ((uint16_t)(slot + KOS_NS_ENTRY_SIZE) <= out_size) {
		struct kfs_dirent file;
		uint8_t i;

		result = iocfat_dir_read(dir, &file);
		if (result <= 0) {
			break; /* end of stream */
		}
		entry.type = (file.type == KFS_TYPE_DIR) ? 0u
							 : KOS_TYPE_FILE;
		entry.flags = (file.type == KFS_TYPE_DIR)
				      ? KOS_NS_ENTRY_CONTAINER : 0u;
		entry.name_length = (uint8_t)file.name_length;
		entry.reserved = 0u;
		for (i = 0u; i < (uint8_t)KOS_NS_NAME_MAX; ++i) {
			entry.name[i] = (i < (uint8_t)file.name_length)
						? file.name[i] : '\0';
		}
		iocfat_ring_key = 0u;
		iocfat_copy(iocfat_ring, (const uint8_t *)&entry,
			    (uint16_t)KOS_NS_ENTRY_SIZE);
		if (iocfat_api->copy_out(frame->desc, slot,
					 iocfat_ring_address,
					 (uint16_t)KOS_NS_ENTRY_SIZE) < 0) {
			break;
		}
		slot = (uint16_t)(slot + KOS_NS_ENTRY_SIZE);
		++written;
		++skip;
		++cookie;
	}
	(void)iocfat_dir_close(dir);
	frame->payload[2] = (uint8_t)cookie;
	frame->payload[3] = (uint8_t)(cookie >> 8);
	return (int16_t)written;
}

int16_t iocfat_dispatch(struct kos_ipc_frame *frame)
{
	if (iocfat_ready == 0u) {
		return (-KOS_ENODEV); /* absent or unreadable card */
	}
	if (frame->flags != 0u) {
		return (-KOS_EINVAL); /* the operations define no flags */
	}
	switch (frame->op) {
	case KFILE_OP_READ:
		return iocfat_read_op(frame);
	case KFILE_OP_WRITE:
		return iocfat_write_op(frame);
	case KFILE_OP_SEEK:
		return iocfat_seek_op(frame);
	case KFILE_OP_META:
		return iocfat_meta_op(frame);
	case KFILE_OP_SYNC:
		return iocfat_sync_op(frame);
#if KOS_IOC_PHASES
	case KFILE_OP_PHASES:
		return iocfat_phases_op(frame);
#endif
	case KFS_OP_OPEN:
		return iocfat_open_op(frame);
	case KFS_OP_CLOSE:
		return iocfat_close_op(frame);
	case KFS_OP_STAT:
		return iocfat_stat_op(frame);
	case KFS_OP_MKDIR:
		return (int16_t)iocfat_mkdir_path(
			(const char *)iocfat_addr(frame->payload));
	case KFS_OP_RMDIR:
		return (int16_t)iocfat_rmdir_path(
			(const char *)iocfat_addr(frame->payload));
	case KFS_OP_UNLINK:
		return (int16_t)iocfat_unlink_path(
			(const char *)iocfat_addr(frame->payload));
	case KFS_OP_RENAME:
		return (int16_t)iocfat_rename_path(
			(const char *)iocfat_addr(frame->payload),
			(const char *)iocfat_addr(&frame->payload[2]));
	case KFS_OP_ENUM:
		return iocfat_enum_op(frame);
	default:
		return (-KOS_EINVAL);
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

uint8_t iocfat_init(const struct driver_boot *boot)
{
	uint16_t chunk_max;
	uint16_t flags;
	int result;

	iocfat_api = boot->api;
	iocfat_ioc = boot->deps[IOCFAT_DEP_IOC];
	iocfat_ring_address = (uint16_t)iocfat_ring;
	iocfat_ring_bytes = iocfat_ring;
	iocfat_ring_valid = iocfat_ring_valid_table;
	iocfat_cfiles = iocfat_cfile_table;
	iocfat_cslots = iocfat_cslot_table;
	iocfat_pathbuf = iocfat_pathbuf_storage;
	iocfat_pathbuf_address = (uint16_t)iocfat_pathbuf_storage;
	iocfat_ready = 0u;
#if KOS_IOC_PHASES
	iocfat_phase_peer = 0u;
#endif
	iocfat_zero_instance();
	iocfat_cache_drop_all();
	if (iocfat_ioc == 0u) {
		/* A malformed bundle: the required IOC dependency is
		 * missing, so the driver cannot serve even an absent card
		 * (the loader fails the boot, 5.3). */
		return 1u;
	}

	/* Drop whatever the controller kept from the previous session: an
	 * open file slot outlives a Z80 restart, and a slot this driver
	 * never closes would make the card look full after a reboot.  The
	 * reset is the same command a media change answers, so the driver
	 * starts from the state CAPS then describes. */
	(void)iocfat_exchange(FS2_CMD_RESET, 0u);

	/* CAPS v1: version, 2-byte tokens and a 512-byte chunk. */
	result = iocfat_exchange(FS2_CMD_CAPS, 0u);
	if (result != 0 || iocfat_reply.status != FS2_STATUS_OK ||
	    iocfat_reply.length < FS2_CAP_REPLY_LEN) {
		return 0u;
	}
	if (iocfat_reply.data[FS2_OFF_CAP_VERSION] != FS2_CAP_VERSION ||
	    iocfat_reply.data[FS2_OFF_CAP_STATUS_VERSION] !=
		    FS2_CAP_STATUS_VERSION ||
	    iocfat_reply.data[FS2_OFF_CAP_TOKEN_BYTES] !=
		    FS2_CAP_TOKEN_BYTES) {
		return 0u;
	}
	chunk_max = iocfat_get16(&iocfat_reply.data[FS2_OFF_CAP_CHUNK_MAX]);
	if (chunk_max < FS2_CAP_CHUNK_MIN ||
	    chunk_max > (uint16_t)IOCFAT_BLOCK_SIZE) {
		return 0u;
	}
	flags = iocfat_get16(&iocfat_reply.data[FS2_OFF_CAP_FLAGS]);
	iocfat.read_only =
		(uint8_t)((flags & FS2_CAP_READ_ONLY) != 0u ? 1u : 0u);
	iocfat.generation =
		iocfat_get32(&iocfat_reply.data[FS2_OFF_CAP_GENERATION]);

	/* Probe the media with the read that proves it (iocfat_media_readable).
	 * A card that is absent or was pulled still answers OPENDIR for the
	 * root, so without the read "no media" would mean different things on
	 * the bench and in the simulator.  Every operation then reports the
	 * one documented error, -KOS_ENODEV (iocfat_dispatch), while
	 * `/system` and the console stay usable. */
	if (iocfat_media_readable() != 0) {
		return 0u;
	}
	iocfat.mounted = 1u;
	iocfat_ready = 1u;
	return 0u;
}
