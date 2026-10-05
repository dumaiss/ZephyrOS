/* ZephyrOS M9 process filesystem state (package 27).
 *
 * Owns the single global descriptor pool and the per-process CWD pool that
 * expose the VFS to native programs (architecture sections 32.21-32.24).
 * Both are `_DATA` state; the provider handles they hold never leave the
 * kernel, and the fd a process names is the pool index.
 *
 * The pool is global, not per-process: ZephyrOS is single-tasking, so a
 * suspended parent's descriptors stay allocated while its child runs.  A
 * full pool is ZOS_EMFILE; every fd-taking operation verifies range, type
 * and owner and reports ZOS_EINVAL on any mismatch.
 *
 * CWD slots are a static pool of ZOS_EXEC_MAX_DEPTH canonical paths,
 * allocated at process start and released at exit.  The kernel launcher
 * has no slot and its CWD is the constant `/`.
 *
 * Everything here runs in bank 7; no function declares an automatic
 * object larger than 32 bytes (work/M9-REFERENCE.md gate-stack
 * discipline), so the caller's CWD scratch is a static bank-7 buffer.
 */

#include "kernel/vfs/vfs.h"
#include "zephyros/process.h"

struct vfs_descriptor vfs_descriptors[VFS_FD_MAX];

/* CWD pool: owner pid (or VFS_FD_NONE when free) plus the canonical path. */
static uint8_t vfs_cwd_owner[ZOS_EXEC_MAX_DEPTH];
static char vfs_cwd_path[ZOS_EXEC_MAX_DEPTH][ZOS_PATH_MAX];

/* m9p_* style guarded copy: NUL-terminate and never overflow. */
static void vfs_path_copy(char *dst, const char *src)
{
	uint16_t i = 0;

	while (src[i] != '\0' && i + 1u < ZOS_PATH_MAX) {
		dst[i] = src[i];
		++i;
	}
	dst[i] = '\0';
}

void vfs_fs_state_init(void)
{
	uint8_t i;

	for (i = 0; i < VFS_FD_MAX; ++i) {
		vfs_descriptors[i].owner = VFS_FD_NONE;
		vfs_descriptors[i].type = VFS_DESC_FREE;
		vfs_descriptors[i].u.file.mount = (struct vfs_mount_runtime *)0;
		vfs_descriptors[i].u.file.handle = (void *)0;
		vfs_descriptors[i].u.file.size = 0;
		vfs_descriptors[i].u.file.offset = 0;
		vfs_descriptors[i].u.file.flags = 0;
		vfs_descriptors[i].u.file.in_use = 0;
	}
	for (i = 0; i < ZOS_EXEC_MAX_DEPTH; ++i) {
		vfs_cwd_owner[i] = VFS_FD_NONE;
		vfs_cwd_path[i][0] = '\0';
	}
}

/* --- descriptor pool -------------------------------------------------- */

int vfs_fd_open(uint8_t pid, uint8_t type, const char *path, uint16_t flags)
{
	uint8_t fd;
	int result;

	for (fd = 0; fd < VFS_FD_MAX; ++fd) {
		if (vfs_descriptors[fd].owner == VFS_FD_NONE) {
			break;
		}
	}
	if (fd >= VFS_FD_MAX) {
		return -ZOS_EMFILE;
	}
	result = vfs_open(path, flags, &vfs_descriptors[fd].u.file);
	if (result != 0) {
		return result;
	}
	vfs_descriptors[fd].owner = pid;
	vfs_descriptors[fd].type = type;
	return (int)fd;
}

int vfs_fd_opendir(uint8_t pid, const char *path)
{
	uint8_t fd;
	int result;

	for (fd = 0; fd < VFS_FD_MAX; ++fd) {
		if (vfs_descriptors[fd].owner == VFS_FD_NONE) {
			break;
		}
	}
	if (fd >= VFS_FD_MAX) {
		return -ZOS_EMFILE;
	}
	result = vfs_opendir(path, &vfs_descriptors[fd].u.dir);
	if (result != 0) {
		return result;
	}
	vfs_descriptors[fd].owner = pid;
	vfs_descriptors[fd].type = VFS_DESC_DIRECTORY;
	return (int)fd;
}

struct vfs_descriptor *vfs_fd_check(uint8_t fd, uint8_t type, uint8_t pid)
{
	struct vfs_descriptor *desc;

	if (fd >= VFS_FD_MAX) {
		return (struct vfs_descriptor *)0;
	}
	desc = &vfs_descriptors[fd];
	if (desc->owner == VFS_FD_NONE || desc->type != type ||
	    desc->owner != pid) {
		return (struct vfs_descriptor *)0;
	}
	return desc;
}

int vfs_fd_close(uint8_t fd)
{
	struct vfs_descriptor *desc;
	int result;

	if (fd >= VFS_FD_MAX) {
		return -ZOS_EINVAL;
	}
	desc = &vfs_descriptors[fd];
	if (desc->owner == VFS_FD_NONE) {
		return -ZOS_EINVAL;
	}
	if (desc->type == VFS_DESC_FILE) {
		result = vfs_close(&desc->u.file);
	} else if (desc->type == VFS_DESC_DIRECTORY) {
		result = vfs_closedir(&desc->u.dir);
	} else {
		return -ZOS_EINVAL;
	}
	desc->owner = VFS_FD_NONE;
	desc->type = VFS_DESC_FREE;
	return result;
}

int vfs_fd_closedir(uint8_t fd)
{
	if (fd >= VFS_FD_MAX ||
	    vfs_descriptors[fd].type != VFS_DESC_DIRECTORY) {
		return -ZOS_EINVAL;
	}
	return vfs_fd_close(fd);
}

uint8_t vfs_fd_active_count(void)
{
	uint8_t fd;
	uint8_t count = 0;

	for (fd = 0; fd < VFS_FD_MAX; ++fd) {
		if (vfs_descriptors[fd].owner != VFS_FD_NONE) {
			++count;
		}
	}
	return count;
}

/* --- CWD pool and process lifecycle ----------------------------------- */

const char *vfs_cwd_for(uint8_t pid)
{
	uint8_t i;

	if (pid != PROCESS_NO_PROCESS) {
		for (i = 0; i < ZOS_EXEC_MAX_DEPTH; ++i) {
			if (vfs_cwd_owner[i] == pid) {
				return vfs_cwd_path[i];
			}
		}
	}
	return "/";
}

static int vfs_cwd_alloc(uint8_t pid)
{
	uint8_t i;

	for (i = 0; i < ZOS_EXEC_MAX_DEPTH; ++i) {
		if (vfs_cwd_owner[i] == VFS_FD_NONE) {
			vfs_cwd_owner[i] = pid;
			return (int)i;
		}
	}
	return -ZOS_ENOSPC;
}

int vfs_process_start(uint8_t pid, uint8_t parent_pid)
{
	int slot;

	if (pid >= PROCESS_TABLE_SIZE) {
		return -ZOS_EINVAL;
	}
	/* A slot already owned by the pid is a repeat start: keep it. */
	{
		uint8_t i;

		for (i = 0; i < ZOS_EXEC_MAX_DEPTH; ++i) {
			if (vfs_cwd_owner[i] == pid) {
				return 0;
			}
		}
	}
	slot = vfs_cwd_alloc(pid);
	if (slot < 0) {
		return slot;
	}
	vfs_path_copy(vfs_cwd_path[slot], vfs_cwd_for(parent_pid));
	return 0;
}

void vfs_process_exit(uint8_t pid)
{
	uint8_t fd;
	uint8_t i;

	for (fd = 0; fd < VFS_FD_MAX; ++fd) {
		if (vfs_descriptors[fd].owner == pid) {
			(void)vfs_fd_close(fd);
		}
	}
	for (i = 0; i < ZOS_EXEC_MAX_DEPTH; ++i) {
		if (vfs_cwd_owner[i] == pid) {
			vfs_cwd_owner[i] = VFS_FD_NONE;
			vfs_cwd_path[i][0] = '\0';
		}
	}
}

/* --- chdir / getcwd --------------------------------------------------- */

static char vfs_cwd_canonical[ZOS_PATH_MAX];

int vfs_chdir(const char *path)
{
	const char *base = vfs_cwd_for(process_running_pid);
	struct zfs_stat st;
	uint8_t i;
	int result;

	result = vfs_path_normalize_at(base, path, vfs_cwd_canonical,
				       ZOS_PATH_MAX);
	if (result != 0) {
		return result;
	}
	result = vfs_stat(vfs_cwd_canonical, &st);
	if (result != 0) {
		return result;
	}
	if (st.type != ZFS_TYPE_DIR) {
		return -ZOS_ENOTDIR;
	}
	for (i = 0; i < ZOS_EXEC_MAX_DEPTH; ++i) {
		if (vfs_cwd_owner[i] == process_running_pid) {
			vfs_path_copy(vfs_cwd_path[i], vfs_cwd_canonical);
			return 0;
		}
	}
	return -ZOS_EINVAL; /* no CWD slot: chdir from an unowned context */
}

int vfs_getcwd(char *buffer, uint16_t length)
{
	const char *cwd = vfs_cwd_for(process_running_pid);
	uint16_t n = 0;
	uint16_t i;

	if (buffer == (char *)0) {
		return -ZOS_EINVAL;
	}
	while (cwd[n] != '\0') {
		++n;
	}
	if ((uint32_t)n + 1u > (uint32_t)length) {
		return -ZOS_EINVAL;
	}
	for (i = 0; i <= n; ++i) {
		buffer[i] = cwd[i];
	}
	return (int)n;
}
