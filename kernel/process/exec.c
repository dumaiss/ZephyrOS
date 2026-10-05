/* ZephyrOS M6 ZEX validation and process loading.
 *
 * process_prepare() is the one loader path: it opens a ZEX, validates the
 * complete geometry before any bank content changes, allocates the lowest
 * free process slot and application bank, constructs the child's bank
 * deterministically (zero 0000h-DFFFh, page zero with exactly the RST 30h
 * vector at 0030h, image at 0100h, zero BSS), and closes the executable
 * source before returning a prepared child.  The kernel launcher and
 * SYS_EXEC both use it; the common launch gate (kernel/process/launch.asm)
 * then enters the child and restores the launcher on exit.
 *
 * Every failure path rolls the slot back to FREE and releases the bank;
 * a partially written bank is never observable as a process because the
 * table entry is published as RUNNING only at launch time.
 *
 * All copying goes through the common-resident gates
 * (kernel/memory/user_copy.asm) and the bounded staging path; bank 7 is
 * rejected by both allocators and the copy gates.
 *
 * The ZEX *source* is read through the kernel VFS file API, exactly as
 * kernel_boot_module() loads the boot modules: the loader resolves the
 * path and streams the container, so the loader never learns where the
 * bytes live.
 */

#include "zephyros/process.h"
#include "kernel/vfs/vfs.h"

/* Common-resident copy gate and the deterministic page-zero vector. */
extern const uint8_t zos_process_page_zero_vector[];

#define ZEX_LOAD_CHUNK 64u /* equals the common copy staging size */

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

/* --- format validation ------------------------------------------------ */

int zex_validate_header(const struct zex_header *header, uint32_t file_size,
			struct zex_geometry *out)
{
	uint32_t image_end;
	uint32_t bss_end;
	uint32_t stack_base;

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
	if (header->reserved1 != 0) {
		return ZEX_EXEC_ERR_RESERVED1;
	}
	if (header->image_size == 0) {
		return ZEX_EXEC_ERR_IMAGE;
	}
	if (file_size != (uint32_t)header->header_size +
			 header->image_size) {
		return ZEX_EXEC_ERR_FILESIZE;
	}
	if (header->entry_offset >= header->image_size) {
		return ZEX_EXEC_ERR_ENTRY;
	}
	if (header->stack_size < ZEX_MIN_STACK_SIZE) {
		return ZEX_EXEC_ERR_STACK;
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

/* --- loading ---------------------------------------------------------- */

/* VFS-backed source: the path is resolved and read through the kernel
 * file API, so the loader no longer knows where the bytes live. */

static int exec_read_header(struct vfs_file *file,
			    struct zex_header *header)
{
	int result;

	if (file->size < ZEX_HEADER_SIZE) {
		return ZEX_EXEC_ERR_FILESIZE;
	}
	result = vfs_seek(file, 0, ZFS_SEEK_SET, (uint32_t *)0);
	if (result != 0) {
		return result; /* provider error, passed through */
	}
	result = vfs_read(file, header, ZEX_HEADER_SIZE);
	if (result != (int)ZEX_HEADER_SIZE) {
		return result < 0 ? result : ZEX_EXEC_ERR_FILESIZE;
	}
	return ZEX_EXEC_OK;
}

/* Stream the image (header payload) into the child bank at 0100h. */
static int exec_load_image(struct vfs_file *file,
			   const struct zex_geometry *geo, uint8_t bank)
{
	uint16_t offset = 0;

	while (offset < geo->image_size) {
		uint16_t chunk = (uint16_t)(geo->image_size - offset);
		int result;

		if (chunk > ZEX_LOAD_CHUNK) {
			chunk = ZEX_LOAD_CHUNK;
		}
		result = vfs_seek(file,
				  (int32_t)ZEX_HEADER_SIZE + (int32_t)offset,
				  ZFS_SEEK_SET, (uint32_t *)0);
		if (result != 0) {
			return result;
		}
		result = vfs_read(file, zos_user_copy_staging, chunk);
		if (result != (int)chunk) {
			return result < 0 ? result : ZEX_EXEC_ERR_READ;
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

/* Every allocating and fallible step, with the executable source closed
 * before returning.  Native -ZOS_E* codes are returned for path, read and
 * allocation failures (a provider read error is passed through unchanged);
 * a ZEX_EXEC_ERR_* code reports a malformed container.  Nothing is
 * published as RUNNING. */
int process_prepare(const char *path, uint8_t parent_pid,
		    struct process_child *child)
{
	struct vfs_file file;
	struct zex_header header;
	struct zex_geometry geo;
	uint8_t pid;
	uint8_t bank;
	int error;
	int result;

	if (path == (const char *)0 || child == (struct process_child *)0) {
		return -ZOS_EINVAL;
	}

	/* VFS_O_EXEC preserves the M6 media-type gate: a file that is not an
	 * executable is refused. */
	result = vfs_open(path, (uint16_t)(ZFS_O_READ | VFS_O_EXEC), &file);
	if (result != 0) {
		return result;
	}

	error = exec_read_header(&file, &header);
	if (error != ZEX_EXEC_OK) {
		(void)vfs_close(&file);
		return error;
	}
	result = zex_validate_header(&header, file.size, &geo);
	if (result != ZEX_EXEC_OK) {
		(void)vfs_close(&file);
		return result;
	}

	/* Reserve bookkeeping before any bank content changes; every
	 * failure from here releases it through the same rollback label. */
	result = process_slot_alloc();
	if (result < 0) {
		(void)vfs_close(&file);
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
		(void)vfs_close(&file);
		return result;
	}
	result = process_alloc_bank();
	if (result < 0) {
		process_slot_release(pid);
		(void)vfs_close(&file);
		return result;
	}
	bank = (uint8_t)result;
	process_table[pid].bank = bank;
	process_table[pid].parent = parent_pid;
	process_table[pid].flags = (parent_pid == PROCESS_KERNEL_PARENT)
		? PROCESS_FLAG_KERNEL_PARENT : 0;
	process_table[pid].exit_status = 0;

	error = ZEX_EXEC_OK;
	/* Deterministic child bank: zero 0000h-DFFFh first, then install
	 * the single page-zero vector, then the image and BSS. */
	if (exec_bank_zero(bank, 0x0000, 0xE000u) != 0 ||
	    exec_bank_write(bank, 0x0030, zos_process_page_zero_vector,
			    3) != 0) {
		error = ZEX_EXEC_ERR_READ;
	}
	if (error == ZEX_EXEC_OK) {
		error = exec_load_image(&file, &geo, bank);
	}
	if (error == ZEX_EXEC_OK && geo.bss_size > 0) {
		if (exec_bank_zero(bank, (uint16_t)(ZEX_LOAD_BASE +
						     geo.image_size),
				   geo.bss_size) != 0) {
			error = ZEX_EXEC_ERR_READ;
		}
	}
	if (error != ZEX_EXEC_OK) {
		process_slot_release(pid);
		(void)vfs_close(&file);
		return error;
	}

	/* The whole image is in the child bank; release the source before
	 * returning so no loader or provider state survives the child. */
	(void)vfs_close(&file);

	child->pid = pid;
	child->bank = bank;
	child->entry = geo.entry;
	return ZEX_EXEC_OK;
}
