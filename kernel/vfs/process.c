/* ZephyrOS M10 process filesystem state (packages 27, 42a, 42b).
 *
 * Owns the single global handle table and the per-process CWD pool that
 * expose the namespace to native programs (architecture 36.13).  Both are
 * `_DATA` state; the provider handles they hold never leave the kernel,
 * and the handle a process names is its own per-process number.
 *
 * The table is global, not per-process: ZephyrOS is single-tasking, so a
 * suspended parent's handles stay allocated while its child runs.  A full
 * table is ZOS_EMFILE; every handle-taking operation verifies range, owner
 * and the bound mark and reports ZOS_EINVAL on any mismatch.
 *
 * CWD slots are a static pool of ZOS_EXEC_MAX_DEPTH canonical paths,
 * allocated at process start and released at exit.  The kernel launcher
 * has no slot and its CWD is the constant `/`.
 *
 * Everything here runs in bank 7; no function declares an automatic object
 * larger than 32 bytes (work/M10-REFERENCE.md gate-stack discipline).
 */

#include "kernel/vfs/vfs.h"
#include "kernel/vfs/ns.h"
#include "zephyros/process.h"

struct vfs_descriptor vfs_descriptors[VFS_FD_MAX];

/* CWD pool: owner pid (or VFS_FD_NONE when free) plus the canonical path. */
static uint8_t vfs_cwd_owner[ZOS_EXEC_MAX_DEPTH];
static char vfs_cwd_path[ZOS_EXEC_MAX_DEPTH][ZOS_PATH_MAX];

/* NUL-terminate and never overflow. */
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
		vfs_descriptors[i].number = 0;
		vfs_descriptors[i].endpoint = 0;
		vfs_descriptors[i].object = 0;
		vfs_descriptors[i].flags = 0;
		vfs_descriptors[i].u.ns.romfs.offset = 0;
		vfs_descriptors[i].u.ns.romfs.size = 0;
		vfs_descriptors[i].u.ns.romfs.flags = 0;
		vfs_descriptors[i].u.ns.offset = 0;
		vfs_descriptors[i].u.ns.flags = 0;
	}
	for (i = 0; i < ZOS_EXEC_MAX_DEPTH; ++i) {
		vfs_cwd_owner[i] = VFS_FD_NONE;
		vfs_cwd_path[i][0] = '\0';
	}
}

/* --- the handle table ------------------------------------------------- */

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

uint8_t vfs_owner_current(void)
{
	uint8_t pid = process_current_pid();

	return (pid == PROCESS_NO_PROCESS) ? VFS_OWNER_KERNEL : pid;
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
	/* Every process starts with the two console handles already bound
	 * (architecture 36.13): the standard numbers are never returned by
	 * `bind` and no program has to discover the console. */
	vfs_ns_standard_handles(pid);
	return 0;
}

void vfs_process_exit(uint8_t pid)
{
	uint8_t fd;
	uint8_t i;

	/* Every descriptor the pid owned goes, the standard handles
	 * included: `unbind` refuses those for a live caller, but a dying
	 * process must not keep table slots (the launcher relaunches the
	 * boot program, and the table is only VFS_FD_MAX entries). */
	for (fd = 0; fd < VFS_FD_MAX; ++fd) {
		if (vfs_descriptors[fd].owner == pid) {
			vfs_ns_release_desc(&vfs_descriptors[fd]);
		}
	}
	for (i = 0; i < ZOS_EXEC_MAX_DEPTH; ++i) {
		if (vfs_cwd_owner[i] == pid) {
			vfs_cwd_owner[i] = VFS_FD_NONE;
			vfs_cwd_path[i][0] = '\0';
		}
	}
}

int vfs_cwd_store(const char *norm)
{
	uint8_t i;

	for (i = 0; i < ZOS_EXEC_MAX_DEPTH; ++i) {
		if (vfs_cwd_owner[i] == process_running_pid) {
			vfs_path_copy(vfs_cwd_path[i], norm);
			return 0;
		}
	}
	return -ZOS_EINVAL; /* no CWD slot: chdir from an unowned context */
}
