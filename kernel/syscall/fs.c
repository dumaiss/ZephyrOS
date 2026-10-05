/* ZephyrOS M9 native filesystem syscall services (package 27).
 *
 * Wires the pinned `FS_*` family (10h-1Fh, architecture section 32.26) to
 * the kernel VFS.  Each service:
 *   - refuses before VFS READY with ZOS_EBUSY;
 *   - validates its complete argument block, the caller-window path and
 *     the user-buffer window before any state change;
 *   - copies paths into the shared static pathname scratch and resolves
 *     them against the caller's CWD (inside the VFS);
 *   - moves file data through the single 512-byte bank-7 staging buffer,
 *     bridging to/from the caller bank through the checked 64-byte common
 *     copy gate;
 *   - verifies descriptor range/type/owner for every fd-taking call.
 *
 * A read/write that fails after at least one confirmed chunk returns the
 * confirmed byte count (a short successful transfer); a failure with no
 * confirmed bytes returns the error.  No function declares an automatic
 * object larger than 32 bytes.
 */

#include "zephyros/syscall.h"
#include "zephyros/fs.h"
#include "zephyros/process.h"

#include "kernel/vfs/vfs.h"

#include "diag.h"
#include "fs.h"

/* The single 512-byte bank-7 transfer staging buffer (architecture section
 * 32.29 / work/M9-REFERENCE.md).  It is also the scratch a stat/readdir
 * result is built in before it is gated out to the caller. */
static uint8_t fs_staging[512];

/* Shared static pathname scratch: `rename` is the only operation that
 * needs two paths at once, so it owns the second buffer. */
static char fs_path1[ZOS_PATH_MAX];
static char fs_path2[ZOS_PATH_MAX];

/* --- helpers ---------------------------------------------------------- */

static uint8_t fs_ready(void)
{
	return (uint8_t)(vfs_phase() == VFS_PHASE_READY);
}

static uint8_t fs_errno_of(int result)
{
	/* -ZOS_E* -> ZOS_E* without an SDCC NEG: unsigned 16-bit wraparound
	 * subtraction of the two's-complement result. */
	return (uint8_t)(uint16_t)(0u - (uint16_t)result);
}

static void fs_publish(int result)
{
	if (result < 0) {
		zos_syscall_error(fs_errno_of(result));
		return;
	}
	zos_syscall_success(0);
}

/* Little-endian 16-bit field at `offset` in the caller-window block. */
static uint16_t fs_le16(uint16_t block, uint8_t offset)
{
	const uint8_t *p = (const uint8_t *)block;

	return (uint16_t)(p[offset] | ((uint16_t)p[offset + 1] << 8));
}

/* Copy `length` caller-window bytes into `out`, NUL-terminated.  A length
 * of 0, an over-long path, a window violation or an embedded NUL is
 * ZOS_EINVAL (work/M9-REFERENCE.md). */
static int fs_copy_path(uint16_t addr, uint16_t length, char *out)
{
	uint16_t i;

	if (length == 0 || length >= ZOS_PATH_MAX) {
		return -ZOS_EINVAL;
	}
	if (!zos_syscall_block_ok(addr, length)) {
		return -ZOS_EINVAL;
	}
	for (i = 0; i < length; ++i) {
		char c = ((const char *)addr)[i];

		if (c == '\0') {
			return -ZOS_EINVAL;
		}
		out[i] = c;
	}
	out[length] = '\0';
	return 0;
}

/* Fill fs_staging[off..off+len) from `bank`:user `uaddr` through the
 * 64-byte common gate.  Returns 0 or -ZOS_EFAULT. */
static int fs_stage_in(uint8_t bank, uint16_t uaddr, uint16_t len,
		       uint16_t off)
{
	uint16_t done = 0;

	while (done < len) {
		uint16_t chunk = (uint16_t)(len - done);
		uint16_t i;

		if (chunk > ZOS_SYSCALL_STAGE_SIZE) {
			chunk = ZOS_SYSCALL_STAGE_SIZE;
		}
		if (zos_syscall_bank_read(bank, (uint16_t)(uaddr + done),
					  chunk) != 0) {
			return -ZOS_EFAULT;
		}
		for (i = 0; i < chunk; ++i) {
			fs_staging[off + done + i] = zos_user_copy_staging[i];
		}
		done = (uint16_t)(done + chunk);
	}
	return 0;
}

/* Inverse of fs_stage_in(). */
static int fs_stage_out(uint8_t bank, uint16_t uaddr, uint16_t len,
			uint16_t off)
{
	uint16_t done = 0;

	while (done < len) {
		uint16_t chunk = (uint16_t)(len - done);
		uint16_t i;

		if (chunk > ZOS_SYSCALL_STAGE_SIZE) {
			chunk = ZOS_SYSCALL_STAGE_SIZE;
		}
		for (i = 0; i < chunk; ++i) {
			zos_user_copy_staging[i] =
				fs_staging[off + done + i];
		}
		if (zos_syscall_bank_write(bank, (uint16_t)(uaddr + done),
					   chunk) != 0) {
			return -ZOS_EFAULT;
		}
		done = (uint16_t)(done + chunk);
	}
	return 0;
}

/* --- services --------------------------------------------------------- */

static void fs_open(uint16_t block)
{
	int result;

	if (!zos_syscall_block_ok(block, 5)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	result = fs_copy_path(fs_le16(block, 0), fs_le16(block, 2), fs_path1);
	if (result != 0) {
		fs_publish(result);
		return;
	}
	result = vfs_fd_open(process_current_pid(), VFS_DESC_FILE, fs_path1,
			     (uint16_t)((const uint8_t *)block)[4]);
	if (result < 0) {
		fs_publish(result);
		return;
	}
	zos_syscall_success((uint16_t)result);
}

static void fs_opendir(uint16_t block)
{
	int result;

	if (!zos_syscall_block_ok(block, 4)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	result = fs_copy_path(fs_le16(block, 0), fs_le16(block, 2), fs_path1);
	if (result != 0) {
		fs_publish(result);
		return;
	}
	result = vfs_fd_opendir(process_current_pid(), fs_path1);
	if (result < 0) {
		fs_publish(result);
		return;
	}
	zos_syscall_success((uint16_t)result);
}

static void fs_close_svc(uint16_t block)
{
	uint8_t fd;

	if (!zos_syscall_block_ok(block, 1)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	fd = ((const uint8_t *)block)[0];
	if (vfs_fd_check(fd, VFS_DESC_FILE, process_current_pid()) ==
	    (struct vfs_descriptor *)0) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	fs_publish(vfs_fd_close(fd));
}

static void fs_closedir_svc(uint16_t block)
{
	uint8_t fd;

	if (!zos_syscall_block_ok(block, 1)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	fd = ((const uint8_t *)block)[0];
	if (vfs_fd_check(fd, VFS_DESC_DIRECTORY, process_current_pid()) ==
	    (struct vfs_descriptor *)0) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	fs_publish(vfs_fd_closedir(fd));
}

/* READ/WRITE share the io block shape; `write` selects the direction. */
static void fs_io(uint8_t bank, uint16_t block, uint8_t write)
{
	struct vfs_descriptor *desc;
	uint8_t fd;
	uint16_t buffer;
	uint16_t length;
	uint16_t confirmed = 0;

	if (!zos_syscall_block_ok(block, 5)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	fd = ((const uint8_t *)block)[0];
	buffer = fs_le16(block, 1);
	length = fs_le16(block, 3);
	desc = vfs_fd_check(fd, VFS_DESC_FILE, process_current_pid());
	if (desc == (struct vfs_descriptor *)0) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	if (!zos_syscall_buffer_ok(buffer, length)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	while (confirmed < length) {
		uint16_t chunk = (uint16_t)(length - confirmed);
		int result;

		if (chunk > (uint16_t)sizeof(fs_staging)) {
			chunk = (uint16_t)sizeof(fs_staging);
		}
		if (write != 0) {
			result = fs_stage_in(bank, (uint16_t)(buffer + confirmed),
					     chunk, 0);
			if (result == 0) {
				result = vfs_write(&desc->u.file, fs_staging,
						   chunk);
			}
		} else {
			result = vfs_read(&desc->u.file, fs_staging, chunk);
			if (result > 0) {
				uint16_t moved = (uint16_t)result;

				result = fs_stage_out(
					bank,
					(uint16_t)(buffer + confirmed),
					moved, 0);
				if (result != 0) {
					if (confirmed != 0) {
						zos_syscall_success(
							confirmed);
					} else {
						zos_syscall_error(
							fs_errno_of(result));
					}
					return;
				}
				confirmed = (uint16_t)(confirmed + moved);
				continue;
			}
		}
		if (result < 0) {
			if (confirmed != 0) {
				zos_syscall_success(confirmed);
			} else {
				zos_syscall_error(fs_errno_of(result));
			}
			return;
		}
		if (write == 0) {
			break; /* clean EOF (result == 0) */
		}
		confirmed = (uint16_t)(confirmed + chunk);
	}
	zos_syscall_success(confirmed);
}

static void fs_seek(uint16_t block)
{
	struct vfs_descriptor *desc;
	uint32_t position = 0;
	uint8_t fd;
	uint8_t whence;
	int32_t offset;
	int result;

	if (!zos_syscall_block_ok(block, 6)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	fd = ((const uint8_t *)block)[0];
	whence = ((const uint8_t *)block)[1];
	offset = (int32_t)((uint32_t)fs_le16(block, 2) |
			   ((uint32_t)fs_le16(block, 4) << 16));
	desc = vfs_fd_check(fd, VFS_DESC_FILE, process_current_pid());
	if (desc == (struct vfs_descriptor *)0) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	result = vfs_seek(&desc->u.file, offset, whence, &position);
	if (result != 0) {
		fs_publish(result);
		return;
	}
	((uint8_t *)block)[2] = (uint8_t)(position & 0xFFu);
	((uint8_t *)block)[3] = (uint8_t)((position >> 8) & 0xFFu);
	((uint8_t *)block)[4] = (uint8_t)((position >> 16) & 0xFFu);
	((uint8_t *)block)[5] = (uint8_t)((position >> 24) & 0xFFu);
	zos_syscall_success(0);
}

static void fs_stat(uint8_t bank, uint16_t block)
{
	struct zfs_stat st;
	uint16_t out;
	const uint8_t *src;
	uint16_t i;
	int result;

	if (!zos_syscall_block_ok(block, 6)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	out = fs_le16(block, 4);
	if (!zos_syscall_buffer_ok(out, (uint16_t)sizeof(st))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	result = fs_copy_path(fs_le16(block, 0), fs_le16(block, 2), fs_path1);
	if (result != 0) {
		fs_publish(result);
		return;
	}
	result = vfs_stat(fs_path1, &st);
	if (result != 0) {
		fs_publish(result);
		return;
	}
	src = (const uint8_t *)&st;
	for (i = 0; i < (uint16_t)sizeof(st); ++i) {
		fs_staging[i] = src[i];
	}
	result = fs_stage_out(bank, out, (uint16_t)sizeof(st), 0);
	if (result != 0) {
		fs_publish(result);
		return;
	}
	zos_syscall_success(0);
}

static void fs_readdir(uint8_t bank, uint16_t block)
{
	struct vfs_descriptor *desc;
	struct zfs_dirent *dirent = (struct zfs_dirent *)fs_staging;
	uint8_t fd;
	uint16_t out;
	int result;

	if (!zos_syscall_block_ok(block, 3)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	fd = ((const uint8_t *)block)[0];
	out = fs_le16(block, 1);
	desc = vfs_fd_check(fd, VFS_DESC_DIRECTORY, process_current_pid());
	if (desc == (struct vfs_descriptor *)0) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	if (!zos_syscall_buffer_ok(out, (uint16_t)sizeof(struct zfs_dirent))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	result = vfs_readdir(&desc->u.dir, dirent);
	if (result < 0) {
		fs_publish(result);
		return;
	}
	if (result == 0) {
		zos_syscall_success(0); /* end of directory */
		return;
	}
	result = fs_stage_out(bank, out, (uint16_t)sizeof(struct zfs_dirent),
			      0);
	if (result != 0) {
		fs_publish(result);
		return;
	}
	zos_syscall_success(1);
}

static void fs_getcwd(uint8_t bank, uint16_t block)
{
	uint16_t buffer;
	uint16_t length;
	uint16_t n;
	int result;

	if (!zos_syscall_block_ok(block, 4)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	buffer = fs_le16(block, 0);
	length = fs_le16(block, 2);
	result = vfs_getcwd(fs_path1, length);
	if (result < 0) {
		fs_publish(result);
		return;
	}
	n = (uint16_t)result;
	if (!zos_syscall_buffer_ok(buffer, (uint16_t)(n + 1))) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	/* Stage the NUL-terminated CWD (vfs_getcwd wrote it into fs_path1)
	 * before gating it out to the caller. */
	{
		uint16_t i;

		for (i = 0; i <= n; ++i) {
			fs_staging[i] = (uint8_t)fs_path1[i];
		}
	}
	result = fs_stage_out(bank, buffer, (uint16_t)(n + 1), 0);
	if (result != 0) {
		fs_publish(result);
		return;
	}
	zos_syscall_success(n);
}

static void fs_chdir(uint16_t block)
{
	int result;

	if (!zos_syscall_block_ok(block, 4)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	result = fs_copy_path(fs_le16(block, 0), fs_le16(block, 2), fs_path1);
	if (result != 0) {
		fs_publish(result);
		return;
	}
	fs_publish(vfs_chdir(fs_path1));
}

static void fs_mutate(uint16_t block, uint8_t op)
{
	int result;

	if (!zos_syscall_block_ok(block, 4)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	result = fs_copy_path(fs_le16(block, 0), fs_le16(block, 2), fs_path1);
	if (result != 0) {
		fs_publish(result);
		return;
	}
	if (op == 0) {
		result = vfs_mkdir(fs_path1);
	} else if (op == 1) {
		result = vfs_rmdir(fs_path1);
	} else {
		result = vfs_unlink(fs_path1);
	}
	fs_publish(result);
}

static void fs_rename(uint16_t block)
{
	int result;

	if (!zos_syscall_block_ok(block, 8)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	result = fs_copy_path(fs_le16(block, 0), fs_le16(block, 2), fs_path1);
	if (result != 0) {
		fs_publish(result);
		return;
	}
	result = fs_copy_path(fs_le16(block, 4), fs_le16(block, 6), fs_path2);
	if (result != 0) {
		fs_publish(result);
		return;
	}
	fs_publish(vfs_rename(fs_path1, fs_path2));
}

static void fs_sync(uint16_t block)
{
	struct vfs_descriptor *desc;
	uint8_t fd;

	if (!zos_syscall_block_ok(block, 1)) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	fd = ((const uint8_t *)block)[0];
	desc = vfs_fd_check(fd, VFS_DESC_FILE, process_current_pid());
	if (desc == (struct vfs_descriptor *)0) {
		zos_syscall_error(ZOS_EINVAL);
		return;
	}
	fs_publish(vfs_sync(&desc->u.file));
}

void zos_fs_dispatch(uint8_t number, uint8_t bank, uint16_t block)
{
	if (!fs_ready()) {
		zos_syscall_error(ZOS_EBUSY);
		return;
	}
	switch (number) {
	case ZOS_SYSCALL_FS_OPEN:
		fs_open(block);
		break;
	case ZOS_SYSCALL_FS_CLOSE:
		fs_close_svc(block);
		break;
	case ZOS_SYSCALL_FS_READ:
		fs_io(bank, block, 0);
		break;
	case ZOS_SYSCALL_FS_WRITE:
		fs_io(bank, block, 1);
		break;
	case ZOS_SYSCALL_FS_SEEK:
		fs_seek(block);
		break;
	case ZOS_SYSCALL_FS_STAT:
		fs_stat(bank, block);
		break;
	case ZOS_SYSCALL_FS_OPENDIR:
		fs_opendir(block);
		break;
	case ZOS_SYSCALL_FS_READDIR:
		fs_readdir(bank, block);
		break;
	case ZOS_SYSCALL_FS_CLOSEDIR:
		fs_closedir_svc(block);
		break;
	case ZOS_SYSCALL_FS_MKDIR:
		fs_mutate(block, 0);
		break;
	case ZOS_SYSCALL_FS_RMDIR:
		fs_mutate(block, 1);
		break;
	case ZOS_SYSCALL_FS_UNLINK:
		fs_mutate(block, 2);
		break;
	case ZOS_SYSCALL_FS_RENAME:
		fs_rename(block);
		break;
	case ZOS_SYSCALL_FS_CHDIR:
		fs_chdir(block);
		break;
	case ZOS_SYSCALL_FS_GETCWD:
		fs_getcwd(bank, block);
		break;
	case ZOS_SYSCALL_FS_SYNC:
		fs_sync(block);
		break;
	default:
		zos_syscall_error(ZOS_ENOSYS);
		break;
	}
}
