/* KonaOS VFS part: the handle table. */
/* Process filesystem state.
 *
 * Owns the single global handle table and the per-process CWD pool that
 * expose the namespace to native programs (architecture 4.10).  Both are
 * `_DATA` state; the provider handles they hold never leave the kernel,
 * and the handle a process names is its own per-process number.
 *
 * The table is global, not per-process: KonaOS is single-tasking, so a
 * suspended parent's handles stay allocated while its child runs.  A full
 * table is KOS_EMFILE; every handle-taking operation verifies range, owner
 * and the bound mark and reports KOS_EINVAL on any mismatch.
 *
 * CWD slots are a static pool of KOS_EXEC_MAX_DEPTH canonical paths,
 * allocated at process start and released at exit.  The kernel launcher
 * has no slot and its CWD is the constant `/`.
 *
 * Everything here runs in bank 7; no function declares an automatic object
 * larger than 32 bytes (the gate stack is shared and small).
 */

#include "kernel/vfs/vfs.h"
#include "process.h"

#include "mod.h"

struct vfs_handle_entry vfs_handles[VFS_HANDLE_MAX];

/* CWD pool: owner pid (or VFS_FD_NONE when free) plus the canonical path. */
static uint8_t vfs_cwd_owner[KOS_EXEC_MAX_DEPTH];
static char vfs_cwd_path[KOS_EXEC_MAX_DEPTH][KOS_PATH_MAX];

/* NUL-terminate and never overflow. */
static void vfs_path_copy(char *dst, const char *src)
{
	uint16_t i = 0;

	while (src[i] != '\0' && i + 1u < KOS_PATH_MAX) {
		dst[i] = src[i];
		++i;
	}
	dst[i] = '\0';
}

void vfs_fs_state_init(void)
{
	uint8_t i;

	for (i = 0; i < VFS_HANDLE_MAX; ++i) {
		vfs_handles[i].owner = VFS_FD_NONE;
		vfs_handles[i].type = VFS_DESC_FREE;
		vfs_handles[i].number = 0;
		vfs_handles[i].endpoint = 0;
		vfs_handles[i].object = 0;
		vfs_handles[i].flags = 0;
		vfs_handles[i].u.ns.romfs.offset = 0;
		vfs_handles[i].u.ns.romfs.size = 0;
		vfs_handles[i].u.ns.romfs.flags = 0;
		vfs_handles[i].u.ns.offset = 0;
		vfs_handles[i].u.ns.flags = 0;
	}
	for (i = 0; i < KOS_EXEC_MAX_DEPTH; ++i) {
		vfs_cwd_owner[i] = VFS_FD_NONE;
		vfs_cwd_path[i][0] = '\0';
	}
}

/* --- the handle table ------------------------------------------------- */

static void vfs_handle_clear(struct vfs_handle_entry *entry)
{
	entry->owner = VFS_FD_NONE;
	entry->type = VFS_DESC_FREE;
	entry->number = 0;
	entry->endpoint = 0;
	entry->object = 0;
	entry->flags = 0;
}

struct vfs_handle_entry *vfs_handle_alloc(uint8_t pid, uint8_t first)
{
	struct vfs_handle_entry *entry;
	uint8_t slot;
	uint8_t number;
	uint8_t i;

	for (slot = 0; slot < VFS_HANDLE_MAX; ++slot) {
		if (vfs_handles[slot].owner == VFS_FD_NONE) {
			break;
		}
	}
	if (slot >= VFS_HANDLE_MAX) {
		return (struct vfs_handle_entry *)0;
	}
	/* The lowest handle number this process does not already name. */
	for (number = first; number != 0u; ++number) {
		uint8_t taken = 0u;

		for (i = 0; i < VFS_HANDLE_MAX; ++i) {
			if (vfs_handles[i].owner == pid &&
			    vfs_handles[i].endpoint != 0u &&
			    vfs_handles[i].number == number) {
				taken = 1u;
				break;
			}
		}
		if (!taken) {
			break;
		}
	}
	if (number == 0u) {
		return (struct vfs_handle_entry *)0;
	}
	entry = &vfs_handles[slot];
	vfs_handle_clear(entry);
	entry->owner = pid;
	entry->number = number;
	return entry;
}

uint8_t vfs_owner_current(void)
{
	uint8_t pid = process_current_pid();

	return (pid == PROCESS_NO_PROCESS) ? VFS_OWNER_KERNEL : pid;
}

struct vfs_handle_entry *vfs_handle_find(uint8_t pid, uint8_t number)
{
	uint8_t i;

	if (number == 0u || pid == VFS_FD_NONE) {
		return (struct vfs_handle_entry *)0;
	}
	for (i = 0; i < VFS_HANDLE_MAX; ++i) {
		struct vfs_handle_entry *entry = &vfs_handles[i];

		if (entry->owner == pid && entry->endpoint != 0u &&
		    entry->number == number) {
			return entry;
		}
	}
	return (struct vfs_handle_entry *)0;
}

void vfs_handle_release(struct vfs_handle_entry *entry)
{
	if (entry != (struct vfs_handle_entry *)0 &&
	    entry->owner != VFS_FD_NONE) {
		vfs_handle_clear(entry);
	}
}

uint8_t vfs_handle_active_count(void)
{
	uint8_t fd;
	uint8_t count = 0;

	for (fd = 0; fd < VFS_HANDLE_MAX; ++fd) {
		if (vfs_handles[fd].owner != VFS_FD_NONE) {
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
		for (i = 0; i < KOS_EXEC_MAX_DEPTH; ++i) {
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

	for (i = 0; i < KOS_EXEC_MAX_DEPTH; ++i) {
		if (vfs_cwd_owner[i] == VFS_FD_NONE) {
			vfs_cwd_owner[i] = pid;
			return (int)i;
		}
	}
	return -KOS_ENOSPC;
}

int vfs_process_start(uint8_t pid, uint8_t parent_pid)
{
	int slot;

	if (pid >= PROCESS_TABLE_SIZE) {
		return -KOS_EINVAL;
	}
	/* A slot already owned by the pid is a repeat start: keep it. */
	{
		uint8_t i;

		for (i = 0; i < KOS_EXEC_MAX_DEPTH; ++i) {
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
	 * (architecture 4.10): the standard numbers are never returned by
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
	 * boot program, and the table is only VFS_HANDLE_MAX entries). */
	for (fd = 0; fd < VFS_HANDLE_MAX; ++fd) {
		if (vfs_handles[fd].owner == pid) {
			vfs_ns_release_entry(&vfs_handles[fd]);
		}
	}
	for (i = 0; i < KOS_EXEC_MAX_DEPTH; ++i) {
		if (vfs_cwd_owner[i] == pid) {
			vfs_cwd_owner[i] = VFS_FD_NONE;
			vfs_cwd_path[i][0] = '\0';
		}
	}
}

int vfs_cwd_store(const char *norm)
{
	uint8_t i;

	for (i = 0; i < KOS_EXEC_MAX_DEPTH; ++i) {
		if (vfs_cwd_owner[i] == process_running_pid) {
			vfs_path_copy(vfs_cwd_path[i], norm);
			return 0;
		}
	}
	return -KOS_EINVAL; /* no CWD slot: chdir from an unowned context */
}

void vfs_ns_release_entry(struct vfs_handle_entry *entry)
{
	/* The provider release: a bound ROMFS FILE instance is the entry
	 * itself, a `/fs` FILE instance is the driver's, and a DEVICE
	 * handle is a notification - the driver keeps the object and
	 * restores the device the handle took over.  An endpoint only
	 * hears about the release when its image declared it
	 * (MOD_EP_RELEASE), so an ordinary device bind costs no call. */
	if (entry->endpoint == VFS_NS_FS_ENDPOINT) {
		vfs_ns_fs_release(entry->type, entry->object);
	} else if ((ipc_endpoint_flags(entry->endpoint) &
		    MOD_EP_RELEASE) != 0u) {
		vfs_device_notify_release(
			entry->endpoint, entry->object,
			(uint8_t)((entry->flags & VFS_HANDLE_EXCLUSIVE) != 0u
					  ? 1u : 0u));
	}
	vfs_handle_release(entry);
}

/* --- standard handles ---------------------------------------------------- */

void vfs_ns_standard_handles(uint8_t pid)
{
	const struct vfs_ns_publication *pub = vfs_pub_find("/console");
	uint8_t number;

	if (pub == (const struct vfs_ns_publication *)0) {
		return; /* no console driver in this build */
	}
	for (number = KOS_HANDLE_STDOUT; number <= KOS_HANDLE_STDIN;
	     ++number) {
		struct vfs_handle_entry *entry = vfs_handle_alloc(pid, number);

		if (entry == (struct vfs_handle_entry *)0) {
			return;
		}
		entry->type = KOS_TYPE_CONSOLE;
		entry->endpoint = pub->number;
		entry->object = pub->object;
		entry->flags = 0u;
	}
}
