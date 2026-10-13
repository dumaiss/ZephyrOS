/* KonaOS M6 process lifecycle (bank-7 kernel code).
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
 * synchronous exec chain of at most KOS_EXEC_MAX_DEPTH processes may be
 * suspended at once (package 28).
 */

#include "process.h"
#include "kernel.h"
#include "kernel/vfs/vfs.h"
#include "kernel/syscall/xmem.h"

/* Common-resident gates (kernel/process/launch.asm). */
void kos_process_launch(const struct kos_process_launch_req *req);
void kos_process_resume(const struct kos_process_resume_req *req);

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

/* --- auxiliary bank slots (architecture 36.15/36.16, package 43) -------
 *
 * One owner byte per slot (pid + 1, 0 = free); the bank states of 36.15
 * are derived: PRIMARY while a table entry owns the bank, AUXILIARY while
 * any slot is owned, FREE otherwise.  A bank reservation also excludes
 * auxiliary use: it is held for a primary allocation. */
static uint8_t aux_owner[PROCESS_BANK_COUNT][KOS_AUX_SLOT_COUNT];

uint8_t process_aux_live(uint8_t bank)
{
	uint8_t slot;

	if (bank >= PROCESS_BANK_COUNT) {
		return 0u;
	}
	for (slot = 0u; slot < KOS_AUX_SLOT_COUNT; ++slot) {
		if (aux_owner[bank][slot] != 0u) {
			return 1u;
		}
	}
	return 0u;
}

/* A bank the auxiliary allocator may touch at all: neither reserved for a
 * primary allocation nor held by a live table entry. */
static uint8_t aux_bank_open(uint8_t bank)
{
	if (bank >= PROCESS_BANK_COUNT) {
		return 0u;
	}
	if ((process_bank_reserved & (uint8_t)(1u << bank)) != 0u) {
		return 0u;
	}
	return (uint8_t)(process_bank_in_use(bank) ? 0u : 1u);
}

static uint8_t aux_run_free(uint8_t bank, uint8_t first, uint8_t slots)
{
	uint8_t i;

	for (i = 0u; i < slots; ++i) {
		if (aux_owner[bank][(uint8_t)(first + i)] != 0u) {
			return 0u;
		}
	}
	return 1u;
}

static void aux_run_take(uint8_t pid, uint8_t bank, uint8_t first,
			 uint8_t slots)
{
	uint8_t i;

	for (i = 0u; i < slots; ++i) {
		aux_owner[bank][(uint8_t)(first + i)] = (uint8_t)(pid + 1u);
	}
}

/* The candidate runs for one allocation, as first-slot values in
 * preference order (architecture 36.16): a far-code segment - or a
 * mappable DATA segment (49d) - is linked at 8000h so it takes slot 4
 * (slots 4-5 when it is over 8 KiB), which is what keeps one of them per
 * auxiliary bank; a mappable object lives in the aperture slice, slots
 * 4-5; a non-mappable object prefers the window-speed slots 3 and 6, then
 * slots 0-2.  Returns the candidate count, 0 when the size cannot be
 * placed at all. */
static uint8_t aux_candidates(uint8_t place, uint8_t slots,
			      uint8_t *firsts)
{
	if (place == KOS_AUX_PLACE_FAR) {
		if (slots == 1u || slots == 2u) {
			firsts[0] = 4u;
			return 1u;
		}
		return 0u;
	}
	if (place == KOS_AUX_PLACE_MAPPABLE) {
		if (slots == 1u) {
			firsts[0] = 4u;
			firsts[1] = 5u;
			return 2u;
		}
		if (slots == 2u) {
			firsts[0] = 4u;
			return 1u;
		}
		return 0u;
	}
	switch (slots) {
	case 1u:
		firsts[0] = 3u;
		firsts[1] = 6u;
		firsts[2] = 0u;
		firsts[3] = 1u;
		firsts[4] = 2u;
		return 5u;
	case 2u:
		firsts[0] = 2u; /* slots 2-3: the run with slot 3 */
		firsts[1] = 0u;
		firsts[2] = 1u;
		return 3u;
	case 3u:
		firsts[0] = 1u; /* slots 1-3: the run with slot 3 */
		firsts[1] = 0u;
		return 2u;
	case 4u:
		firsts[0] = 0u; /* slots 0-3 */
		return 1u;
	default:
		return 0u;
	}
}

/* The preferred free run of this class and size in `bank`, if any. */
static uint8_t aux_bank_run(uint8_t bank, uint8_t place, uint8_t slots,
			    uint8_t *first)
{
	uint8_t firsts[5];
	uint8_t count;
	uint8_t i;

	count = aux_candidates(place, slots, firsts);
	for (i = 0u; i < count; ++i) {
		if (aux_run_free(bank, firsts[i], slots) != 0u) {
			*first = firsts[i];
			return 1u;
		}
	}
	return 0u;
}

int process_aux_alloc(uint8_t pid, uint8_t slots, uint8_t place,
		      uint8_t *bank, uint8_t *slot)
{
	uint8_t b;
	uint8_t first;

	if (pid >= PROCESS_TABLE_SIZE || bank == (uint8_t *)0 ||
	    slot == (uint8_t *)0 || slots == 0u ||
	    slots > KOS_AUX_SLOT_COUNT ||
	    place > KOS_AUX_PLACE_FAR) {
		return -KOS_EINVAL;
	}
	/* Pack an existing auxiliary bank first (36.15), highest first. */
	for (b = PROCESS_BANK_COUNT; b-- > 0;) {
		if (!aux_bank_open(b) || !process_aux_live(b)) {
			continue;
		}
		if (aux_bank_run(b, place, slots, &first) != 0u) {
			aux_run_take(pid, b, first, slots);
			*bank = b;
			*slot = first;
			return 0;
		}
	}
	/* Otherwise open the highest completely free bank. */
	for (b = PROCESS_BANK_COUNT; b-- > 0;) {
		if (!aux_bank_open(b) || process_aux_live(b)) {
			continue;
		}
		if (aux_bank_run(b, place, slots, &first) != 0u) {
			aux_run_take(pid, b, first, slots);
			*bank = b;
			*slot = first;
			return 0;
		}
	}
	return -KOS_ENOSPC;
}

void process_aux_free(uint8_t pid, uint8_t bank, uint8_t slot, uint8_t slots)
{
	uint8_t i;
	uint8_t owner = (uint8_t)(pid + 1u);

	if (bank >= PROCESS_BANK_COUNT || slots == 0u ||
	    slot >= KOS_AUX_SLOT_COUNT) {
		return;
	}
	for (i = 0u; i < slots && (uint8_t)(slot + i) < KOS_AUX_SLOT_COUNT;
	     ++i) {
		if (aux_owner[bank][(uint8_t)(slot + i)] == owner) {
			aux_owner[bank][(uint8_t)(slot + i)] = 0u;
		}
	}
}

/* Lowest bank 0-5 that is neither reserved, held by a table entry, nor
 * auxiliary (architecture 36.15: a bank with any live slot cannot become
 * a primary bank). */
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
		if (process_aux_live(bank)) {
			continue;
		}
		return (int)bank;
	}
	return -KOS_ENOSPC;
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
	return -KOS_ENOSPC;
}

void process_slot_release(uint8_t pid)
{
	if (pid >= PROCESS_TABLE_SIZE) {
		return;
	}
	/* Failed-load rollback also discards the child's filesystem state
	 * (descriptors and CWD) and any extended-memory objects already
	 * allocated for it.  Idempotent. */
	vfs_process_exit(pid);
	kos_xmem_release_process(pid);
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

/* Banks in the FREE state of architecture 36.15: neither held by a table
 * entry (primary) nor carrying a live auxiliary slot.  A reservation
 * (`process_bank_reserve`) is a hold for a future primary allocation, not
 * a bank state, so it does not subtract here.  Reported at boot. */
uint8_t process_free_banks(void)
{
	uint8_t bank;
	uint8_t count = 0;

	for (bank = 0; bank < PROCESS_BANK_COUNT; ++bank) {
		if (process_bank_in_use(bank)) {
			continue;
		}
		if (process_aux_live(bank)) {
			continue;
		}
		++count;
	}
	return count;
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
	struct kos_process_launch_req req;
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
	kos_process_launch(&req);
}

/* Kernel-launcher convenience wrapper: prepare a depth-0 child, then run
 * it.  The child/parent linkage, the loader path and the exit path are
 * shared with SYS_EXEC; only the returned error space differs (the M6
 * EXE_EXEC_ERR_* codes are preserved for the boot launcher and its
 * acceptance tests). */
int process_exec(const char *path, uint16_t *status)
{
	struct process_child child;
	int result;

	if (path == (const char *)0 || status == (uint16_t *)0) {
		return EXE_EXEC_ERR_ARG;
	}
	result = process_prepare(path, PROCESS_KERNEL_PARENT, 0, 0, &child);
	if (result != 0) {
		if (result == -KOS_ENOENT) {
			return EXE_EXEC_ERR_ROMFS;
		}
		if (result == -KOS_EISDIR || result == -KOS_ENOTDIR ||
		    result == -KOS_EINVAL) {
			return EXE_EXEC_ERR_TYPE;
		}
		if (result == -KOS_ENOSPC || result == -KOS_EBUSY) {
			return EXE_EXEC_ERR_NOSPACE;
		}
		if (result <= -0x20) {
			return result; /* granular EXE_EXEC_ERR_* */
		}
		return EXE_EXEC_ERR_READ; /* provider read error */
	}
	process_launch(&child);
	*status = process_last_status;
	return EXE_EXEC_OK;
}

/* Teardown: release the running child's resources, then resume the
 * suspended parent through the common gate.  Never returns to its caller
 * (the caller runs on a stack that is abandoned with the child).  The
 * child's status becomes the resumed SYS_EXEC's result; a kernel-parent
 * child resumes the boot launcher instead. */
void process_exit(uint16_t status)
{
	struct kos_process_resume_req req;
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
	 * release its CWD slot before the parent resumes; then release its
	 * extended-memory objects (36.13/36.15: exit frees every object
	 * after the handles are unbound). */
	vfs_process_exit(pid);
	kos_xmem_release_process(pid);
	/* After the child is released the active count is the parent's
	 * depth, which is also the continuation index saved at its launch. */
	req.status = status;
	req.index = process_active_count();
	if (parent < PROCESS_TABLE_SIZE) {
		process_table[parent].state = PROCESS_RUNNING;
		process_running_pid = parent;
	}
	kos_process_resume(&req);
	for (;;) {
		/* kos_process_resume never returns. */
	}
}

/* Common return trap continuation: a process entry returned without
 * calling SYS_EXIT.  The child is torn down with KOS_EFAULT; all other
 * exit rules are unchanged. */
void process_abort(void)
{
	process_exit(KOS_EFAULT);
}
