/* ZephyrOS M6 process lifecycle contract (internal kernel interface).
 *
 * M6 is single-tasking with a fixed seven-entry process table, one
 * application bank per process, and synchronous parent/child execution:
 *
 *     parent runs
 *         process_exec(name, &status)
 *             child RUNNING, parent suspended
 *             child runs alone until SYS_EXIT (or a raw entry return)
 *         parent resumed exactly where it called process_exec
 *
 * Process descriptors live in bank 7.  Application banks are 0-6; bank 7
 * is kernel-only and is rejected by every allocation path.  At most one
 * process is RUNNING at a time.
 *
 * process_exec() must be called from kernel context: mode 11 active,
 * interrupts in the caller's own state (the launch gate masks them during
 * the transition and restores them exactly on resume), and SP on a
 * common-memory stack (the boot stack or the syscall gate stack).  This
 * interface is kernel-private; it is not part of the native syscall ABI.
 */

#ifndef ZEPHYROS_PROCESS_H
#define ZEPHYROS_PROCESS_H

#include "zephyros/types.h"
#include "zephyros/zex.h"

/* Table geometry: one slot per application bank. */
#define PROCESS_TABLE_SIZE 7u
#define PROCESS_BANK_COUNT 7u
#define PROCESS_MAX_BANK   6u
#define PROCESS_KERNEL_BANK 7u

/* Parent identity markers. */
#define PROCESS_KERNEL_PARENT 0xFFu /* no user parent descriptor     */
#define PROCESS_NO_PROCESS    0xFFu /* no RUNNING process            */

/* Descriptor / getinfo flags.  The kernel-parent bit is shared by the
 * descriptor and the native getinfo record (see syscall.h), so the two
 * must stay value-identical; kernel/syscall.c asserts the pairing. */
#define PROCESS_FLAG_KERNEL_PARENT 0x0001u

/* Slot states.  A slot is SUSPENDED while a load is being built and
 * RUNNING while its child executes; exit returns it to FREE. */
enum {
	PROCESS_FREE = 0,
	PROCESS_RUNNING,
	PROCESS_SUSPENDED
};

struct process {
	uint8_t  state;       /* PROCESS_*                          */
	uint8_t  pid;         /* slot index, 0-6                    */
	uint8_t  bank;        /* application bank 0-6               */
	uint8_t  parent;      /* pid or PROCESS_KERNEL_PARENT       */
	uint16_t flags;       /* PROCESS_FLAG_*                     */
	uint16_t exit_status; /* last recorded status of this slot  */
};

/* Fixed table (bank-7 data), exported for acceptance inspection. */
extern struct process process_table[PROCESS_TABLE_SIZE];

/* Status of the most recently exited child, handed to the parent after
 * the common resume gate returns control to process_exec(). */
extern uint16_t process_last_status;

/* Execute one child synchronously from the ROMFS recovery store.
 *
 * On success the child ran to SYS_EXIT (or returned through the common
 * trap, which synthesizes ZOS_EFAULT) and *status receives the u16 exit
 * status; the return value is ZEX_EXEC_OK.  On failure nothing is
 * executed, process bookkeeping is rolled back exactly, and a negative
 * ZEX_EXEC_ERR_* code is returned.
 *
 * `romfs_name` is a NUL-terminated ROMFS name; `status` must not be
 * null.  Nesting is not supported (the launcher is single-depth). */
int process_exec(const char *romfs_name, uint16_t *status);

/* Diagnostics / acceptance queries. */
uint8_t process_active_count(void);
uint8_t process_free_banks(void);
uint8_t process_current_pid(void);
uint8_t process_current_bank(void);

/* Loader-facing helpers (kernel/process/exec.c drives these):
 *   process_slot_alloc()    reserve the lowest FREE slot (SUSPENDED)
 *   process_slot_release()  exact rollback of a failed load
 *   process_alloc_bank()    lowest bank 0-6 not reserved / in use
 *   process_launch_child()  mark RUNNING and enter through the gate;
 *                           returns after the child exits and the
 *                           launcher context was restored. */
int process_slot_alloc(void);
void process_slot_release(uint8_t pid);
int process_alloc_bank(void);
void process_launch_child(uint8_t pid, uint8_t bank, uint16_t entry);

/* PID of the RUNNING process (PROCESS_NO_PROCESS when none). */
extern uint8_t process_running_pid;

/* --- application-bank copy gate (kernel/memory/user_copy.asm) ---------
 *
 * The gate performs one bounded transfer between an application bank and
 * a common-resident buffer.  The request is read in mode 11 before any
 * mapping change, so it may live in bank-7 or common kernel memory;
 * `buffer` must be common-resident (normally zos_user_copy_staging).
 * Bank 7 is rejected with ZOS_EINVAL before any state changes; a zero
 * length is a validated no-op.  Single-depth. */
#define ZOS_BANK_OP_READ  1u /* user addr -> buffer  */
#define ZOS_BANK_OP_WRITE 2u /* buffer -> user addr  */
#define ZOS_BANK_OP_ZERO  3u /* fill user addr range */

struct zos_bank_copy_req {
	uint8_t bank;   /* application bank 0-6               */
	uint8_t op;     /* ZOS_BANK_OP_*                      */
	uint16_t addr;  /* user-side logical address          */
	uint8_t *buffer;/* common staging address (READ/WRITE)*/
	uint16_t length;/* bytes; 0 = validated no-op         */
};

#if defined(__SDCC)
_Static_assert(sizeof(uint8_t *) == 2,
	       "the copy gate interface requires 16-bit data pointers");
_Static_assert(sizeof(struct zos_bank_copy_req) == 8,
	       "bank copy request must be 8 packed bytes");
_Static_assert(__builtin_offsetof(struct zos_bank_copy_req, op) == 1,
	       "bank copy op offset");
_Static_assert(__builtin_offsetof(struct zos_bank_copy_req, addr) == 2,
	       "bank copy addr offset");
_Static_assert(__builtin_offsetof(struct zos_bank_copy_req, buffer) == 4,
	       "bank copy buffer offset");
_Static_assert(__builtin_offsetof(struct zos_bank_copy_req, length) == 6,
	       "bank copy length offset");
#endif

/* Returns 0 or ZOS_EINVAL. */
uint8_t zos_bank_copy(const struct zos_bank_copy_req *req);

/* Common staging buffer the syscall service transfers through. */
extern uint8_t zos_user_copy_staging[];

/* Bank reservation through the same deterministic allocator exec uses
 * (lowest free bank).  process_bank_reserve() returns the bank or
 * -ZOS_ENOSPC when none is free; release rejects banks that were not
 * reserved and bank 7.  Reservations never outlive the boot session. */
int process_bank_reserve(void);
void process_bank_release(uint8_t bank);

/* Process teardown (bank-7, called from the syscall dispatcher /
 * common return trap; never returns - the parent is resumed through
 * zos_process_resume). */
void process_exit(uint16_t status);
void process_abort(void); /* exit with ZOS_EFAULT (raw entry return) */

#endif /* ZEPHYROS_PROCESS_H */
