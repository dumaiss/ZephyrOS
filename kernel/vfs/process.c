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
#if ZOS_NS_HANDLES
#include "kernel/vfs/ns.h"
#endif
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
#if ZOS_NS_HANDLES
		vfs_descriptors[i].number = 0;
		vfs_descriptors[i].endpoint = 0;
		vfs_descriptors[i].object = 0;
		vfs_descriptors[i].flags = 0;
#endif
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
#if ZOS_NS_HANDLES
	/* An M9 descriptor, not a bound handle. */
	vfs_descriptors[fd].endpoint = 0u;
#endif
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
#if ZOS_NS_HANDLES
	/* An M9 descriptor, not a bound handle. */
	vfs_descriptors[fd].endpoint = 0u;
#endif
	return (int)fd;
}

struct vfs_descriptor *vfs_fd_check(uint8_t fd, uint8_t type, uint8_t pid)
{
	struct vfs_descriptor *desc;

	if (fd >= VFS_FD_MAX) {
		return (struct vfs_descriptor *)0;
	}
	desc = &vfs_descriptors[fd];
#if ZOS_NS_HANDLES
	/* A bound object handle (nonzero endpoint) is never an M9
	 * descriptor, whatever its protocol-type number: the two families
	 * share one pool but stay disjoint. */
	if (desc->owner == VFS_FD_NONE || desc->endpoint != 0u ||
	    desc->type != type || desc->owner != pid) {
		return (struct vfs_descriptor *)0;
	}
#else
	if (desc->owner == VFS_FD_NONE || desc->type != type ||
	    desc->owner != pid) {
		return (struct vfs_descriptor *)0;
	}
#endif
	return desc;
}

#if ZOS_NS_HANDLES
/* --- the object handle table (package 42a) ---------------------------- */

static void vfs_handle_clear(struct vfs_descriptor *desc)
{
	desc->owner = VFS_FD_NONE;
	desc->type = VFS_DESC_FREE;
	desc->number = 0;
	desc->endpoint = 0;
	desc->object = 0;
	desc->flags = 0;
}

struct vfs_descriptor *vfs_handle_alloc(uint8_t pid, uint8_t first)
{
	struct vfs_descriptor *desc;
	uint8_t slot;
	uint8_t number;
	uint8_t i;

	for (slot = 0; slot < VFS_FD_MAX; ++slot) {
		if (vfs_descriptors[slot].owner == VFS_FD_NONE) {
			break;
		}
	}
	if (slot >= VFS_FD_MAX) {
		return (struct vfs_descriptor *)0;
	}
	/* The lowest handle number this process does not already name. */
	for (number = first; number != 0u; ++number) {
		uint8_t taken = 0u;

		for (i = 0; i < VFS_FD_MAX; ++i) {
			if (vfs_descriptors[i].owner == pid &&
			    vfs_descriptors[i].endpoint != 0u &&
			    vfs_descriptors[i].number == number) {
				taken = 1u;
				break;
			}
		}
		if (!taken) {
			break;
		}
	}
	if (number == 0u) {
		return (struct vfs_descriptor *)0;
	}
	desc = &vfs_descriptors[slot];
	vfs_handle_clear(desc);
	desc->owner = pid;
	desc->number = number;
	return desc;
}

struct vfs_descriptor *vfs_handle_find(uint8_t pid, uint8_t number)
{
	uint8_t i;

	if (number == 0u || pid == VFS_FD_NONE) {
		return (struct vfs_descriptor *)0;
	}
	for (i = 0; i < VFS_FD_MAX; ++i) {
		struct vfs_descriptor *desc = &vfs_descriptors[i];

		if (desc->owner == pid && desc->endpoint != 0u &&
		    desc->number == number) {
			return desc;
		}
	}
	return (struct vfs_descriptor *)0;
}

void vfs_handle_release(struct vfs_descriptor *desc)
{
	if (desc != (struct vfs_descriptor *)0 &&
	    desc->owner != VFS_FD_NONE) {
		vfs_handle_clear(desc);
	}
}
#endif

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
#if ZOS_NS_HANDLES
	if (desc->endpoint != 0u) {
		/* A bound object handle: release the instance (exit unbinds
		 * every handle the process owns, architecture 36.13). */
		vfs_handle_release(desc);
		return 0;
	}
#endif
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
#if ZOS_NS_HANDLES
	    vfs_descriptors[fd].endpoint != 0u ||
#endif
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
#if ZOS_NS_HANDLES
	/* Every process starts with its own console input/output handles
	 * (architecture 36.13, package 42a): they are new entries for the
	 * child, not the parent's, and absent when the build publishes no
	 * `/dev/console` - not a start failure. */
	(void)vfs_ns_standard_handles(pid);
#endif
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

int vfs_chdir(const char *path)
{
	const char *base = vfs_cwd_for(process_running_pid);
	struct zfs_stat st;
	uint8_t i;
	int result;

	/* Normalize into the workspace path slot, verify the directory with the
	 * non-normalizing stat (so the slot is never read and written at once),
	 * and only then store the canonical path in the caller's CWD slot, so a
	 * failed chdir never disturbs the CWD. */
	result = vfs_path_normalize_at(base, path, VFS_WS_NORM_A,
				       ZOS_PATH_MAX);
	if (result != 0) {
		return result;
	}
	result = vfs_stat_norm(VFS_WS_NORM_A, &st);
	if (result != 0) {
		return result;
	}
	if (st.type != ZFS_TYPE_DIR) {
		return -ZOS_ENOTDIR;
	}
	for (i = 0; i < ZOS_EXEC_MAX_DEPTH; ++i) {
		if (vfs_cwd_owner[i] == process_running_pid) {
			vfs_path_copy(vfs_cwd_path[i], VFS_WS_NORM_A);
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
