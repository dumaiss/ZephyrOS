/* ZephyrOS M6/M10 ZEX v2 validation and process loading.
 *
 * process_prepare() is the one loader path: it binds the executable as a
 * FILE object (architecture 36.13: exec binds the path, requires FILE and
 * streams the image through FILE calls), makes one up-front feasibility
 * check from the v2 header (geometry, segment count and slot total,
 * table/payload placement), allocates the lowest free process slot,
 * application bank and the container's segments - `DATA` segments as
 * extended-memory objects and `FAR_CODE` segments in the aperture slice
 * of their own auxiliary banks (one far-code segment per bank) - and only
 * then constructs the child's bank: the page-zero RST 30h vector at
 * 0030h, the primary image at 0100h, zero BSS, each segment's payload and
 * zero BSS.  Nothing else is pre-zeroed (36.19): there is no whole-bank
 * clear and no zeroing of xmem objects.  The kernel launcher and SYS_EXEC
 * both use it; the common launch gate (kernel/process/launch.asm) then
 * enters the child and restores the launcher on exit.
 *
 * Every failure path rolls the slot back to FREE, releases the bank, the
 * child's segments/objects and unbinds the source; a partially written
 * bank is never observable as a process because the table entry is
 * published as RUNNING only at launch time.
 *
 * All copying goes through the common-resident gates
 * (kernel/memory/user_copy.asm): the primary bank and non-aperture slots
 * through the mode-10 gate, auxiliary slots 3-6 through the mode-11
 * window copy.  The segment table and header live in bank-7 `_DATA`
 * (never on the gate stack, which is 256 bytes and shared with SYS_EXEC).
 *
 * The FILE calls are nucleus calls whose caller buffer is the common
 * staging block: the cold loader holds no descriptor and names no bank-7
 * buffer to a driver (the call machinery stages the frame in the fixed
 * boundary buffer), so a `/FS` executable living behind a bank-6 driver
 * streams exactly like a `/SYSTEM` one.
 */

#include "zephyros/process.h"
#include "zos/syscall.h"
#include "zos/object.h"
#include "zos/protocol.h"
#include "zephyros/romfs.h"
#include "kernel/ipc/ipc.h"
#include "kernel/syscall/diag.h"
#include "kernel/syscall/xmem.h"
#include "zephyros/kernel.h"
#include "kernel/vfs/vfs.h"
#include "kernel/vfs/ns.h"

/* Common-resident copy gate and the deterministic page-zero vector. */
extern const uint8_t zos_process_page_zero_vector[];

#define ZEX_LOAD_CHUNK ZOS_SYSCALL_STAGE_SIZE

/* Slots 0-2 live below the mode-11 window boundary and are copied through
 * the mode-10 gate; 6000h and above go through the window. */
#define EXEC_WINDOW_BASE 0x6000u

/* The header and segment table of the container being loaded.  They live
 * in bank-7 `_DATA`, not on the caller's stack: process_prepare() runs on
 * the 256-byte gate stack under SYS_EXEC and declares no automatic object
 * larger than 32 bytes, and the rows must stay visible for the whole load.
 * Single-tasking: exactly one load is in progress at a time. */
static struct zex_header zex_pending_header;
static struct zex_segment zex_pending_segments[ZEX_MAX_SEGMENTS];
static uint8_t zex_segment_bank[ZEX_MAX_SEGMENTS];
static uint8_t zex_segment_slot[ZEX_MAX_SEGMENTS];

#if defined(__SDCC)
_Static_assert(sizeof(zex_pending_segments) <= 256u,
	       "the segment table must stay small enough for bank-7 data");
#endif

/* One copy request against the child bank, through the common gate. */
static uint8_t exec_bank_write(uint8_t bank, uint16_t addr,
			       const uint8_t *src, uint16_t length)
{
	struct zos_bank_copy_req req;

	req.bank = bank;
	req.op = ZOS_BANK_OP_WRITE;
	req.addr = addr;
	req.buffer = (uint8_t *)src;
	req.length = length;
	return zos_bank_copy(&req);
}

static uint8_t exec_bank_zero(uint8_t bank, uint16_t addr, uint16_t length)
{
	struct zos_bank_copy_req req;

	req.bank = bank;
	req.op = ZOS_BANK_OP_ZERO;
	req.addr = addr;
	req.buffer = (uint8_t *)0;
	req.length = length;
	return zos_bank_copy(&req);
}

/* One segment-object transfer: from 6000h up through the mode-11 window
 * (the copy runs with the page field set, so only 6000h-DFFFh is
 * reachable), below that through the mode-10 gate, which shows the whole
 * bank even when a one-chunk run crosses the boundary. */
static uint8_t exec_seg_write(uint8_t bank, uint16_t addr,
			      const uint8_t *src, uint16_t length)
{
	struct zos_bank_copy_req req;

	req.bank = bank;
	req.op = ZOS_BANK_OP_WRITE;
	req.addr = addr;
	req.buffer = (uint8_t *)src;
	req.length = length;
	if (addr >= EXEC_WINDOW_BASE) {
		return zos_window_copy(&req);
	}
	return zos_bank_copy(&req);
}

/* Zero `length` bytes of a segment object.  The mode-10 gate has a ZERO
 * op; the window copy does not, so the window path writes a zeroed staging
 * block instead. */
static uint8_t exec_seg_zero(uint8_t bank, uint16_t addr, uint16_t length)
{
	uint16_t done = 0u;

	while (done < length) {
		uint16_t chunk = (uint16_t)(length - done);
		uint16_t at = (uint16_t)(addr + done);

		if (chunk > ZEX_LOAD_CHUNK) {
			chunk = ZEX_LOAD_CHUNK;
		}
		if (at >= EXEC_WINDOW_BASE) {
			uint16_t i;

			for (i = 0u; i < chunk; ++i) {
				zos_user_copy_staging[i] = 0u;
			}
			if (exec_seg_write(bank, at, zos_user_copy_staging,
					   chunk) != 0) {
				return 1;
			}
		} else if (exec_bank_zero(bank, at, chunk) != 0) {
			return 1;
		}
		done = (uint16_t)(done + chunk);
	}
	return 0;
}

/* Build startup block v1 in the common ROMFS staging view and write it
 * into the child's page zero at 0080h.
 *
 * The tail is read from the parent's bank only here, after the executable
 * source is closed, and it is never kept; its bank follows the
 * buffer-identity rule, so a tail behind the parent's aperture is read
 * from the far bank.  The one existing 128-byte common staging view (the
 * ROMFS access buffer, exactly the startup-block size and free once the
 * source is closed) is the gate buffer; the VFS workspace's staging view
 * is bank-7 data and bank 7 is not mapped while the gate runs in mode 10.
 * Nothing new is allocated and nothing is written beyond the child's page
 * zero.  The parent's bank cannot change during process_prepare().
 * Returns 0 or 1 (the caller maps it to a read error). */
static uint8_t exec_startup_block(uint8_t child_bank, uint16_t args,
				  uint16_t args_length)
{
	struct zos_bank_copy_req req;

	rom_access_staging[0] = ZOS_STARTUP_VERSION;
	rom_access_staging[1] = (uint8_t)args_length;
	if (args_length != 0) {
		req.bank = zos_syscall_buffer_bank(args);
		req.op = ZOS_BANK_OP_READ;
		req.addr = args;
		req.buffer = rom_access_staging + 2;
		req.length = args_length;
		if (zos_bank_copy(&req) != 0) {
			return 1;
		}
	}
	rom_access_staging[2u + args_length] = 0; /* always NUL-terminated */
	req.bank = child_bank;
	req.op = ZOS_BANK_OP_WRITE;
	req.addr = ZOS_STARTUP_BASE;
	req.buffer = rom_access_staging;
	req.length = (uint16_t)(2u + args_length + 1u);
	return zos_bank_copy(&req);
}

/* --- format validation ------------------------------------------------ */

/* The one up-front feasibility check: every count and size in the header,
 * validated in 32 bits before anything is allocated or written.  Segment
 * rows are checked separately before they are used. */
int zex_validate_header(const struct zex_header *header, uint32_t file_size,
			struct zex_geometry *out)
{
	uint32_t image_end;
	uint32_t bss_end;
	uint32_t stack_base;
	uint32_t table_end;

	if (header == (const struct zex_header *)0) {
		return ZEX_EXEC_ERR_ARG;
	}
	if (header->magic[0] != ZEX_MAGIC_0 ||
	    header->magic[1] != ZEX_MAGIC_1 ||
	    header->magic[2] != ZEX_MAGIC_2 ||
	    header->magic[3] != ZEX_MAGIC_3) {
		return ZEX_EXEC_ERR_MAGIC;
	}
	if (header->format_version != ZEX_FORMAT_VERSION) {
		return ZEX_EXEC_ERR_FORMAT;
	}
	if (header->abi_version != ZEX_ABI_VERSION) {
		return ZEX_EXEC_ERR_ABI;
	}
	if (header->flags != ZEX_FLAG_NONE) {
		return ZEX_EXEC_ERR_FLAGS;
	}
	if (header->reserved0 != 0) {
		return ZEX_EXEC_ERR_RESERVED0;
	}
	if (header->header_size != ZEX_HEADER_SIZE) {
		return ZEX_EXEC_ERR_HEADER;
	}
	if (header->reserved1 != 0u) {
		return ZEX_EXEC_ERR_RESERVED1;
	}
	if (header->image_size == 0) {
		return ZEX_EXEC_ERR_IMAGE;
	}
	if (file_size != header->file_size) {
		return ZEX_EXEC_ERR_FILESIZE;
	}
	if (header->entry_offset >= header->image_size) {
		return ZEX_EXEC_ERR_ENTRY;
	}
	if (header->stack_size < ZEX_MIN_STACK_SIZE) {
		return ZEX_EXEC_ERR_STACK;
	}
	if (header->segment_count > ZEX_MAX_SEGMENTS ||
	    header->total_segment_slots > ZEX_MAX_TOTAL_SLOTS) {
		return ZEX_EXEC_ERR_SEG_SLOTS;
	}

	/* The segment table and the primary payload must lie inside the
	 * file (all arithmetic 32-bit before it is used). */
	table_end = header->segment_table_offset +
		    ((uint32_t)header->segment_count *
		     ZEX_SEGMENT_ENTRY_SIZE);
	if (table_end < header->segment_table_offset ||
	    table_end > file_size) {
		return ZEX_EXEC_ERR_SEG_TABLE;
	}
	if ((uint32_t)header->primary_payload_offset + header->image_size >
	    file_size) {
		return ZEX_EXEC_ERR_FILESIZE;
	}

	/* All geometry arithmetic is checked in 32 bits before it is
	 * narrowed, so a 16-bit wrap can never be mistaken for a fit. */
	image_end = (uint32_t)ZEX_LOAD_BASE + header->image_size;
	if (image_end > ZEX_STACK_TOP) {
		return ZEX_EXEC_ERR_IMAGE_END;
	}
	bss_end = image_end + header->bss_size;
	if (bss_end > ZEX_STACK_TOP) {
		return ZEX_EXEC_ERR_BSS_END;
	}
	stack_base = (uint32_t)ZEX_STACK_TOP - header->stack_size;
	if (stack_base < ZEX_LOAD_BASE || bss_end > stack_base) {
		return ZEX_EXEC_ERR_COLLISION;
	}

	if (out != (struct zex_geometry *)0) {
		out->image_size = header->image_size;
		out->bss_size = header->bss_size;
		out->entry = (uint16_t)(ZEX_LOAD_BASE + header->entry_offset);
		out->stack_size = header->stack_size;
		out->stack_base = (uint16_t)stack_base;
	}
	return ZEX_EXEC_OK;
}

/* Protective bounds for one segment row, checked before the row is
 * allocated or copied: a known kind, a non-empty image + BSS that fits its
 * placement (a DATA segment the four non-aperture slots of one bank, a
 * FAR_CODE segment the 16 KiB aperture slice), zero reserved fields and a
 * payload inside the file.  The packer proves the same geometry on the
 * host. */
int zex_validate_segment(const struct zex_segment *segment,
			 uint32_t file_size)
{
	uint32_t total;

	if (segment == (const struct zex_segment *)0) {
		return ZEX_EXEC_ERR_ARG;
	}
	if (segment->kind != ZEX_SEG_KIND_DATA &&
	    segment->kind != ZEX_SEG_KIND_FAR_CODE) {
		return ZEX_EXEC_ERR_SEG_KIND;
	}
	if (segment->image_size == 0 || segment->reserved0 != 0u ||
	    segment->reserved1 != 0u) {
		return ZEX_EXEC_ERR_SEGMENT;
	}
	total = (uint32_t)segment->image_size + segment->bss_size;
	if (total > (segment->kind == ZEX_SEG_KIND_FAR_CODE
			     ? ZEX_SEG_MAX_FAR_SIZE
			     : ZEX_SEG_MAX_SIZE)) {
		return ZEX_EXEC_ERR_SEGMENT;
	}
	if ((uint32_t)segment->payload_offset + segment->image_size >
	    file_size) {
		return ZEX_EXEC_ERR_SEGMENT;
	}
	return ZEX_EXEC_OK;
}

/* --- loading ---------------------------------------------------------- */

/* One FILE call on the bound executable.  `length` nonzero reads that many
 * bytes into the common staging block and returns the count (the caller
 * copies them where they belong); zero is the descriptor-less form
 * (metadata, seek, sync).  Returns the call status. */
static int exec_file_call(const struct vfs_descriptor *file, uint8_t op,
			  uint8_t arg0, uint16_t arg1, uint16_t length,
			  uint8_t dir, struct zos_ipc_frame *reply)
{
	struct zos_ipc_frame frame;
	int result;

	frame.op = op;
	frame.flags = 0u;
	frame.desc = ZOS_IPC_DESC_NONE;
	frame.arg0 = arg0;
	frame.arg1 = arg1;
	frame.payload[0] = file->object;
	result = vfs_ns_file_call(file->number, file->object, &frame, length,
				  dir);
	if (reply != (struct zos_ipc_frame *)0) {
		*reply = frame;
	}
	return result;
}

/* The file's size, from the FILE metadata operation. */
static int exec_file_size(const struct vfs_descriptor *file,
			  uint32_t *size)
{
	struct zos_ipc_frame frame;
	int result;

	result = exec_file_call(file, ZFILE_OP_META, 0u, 0u, 0u,
				ZOS_IPC_DESC_NONE, &frame);
	if (result < 0) {
		return result;
	}
	*size = (uint32_t)frame.payload[4] |
		((uint32_t)frame.payload[5] << 8) |
		((uint32_t)frame.payload[6] << 16) |
		((uint32_t)frame.payload[7] << 24);
	return 0;
}

static int exec_seek(const struct vfs_descriptor *file, uint32_t offset)
{
	struct zos_ipc_frame frame;
	int result;

	frame.op = ZFILE_OP_SEEK;
	frame.flags = 0u;
	frame.desc = ZOS_IPC_DESC_NONE;
	frame.arg0 = ZFS_SEEK_SET;
	frame.arg1 = 0u;
	frame.payload[0] = file->object;
	frame.payload[1] = (uint8_t)offset;
	frame.payload[2] = (uint8_t)(offset >> 8);
	frame.payload[3] = (uint8_t)(offset >> 16);
	frame.payload[4] = (uint8_t)(offset >> 24);
	result = vfs_ns_file_call(file->number, file->object, &frame, 0u,
				  ZOS_IPC_DESC_NONE);
	return result;
}

/* Read the ZEX header into the loader's bank-7 storage through the common
 * staging block. */
static int exec_read_header(const struct vfs_descriptor *file)
{
	uint16_t i;
	int result;

	result = exec_seek(file, 0u);
	if (result != 0) {
		return result; /* provider error, passed through */
	}
	result = exec_file_call(file, ZFILE_OP_READ, 0u, ZEX_HEADER_SIZE,
				ZEX_HEADER_SIZE, ZOS_IPC_DESC_OUT,
				(struct zos_ipc_frame *)0);
	if (result < 0) {
		return result;
	}
	if (result != (int)ZEX_HEADER_SIZE) {
		return ZEX_EXEC_ERR_FILESIZE;
	}
	for (i = 0u; i < ZEX_HEADER_SIZE; ++i) {
		((uint8_t *)&zex_pending_header)[i] = zos_user_copy_staging[i];
	}
	return ZEX_EXEC_OK;
}

/* Stream `length` bytes at `file_offset` into `dst` through the staging
 * block, `chunk` at a time.  Returns a native provider error (negative) or
 * a ZEX_EXEC_ERR_* code. */
static int exec_stream(const struct vfs_descriptor *file, uint32_t offset,
		       uint8_t *dst, uint16_t length)
{
	while (length != 0u) {
		uint16_t chunk = length;
		int result;

		if (chunk > ZEX_LOAD_CHUNK) {
			chunk = ZEX_LOAD_CHUNK;
		}
		result = exec_seek(file, offset);
		if (result != 0) {
			return result;
		}
		result = exec_file_call(file, ZFILE_OP_READ, 0u, chunk, chunk,
					ZOS_IPC_DESC_OUT,
					(struct zos_ipc_frame *)0);
		if (result < 0) {
			return result;
		}
		if (result != (int)chunk) {
			return ZEX_EXEC_ERR_READ;
		}
		{
			uint16_t i;

			for (i = 0u; i < chunk; ++i) {
				dst[i] = zos_user_copy_staging[i];
			}
		}
		dst = dst + chunk;
		offset += chunk;
		length = (uint16_t)(length - chunk);
	}
	return ZEX_EXEC_OK;
}

/* Read the segment table into the loader's own storage and validate every
 * row against the header before anything is allocated: the row's bounds,
 * its ascending container id and the slot total the header promised. */
static int exec_read_segments(const struct vfs_descriptor *file,
			      const struct zex_header *header)
{
	uint16_t rows_size =
		(uint16_t)((uint16_t)header->segment_count *
			   ZEX_SEGMENT_ENTRY_SIZE);
	uint8_t slots = 0u;
	uint8_t i;
	int result;

	if (rows_size != 0u) {
		result = exec_stream(file, header->segment_table_offset,
				     (uint8_t *)zex_pending_segments,
				     rows_size);
		if (result != ZEX_EXEC_OK) {
			return result;
		}
	}
	for (i = 0u; i < header->segment_count; ++i) {
		const struct zex_segment *segment = &zex_pending_segments[i];

		result = zex_validate_segment(segment, header->file_size);
		if (result != ZEX_EXEC_OK) {
			return result;
		}
		if (segment->id != (uint8_t)(i + 1u)) {
			return ZEX_EXEC_ERR_SEGMENT;
		}
		slots = (uint8_t)(slots +
				  ((uint8_t)(((uint32_t)segment->image_size +
					      segment->bss_size +
					      (ZEX_SEG_SLOT_SIZE - 1u)) >>
					     13)));
	}
	if (slots != header->total_segment_slots) {
		return ZEX_EXEC_ERR_SEG_SLOTS;
	}
	return ZEX_EXEC_OK;
}

/* Stream the primary payload (the header payload) into the child bank at
 * 0100h. */
static int exec_load_image(const struct vfs_descriptor *file,
			   const struct zex_header *header, uint8_t bank)
{
	uint16_t offset = 0;

	while (offset < header->image_size) {
		uint16_t chunk = (uint16_t)(header->image_size - offset);
		int result;

		if (chunk > ZEX_LOAD_CHUNK) {
			chunk = ZEX_LOAD_CHUNK;
		}
		result = exec_seek(file,
				   (uint32_t)header->primary_payload_offset +
					   (uint32_t)offset);
		if (result != 0) {
			return result;
		}
		result = exec_file_call(file, ZFILE_OP_READ, 0u, chunk, chunk,
					ZOS_IPC_DESC_OUT,
					(struct zos_ipc_frame *)0);
		if (result < 0) {
			return result;
		}
		if (result != (int)chunk) {
			return ZEX_EXEC_ERR_READ;
		}
		if (exec_bank_write(bank,
				    (uint16_t)(ZEX_LOAD_BASE + offset),
				    zos_user_copy_staging, chunk) != 0) {
			return ZEX_EXEC_ERR_READ;
		}
		offset = (uint16_t)(offset + chunk);
	}
	return ZEX_EXEC_OK;
}

/* Stream one segment's payload into its bank slots (a `DATA` object or a
 * `FAR_CODE` aperture slice; both start at their slot base) and zero its
 * BSS. */
static int exec_load_segment(const struct vfs_descriptor *file,
			     const struct zex_segment *segment, uint8_t bank,
			     uint8_t slot)
{
	uint16_t base = (uint16_t)((uint16_t)slot * ZEX_SEG_SLOT_SIZE);
	uint16_t offset = 0;

	while (offset < segment->image_size) {
		uint16_t chunk = (uint16_t)(segment->image_size - offset);
		int result;

		if (chunk > ZEX_LOAD_CHUNK) {
			chunk = ZEX_LOAD_CHUNK;
		}
		result = exec_seek(file,
				   (uint32_t)segment->payload_offset +
					   (uint32_t)offset);
		if (result != 0) {
			return result;
		}
		result = exec_file_call(file, ZFILE_OP_READ, 0u, chunk, chunk,
					ZOS_IPC_DESC_OUT,
					(struct zos_ipc_frame *)0);
		if (result < 0) {
			return result;
		}
		if (result != (int)chunk) {
			return ZEX_EXEC_ERR_READ;
		}
		if (exec_seg_write(bank, (uint16_t)(base + offset),
				   zos_user_copy_staging, chunk) != 0) {
			return ZEX_EXEC_ERR_READ;
		}
		offset = (uint16_t)(offset + chunk);
	}
	if (segment->bss_size != 0u &&
	    exec_seg_zero(bank, (uint16_t)(base + segment->image_size),
			  segment->bss_size) != 0) {
		return ZEX_EXEC_ERR_READ;
	}
	return ZEX_EXEC_OK;
}

/* The loader reads forward from a fixed offset and never needs the reply
 * position, so no ZFILE_OP_SEEK/CUR/END use exists. */

/* Release the bound executable: the provider's release runs (a driver's
 * CLOSE for `/FS`, the entry itself for ROMFS), then the entry. */
static void exec_close(const struct vfs_descriptor *file, uint8_t pid)
{
	(void)vfs_ns_unbind(pid, file->number);
}

/* Every allocating and fallible step, with the executable source closed
 * before returning.  Native -ZOS_E* codes are returned for path, read and
 * allocation failures (a provider read error is passed through unchanged);
 * a ZEX_EXEC_ERR_* code reports a malformed container.  Nothing is
 * published as RUNNING. */
int process_prepare(const char *path, uint8_t parent_pid, uint16_t args,
		    uint16_t args_length, struct process_child *child)
{
	struct vfs_descriptor *file;
	struct zex_geometry geo;
	uint8_t type = 0u;
	uint8_t pid;
	uint8_t bank;
	uint8_t i;
	uint32_t file_size = 0u;
	int error;
	int result;

	if (path == (const char *)0 || child == (struct process_child *)0) {
		return -ZOS_EINVAL;
	}

	/* The path resolves through the namespace like any other name; exec
	 * requires a FILE (architecture 36.13).  The handle belongs to the
	 * running process - the launcher's or the parent's - and is
	 * released before the child is entered.
	 *
	 * The caller's copy lives in the VFS workspace's path view
	 * (SYS_EXEC stages it there, kernel/syscall.c), so the view is held
	 * for exactly the call that hands its address to a provider: a
	 * `/FS` provider reads that path while the window shows its own
	 * bank, which is why a cold-half buffer could never work.  Nothing
	 * after the bind reads the path - the loader streams the bound FILE
	 * through descriptors and the common staging block. */
	vfs_ws_enter(VFS_WS_VIEW_PATH);
	result = vfs_ns_bind(vfs_owner_current(), path,
			     (uint16_t)ZFS_O_READ, &type);
	vfs_ws_leave(VFS_WS_VIEW_PATH);
	if (result < 0) {
		return result;
	}
	if (type != ZOS_TYPE_FILE) {
		(void)vfs_ns_unbind(vfs_owner_current(), (uint8_t)result);
		return -ZOS_EISDIR;
	}
	file = vfs_handle_find(vfs_owner_current(), (uint8_t)result);
	if (file == (struct vfs_descriptor *)0) {
		return -ZOS_EINVAL;
	}
	/* The M6 media gate: an executable under `/SYSTEM` must be a ZEX
	 * entry, not DATA or a service image (the M9 provider enforced this
	 * through its exec flag, which the namespace bind does not carry).
	 * A filesystem driver's FILE has no such kind and is validated by
	 * its header alone, exactly like a card executable in M9. */
	if (file->endpoint == VFS_NS_FILE_ENDPOINT &&
	    file->u.ns.romfs.flags != ROMFS_FLAG_ZEX) {
		exec_close(file, vfs_owner_current());
		return ZEX_EXEC_ERR_TYPE;
	}
	/* The loader reads the size next (a directory cannot be bound as a
	 * FILE anyway) and requires it to match the header's file_size. */
	result = exec_file_size(file, &file_size);
	if (result != 0) {
		exec_close(file, vfs_owner_current());
		return result;
	}
	error = exec_read_header(file);
	if (error != ZEX_EXEC_OK) {
		exec_close(file, vfs_owner_current());
		return error;
	}
	/* The one up-front feasibility check (36.18): every count and size
	 * the load needs, before a slot, a bank or a byte changes. */
	result = zex_validate_header(&zex_pending_header, file_size, &geo);
	if (result != ZEX_EXEC_OK) {
		exec_close(file, vfs_owner_current());
		return result;
	}
	error = exec_read_segments(file, &zex_pending_header);
	if (error != ZEX_EXEC_OK) {
		exec_close(file, vfs_owner_current());
		return error;
	}

	/* Reserve bookkeeping before any bank content changes; every
	 * failure from here releases it through the same rollback path. */
	result = process_slot_alloc();
	if (result < 0) {
		exec_close(file, vfs_owner_current());
		return result;
	}
	pid = (uint8_t)result;
	/* Allocate the child's CWD slot before any bank content changes; a
	 * full CWD pool fails the launch cleanly and rolls the slot back
	 * through process_slot_release().  The child inherits the parent's
	 * CWD (or `/` for the kernel launcher). */
	result = vfs_process_start(pid, parent_pid);
	if (result != 0) {
		process_slot_release(pid);
		exec_close(file, vfs_owner_current());
		return result;
	}
	result = process_alloc_bank();
	if (result < 0) {
		process_slot_release(pid);
		exec_close(file, vfs_owner_current());
		return result;
	}
	bank = (uint8_t)result;
	process_table[pid].bank = bank;
	process_table[pid].parent = parent_pid;
	process_table[pid].flags = (parent_pid == PROCESS_KERNEL_PARENT)
		? PROCESS_FLAG_KERNEL_PARENT : 0;
	process_table[pid].exit_status = 0;

	/* Place the container's segments before the bank is touched
	 * (36.18): a refusal is a clean NOSPACE rollback, and a later copy
	 * failure releases them through process_slot_release() ->
	 * zos_xmem_release_process().  A DATA segment becomes the child's
	 * extended-memory object; a FAR_CODE segment takes the aperture
	 * slice of an auxiliary bank (its handle stays 0). */
	error = ZEX_EXEC_OK;
	for (i = 0u; i < zex_pending_header.segment_count; ++i) {
		const struct zex_segment *segment = &zex_pending_segments[i];
		uint16_t handle; /* SYS_SEG_INFO derives it again by id */

		if (zos_xmem_segment_alloc(pid, segment->id, segment->kind,
					   (uint16_t)(segment->image_size +
						      segment->bss_size),
					   &handle, &zex_segment_bank[i],
					   &zex_segment_slot[i]) != 0) {
			error = ZEX_EXEC_ERR_NOSPACE;
			break;
		}
	}

	/* Construct the child's bank (36.19: only what the ABI requires -
	 * the page-zero vector, the image, BSS and the startup block; no
	 * whole-bank clear). */
	if (error == ZEX_EXEC_OK &&
	    exec_bank_write(bank, 0x0030, zos_process_page_zero_vector,
			    3) != 0) {
		error = ZEX_EXEC_ERR_READ;
	}
	if (error == ZEX_EXEC_OK) {
		error = exec_load_image(file, &zex_pending_header, bank);
	}
	if (error == ZEX_EXEC_OK && geo.bss_size > 0) {
		if (exec_bank_zero(bank, (uint16_t)(ZEX_LOAD_BASE +
						     geo.image_size),
				   geo.bss_size) != 0) {
			error = ZEX_EXEC_ERR_READ;
		}
	}
	for (i = 0u; error == ZEX_EXEC_OK &&
		     i < zex_pending_header.segment_count; ++i) {
		error = exec_load_segment(file, &zex_pending_segments[i],
					  zex_segment_bank[i],
					  zex_segment_slot[i]);
	}
	if (error != ZEX_EXEC_OK) {
		process_slot_release(pid);
		exec_close(file, vfs_owner_current());
		return error;
	}

	/* The whole image is in the child bank; unbind the source before
	 * returning so no loader or provider state survives the child. */
	exec_close(file, vfs_owner_current());

	/* Startup block v1: the kernel transports the parent's raw tail
	 * byte-exact into the child's page zero (0080h).  It is built from
	 * the parent bank here, after the source is closed, and never
	 * stored anywhere.  process_prepare() runs in the caller's own
	 * context, so the running process is the parent and its bank is
	 * current; the kernel launcher passes an empty tail and never reads
	 * it.  The parent's bank cannot change during process_prepare(). */
	if (exec_startup_block(bank, args, args_length) != 0) {
		process_slot_release(pid);
		return ZEX_EXEC_ERR_READ;
	}

	child->pid = pid;
	child->bank = bank;
	child->entry = geo.entry;
	return ZEX_EXEC_OK;
}
