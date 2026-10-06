/* ZephyrOS M6 process lifecycle (bank-7 kernel code).
 *
 * Owns the fixed seven-entry process table, the deterministic
 * lowest-free-bank allocator (including test/acceptance reservations),
 * and the teardown path that resumes the suspended launcher.
 *
 * The table is process metadata only: the child's memory lives in its
 * application bank and its saved launcher context lives in common state
 * (kernel/process/launch.asm).  A failed load never leaves a slot or a
 * bank reserved; process_prepare() in exec.c performs the loading and
 * rolls back through process_slot_release() below.
 *
 * Single-tasking contract: at most one process is RUNNING, and one
 * synchronous exec chain of at most ZOS_EXEC_MAX_DEPTH processes may be
 * suspended at once (package 28).
 */

#include "zephyros/process.h"
#include "zephyros/kernel.h"
#include "kernel/vfs/vfs.h"

#if defined(ZOS_M10_SPIKE)
#include "kernel/process/bankmap.h"
#include "kernel/process/zex2prog.h"
#include "kernel/syscall/xmem.h"
#endif

/* Suspended-SYS_EXEC path buffer (bank-7 runtime data).
 *
 * SYS_EXEC copies the caller's path here and process_prepare() reads it
 * after the copy-in call has returned; that is the whole lifetime, and
 * both ends run in mode 11 with bank 7 mapped.  It must not be one of the
 * VFS workspace slots because the loader is using those while it reads
 * this path.
 *
 * It is bank-7 `_DATA`, not common memory: common memory is permanent
 * code and state that has to stay reachable while the mapping changes
 * (docs/architecture.md section 7), and nothing here is touched across a
 * mode change.  Common memory is also visibly mapped in mode 10, so
 * placing anything there is a budget decision, never a protection one. */
char zos_process_path_scratch[ZOS_PATH_MAX];

#if defined(__SDCC)
_Static_assert(sizeof(zos_process_path_scratch) == ZOS_PATH_MAX,
	       "the SYS_EXEC path buffer must hold a full path");
#endif

/* Common-resident gates (kernel/process/launch.asm). */
void zos_process_launch(const struct zos_process_launch_req *req);
void zos_process_resume(const struct zos_process_resume_req *req);

struct process process_table[PROCESS_TABLE_SIZE];
uint16_t process_last_status;
uint8_t process_running_pid = PROCESS_NO_PROCESS;

static uint8_t process_bank_reserved; /* bit N set = bank N reserved */

static int process_bank_in_use(uint8_t bank)
{
	uint8_t i;

	for (i = 0; i < PROCESS_TABLE_SIZE; ++i) {
		if (process_table[i].state != PROCESS_FREE &&
		    process_table[i].bank == bank) {
			return 1;
		}
	}
	return 0;
}

/* Lowest bank 0-6 that is neither reserved nor held by a table entry. */
int process_alloc_bank(void)
{
	uint8_t bank;

	for (bank = 0; bank < PROCESS_BANK_COUNT; ++bank) {
		if ((process_bank_reserved & (uint8_t)(1u << bank)) != 0) {
			continue;
		}
		if (process_bank_in_use(bank)) {
			continue;
		}
#if defined(ZOS_M10_SPIKE)
		/* A bank carrying an auxiliary allocation is not free for a
		 * primary process (architecture section 36.5). */
		if (!bankmap_bank_free(bank)) {
			continue;
		}
#endif
		return (int)bank;
	}
	return -ZOS_ENOSPC;
}

int process_bank_reserve(void)
{
	int bank = process_alloc_bank();

	if (bank < 0) {
		return bank;
	}
	process_bank_reserved |= (uint8_t)(1u << (uint8_t)bank);
	return bank;
}

void process_bank_release(uint8_t bank)
{
	if (bank >= PROCESS_BANK_COUNT) {
		return; /* bank 7 is never reservable */
	}
	process_bank_reserved &= (uint8_t)~(uint8_t)(1u << bank);
}

#if defined(ZOS_M10_SPIKE)
uint8_t process_bank_primary_used(uint8_t bank)
{
	if (bank >= PROCESS_BANK_COUNT) {
		return 1;
	}
	if ((process_bank_reserved & (uint8_t)(1u << bank)) != 0) {
		return 1;
	}
	return (uint8_t)(process_bank_in_use(bank) ? 1 : 0);
}
#endif

/* Reserve a table slot for a load in progress (state SUSPENDED).  The
 * bank field is invalidated so bank_in_use() cannot mistake this
 * half-built entry for an owner of application bank 0. */
int process_slot_alloc(void)
{
	uint8_t pid;

	for (pid = 0; pid < PROCESS_TABLE_SIZE; ++pid) {
		if (process_table[pid].state == PROCESS_FREE) {
			process_table[pid].state = PROCESS_SUSPENDED;
			process_table[pid].pid = pid;
			process_table[pid].bank = PROCESS_NO_PROCESS;
			process_table[pid].parent = PROCESS_KERNEL_PARENT;
			process_table[pid].exit_status = 0;
			process_table[pid].flags = 0;
			return (int)pid;
		}
	}
	return -ZOS_ENOSPC;
}

void process_slot_release(uint8_t pid)
{
	if (pid >= PROCESS_TABLE_SIZE) {
		return;
	}
#if defined(ZOS_M10_SPIKE)
	/* Rollback also drops the attempted process's auxiliary slots,
	 * runtime xmem objects and its ZEX v2 program state (idempotent). */
	zos_xmem_release_pid(pid);
	bankmap_aux_release(pid);
	if (zos_zex2_owner == (uint8_t)(pid + 1u)) {
		zex2_prog_reset();
	}
#endif
	/* Failed-load rollback also discards the child's filesystem state
	 * (descriptors and CWD) if it was already allocated.  Idempotent. */
	vfs_process_exit(pid);
	process_table[pid].state = PROCESS_FREE;
	process_table[pid].bank = PROCESS_NO_PROCESS;
	process_table[pid].exit_status = 0;
}

uint8_t process_active_count(void)
{
	uint8_t i;
	uint8_t count = 0;

	for (i = 0; i < PROCESS_TABLE_SIZE; ++i) {
		if (process_table[i].state != PROCESS_FREE) {
			++count;
		}
	}
	return count;
}

uint8_t process_free_banks(void)
{
	return (uint8_t)(PROCESS_BANK_COUNT - process_active_count());
}

uint8_t process_current_pid(void)
{
	return process_running_pid;
}

uint8_t process_current_bank(void)
{
	if (process_running_pid >= PROCESS_TABLE_SIZE) {
		return 0xFF; /* no RUNNING process */
	}
	return process_table[process_running_pid].bank;
}

/* Mark the loaded child RUNNING, suspend the launcher, and enter it
 * through the common gate.  Returns only after the child exited and the
 * launcher context was restored exactly. */
void process_launch(const struct process_child *child)
{
	struct zos_process_launch_req req;
	uint8_t index = (uint8_t)(process_active_count() - 1u);

	/* The launching process (when any) is suspended at this depth; the
	 * continuation index is its depth.  The kernel launcher runs at
	 * depth 0 on the boot stack and needs no gate frame. */
	if (process_running_pid < PROCESS_TABLE_SIZE) {
		process_table[process_running_pid].state = PROCESS_SUSPENDED;
	}
	process_table[child->pid].state = PROCESS_RUNNING;
	process_running_pid = child->pid;

	req.bank = child->bank;
	req.entry = child->entry;
	req.index = index;
	/* Suspension point.  Everything the launcher can still need is either
	 * its own stack frame on the common gate stack (which the gate saves
	 * per depth) or common state: the child's SYS_EXEC path went through
	 * the common path scratch, process_prepare() returned its loader and
	 * provider state to the callers, the executable source is closed and
	 * no borrowed VFS workspace view outlives the call.  The child is
	 * free to reuse all of it; restoring the saved frame restores only
	 * the launcher's own continuation. */
	zos_process_launch(&req);
}

/* Kernel-launcher convenience wrapper: prepare a depth-0 child, then run
 * it.  The child/parent linkage, the loader path and the exit path are
 * shared with SYS_EXEC; only the returned error space differs (the M6
 * ZEX_EXEC_ERR_* codes are preserved for the boot launcher and its
 * acceptance tests). */
int process_exec(const char *path, uint16_t *status)
{
	struct process_child child;
	int result;

	if (path == (const char *)0 || status == (uint16_t *)0) {
		return ZEX_EXEC_ERR_ARG;
	}
	result = process_prepare(path, PROCESS_KERNEL_PARENT, 0, 0, &child);
	if (result != 0) {
		if (result == -ZOS_ENOENT) {
			return ZEX_EXEC_ERR_ROMFS;
		}
		if (result == -ZOS_EISDIR || result == -ZOS_ENOTDIR ||
		    result == -ZOS_EINVAL) {
			return ZEX_EXEC_ERR_TYPE;
		}
		if (result == -ZOS_ENOSPC || result == -ZOS_EBUSY) {
			return ZEX_EXEC_ERR_NOSPACE;
		}
		if (result <= -0x20) {
			return result; /* granular ZEX_EXEC_ERR_* */
		}
		return ZEX_EXEC_ERR_READ; /* provider read error */
	}
	process_launch(&child);
	*status = process_last_status;
	return ZEX_EXEC_OK;
}

/* Teardown: release the running child's resources, then resume the
 * suspended parent through the common gate.  Never returns to its caller
 * (the caller runs on a stack that is abandoned with the child).  The
 * child's status becomes the resumed SYS_EXEC's result; a kernel-parent
 * child resumes the boot launcher instead. */
void process_exit(uint16_t status)
{
	struct zos_process_resume_req req;
	uint8_t pid = process_running_pid;
	uint8_t parent;

	process_last_status = status;
	if (pid >= PROCESS_TABLE_SIZE) {
		panic("process exit without a running process");
	}
	process_table[pid].exit_status = status;
	parent = process_table[pid].parent;
	process_table[pid].state = PROCESS_FREE;
	process_running_pid = PROCESS_NO_PROCESS;
	/* Close every descriptor the pid owned (with close semantics) and
	 * release its CWD slot before the parent resumes. */
	vfs_process_exit(pid);
#if defined(ZOS_M10_SPIKE)
	/* Release the process's auxiliary slots, runtime objects and ZEX v2
	 * program state so a repeated exec/exit cycle leaks nothing
	 * (architecture 36.6). */
	zos_xmem_release_pid(pid);
	bankmap_aux_release(pid);
	if (zos_zex2_owner == (uint8_t)(pid + 1u)) {
		zex2_prog_reset();
	}
#endif

	/* After the child is released the active count is the parent's
	 * depth, which is also the continuation index saved at its launch. */
	req.status = status;
	req.index = process_active_count();
	if (parent < PROCESS_TABLE_SIZE) {
		process_table[parent].state = PROCESS_RUNNING;
		process_running_pid = parent;
	}
	zos_process_resume(&req);
	for (;;) {
		/* zos_process_resume never returns. */
	}
}

/* Common return trap continuation: a process entry returned without
 * calling SYS_EXIT.  The child is torn down with ZOS_EFAULT; all other
 * exit rules are unchanged. */
void process_abort(void)
{
	process_exit(ZOS_EFAULT);
}
