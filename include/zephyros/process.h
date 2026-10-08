/* ZephyrOS M6 process lifecycle contract (internal kernel interface).
 *
 * M6 is single-tasking with a fixed seven-entry process table, one
 * application bank per process, and synchronous parent/child execution:
 *
 *     parent runs
 *         process_exec(path, &status)      (or SYS_EXEC 05h)
 *             child RUNNING, parent SUSPENDED
 *             child runs alone until SYS_EXIT (or a raw entry return)
 *         parent resumed exactly where it called exec
 *
 * Package 28 extends the same machinery to a bounded chain of ordinary
 * processes (ZOS_EXEC_MAX_DEPTH deep): a child run by SYS_EXEC is an
 * ordinary process, and on exit its suspended parent is marked RUNNING
 * and resumed with the child's status as the SYS_EXEC result.  There is
 * one loader path (process_prepare), one launch/resume gate
 * (process_launch/zos_process_resume) and one exit path.
 *
 * Process descriptors live in bank 7.  Application banks are 0-6; bank 7
 * is kernel-only and is rejected by every allocation path.  At most one
 * process is RUNNING at a time.
 *
 * process_prepare()/process_launch() must be called from kernel context:
 * mode 11 active, interrupts in the caller's own state (the launch gate
 * masks them during the transition and restores them exactly on resume),
 * and SP on a common-memory stack (the boot stack or the syscall gate
 * stack).  This interface is kernel-private; it is not part of the native
 * syscall ABI.
 */

#ifndef ZEPHYROS_PROCESS_H
#define ZEPHYROS_PROCESS_H

#include "zos/types.h"
#include "zos/process.h"
#include "zephyros/zex.h"

/* Table geometry: one slot per application bank. */
#define PROCESS_TABLE_SIZE 7u
#define PROCESS_BANK_COUNT 7u
#define PROCESS_MAX_BANK   6u
#define PROCESS_KERNEL_BANK 7u

/* Nesting geometry (package 28).  ZOS_EXEC_MAX_DEPTH user processes may
 * form one synchronous exec chain; the kernel launcher sits at depth 0:
 *
 *   kernel launcher (boot stack)
 *     +- shell       depth 1: kernel continuation, no saved gate frame
 *        +- IDE      depth 2: frame save #0 holds the shell's SYS_EXEC
 *           +- compiler  depth 3: frame save #1 holds the IDE's
 *              +- linker  depth 4: frame save #2 holds the compiler's
 *                 linker calls SYS_EXEC -> ZOS_EBUSY (before allocation)
 *
 * So there are exactly ZOS_EXEC_MAX_DEPTH - 1 frame-save areas: the
 * outermost process is launched by the kernel from the boot stack and
 * needs no copy.  The continuation arrays carry one saved launcher
 * context (SP, latch, IFF) per depth.  The value itself is owned by the
 * public ABI header abi/zos/process.h. */

/* Gate-frame save size.  Measured in the simulator from the acceptance
 * image: the launch gate's captured continuation SP is exactly
 * zos_gate_stack_top - 32 at every depth in the chain (the gate's saved
 * registers, the SYS_EXEC frame and the process_launch() frame), so
 * 32 + 25% = 40 rounds up to 16-byte units as ZOS_EXEC_FRAME_SAVE = 48.
 * The save areas live in permanent common memory beside the gate stack
 * they copy, so their size is not part of the bank-7 arena budget; the
 * package-28 validation record carries the measured depth. */
#define ZOS_EXEC_FRAME_SAVE 48u

/* Headroom between the frame the preflight measures and the frame the
 * launch gate actually copies.  The preflight runs in the SYS_EXEC
 * handler frame (measured 21 bytes); the copy starts in the deeper
 * process_launch() frame (measured 32 bytes), because the compiler keeps
 * live values in registers across the launch call.  A conservative bound
 * keeps an oversize frame from reaching the copy: the preflight rejects
 * the handler frame above ZOS_EXEC_FRAME_SAVE - ZOS_EXEC_FRAME_SLACK.
 * The simulator test asserts the copied length itself stays inside
 * ZOS_EXEC_FRAME_SAVE at every depth. */
#define ZOS_EXEC_FRAME_SLACK 16u

#if defined(__SDCC)
_Static_assert(ZOS_EXEC_FRAME_SAVE % 16u == 0u,
	       "the exec frame save must be a 16-byte multiple");
_Static_assert(ZOS_EXEC_FRAME_SAVE > ZOS_EXEC_FRAME_SLACK,
	       "the exec frame preflight limit must stay positive");
#endif

/* Parent identity markers. */
#define PROCESS_KERNEL_PARENT 0xFFu /* no user parent descriptor     */
#define PROCESS_NO_PROCESS    0xFFu /* no RUNNING process            */

/* Descriptor / getinfo flags.  The kernel-parent bit is shared by the
 * descriptor and the native getinfo record (see zos/process.h), so the
 * two must stay value-identical; kernel/syscall.c asserts the pairing. */
#define PROCESS_FLAG_KERNEL_PARENT ZOS_PROCESS_FLAG_KERNEL_PARENT

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

/* A prepared (loaded, not yet running) child.  process_prepare() fills
 * it; process_launch() enters it. */
struct process_child {
	uint8_t  pid;   /* slot index 0-6                     */
	uint8_t  bank;  /* application bank 0-6               */
	uint16_t entry; /* child logical entry address        */
};

/* Execute one child synchronously from a VFS path.
 *
 * This is the kernel launcher's convenience wrapper over
 * process_prepare() + process_launch(); it is the depth-0 caller (the
 * parent is PROCESS_KERNEL_PARENT).  On success the child ran to SYS_EXIT
 * (or returned through the common trap, which synthesizes ZOS_EFAULT) and
 * *status receives the u16 exit status; the return value is ZEX_EXEC_OK.
 * On failure nothing is executed, process bookkeeping is rolled back
 * exactly, and a negative ZEX_EXEC_ERR_* code is returned.
 *
 * `path` is a NUL-terminated VFS path (absolute, or relative to `/`);
 * the ZEX image is read through the kernel file API.  `status` must not
 * be null.  SYS_EXEC uses process_prepare()/process_launch() directly so
 * its failures are native ZOS_E* codes (docs/abi.md). */
int process_exec(const char *path, uint16_t *status);

/* Prepare a child: every allocating and fallible step (path resolution,
 * slot, bank, the VFS-backed ZEX load and validation, page zero, the
 * startup block), with the executable source closed before returning.
 * `parent_pid` is the caller's pid or PROCESS_KERNEL_PARENT.  `args` and
 * `args_length` are the caller's raw command tail in `parent_pid`'s
 * application bank (`args_length` 0..ZOS_STARTUP_TAIL_MAX; `args` is
 * ignored when the length is 0); the kernel copies it verbatim into the
 * child's startup block at 0080h and NUL-terminates it.  The `args`
 * window is validated by the SYS_EXEC preflight, not here.  Returns 0 and
 * fills `child`, or a negative code: a native -ZOS_E* value for
 * path/read/allocation failures (ZOS_ENOENT, ZOS_EISDIR, ZOS_ENOSPC, a
 * provider error passed through unchanged), or a ZEX_EXEC_ERR_* validation
 * code for a malformed container.  Nothing is published as RUNNING and
 * every failure rolls the slot back to FREE.
 *
 * process_prepare() runs on the caller's stack (the 256-byte gate stack
 * for SYS_EXEC), so it declares no automatic object larger than 32 bytes;
 * it must return completely before process_launch() runs. */
int process_prepare(const char *path, uint8_t parent_pid, uint16_t args,
		    uint16_t args_length, struct process_child *child);

/* Enter a prepared child through the common launch gate.  Infallible:
 * depth and frame size were preflighted and process_prepare() succeeded.
 * The launching process (when any) is marked SUSPENDED and the current
 * continuation is saved; returns after the child exited and the launcher
 * context was restored.  `child` lives on the caller's stack. */
void process_launch(const struct process_child *child);

/* Diagnostics / acceptance queries. */
uint8_t process_active_count(void);
uint8_t process_free_banks(void);
uint8_t process_current_pid(void);
uint8_t process_current_bank(void);

/* Loader-facing helpers (kernel/process/exec.c drives these):
 *   process_slot_alloc()    reserve the lowest FREE slot (SUSPENDED)
 *   process_slot_release()  exact rollback of a failed load
 *   process_alloc_bank()    lowest bank 0-6 not reserved / in use */
int process_slot_alloc(void);
void process_slot_release(uint8_t pid);
int process_alloc_bank(void);

/* --- exec continuation state (kernel/process/launch.asm, common RAM) ---
 *
 * One saved launcher context per nesting depth; index 0 is the kernel
 * launcher on the boot stack (no gate frame).  The common-resident launch
 * gate fills these; the C side only selects the index. */
struct process_continuation {
	uint16_t saved_sp;    /* launcher SP at the launch call site   */
	uint8_t  saved_latch; /* launcher memory latch                 */
	uint8_t  saved_iff;   /* launcher IFF2 (0 = masked)            */
};

extern struct process_continuation
	zos_process_continuations[ZOS_EXEC_MAX_DEPTH];
extern uint8_t
	zos_process_frames[ZOS_EXEC_MAX_DEPTH - 1][ZOS_EXEC_FRAME_SAVE];

/* Bank-7 runtime-data copy of a SYS_EXEC caller path (ZOS_PATH_MAX
 * bytes; kernel/syscall.c fills it, process_prepare() reads it).  It is
 * `_DATA`, not common memory: it is only ever touched in mode 11, so it
 * has no business in the permanently mapped window. */
extern char zos_process_path_scratch[];

/* Gate-stack bytes in use by this call's frame (zos_gate_stack_top - SP
 * as seen by the caller).  Tiny common helper, used by the SYS_EXEC
 * preflight; measures 0 when the caller is not on the gate stack. */
uint16_t zos_process_continuation_size(void);

/* Common-resident launch/resume primitives (kernel/process/launch.asm).
 * The request structs are read before any mapping change and their layout
 * is pinned by _Static_assert. */
struct zos_process_launch_req {
	uint8_t bank;                          /* child bank 0-6       */
	uint16_t entry;                        /* child entry          */
	uint8_t index;                         /* continuation depth   */
};
void zos_process_launch(const struct zos_process_launch_req *req);

struct zos_process_resume_req {
	uint16_t status;                       /* child exit status    */
	uint8_t index;                         /* continuation depth   */
};
void zos_process_resume(const struct zos_process_resume_req *req);

#if defined(__SDCC)
_Static_assert(sizeof(struct process_child) == 4,
	       "process_child must be 4 packed bytes");
_Static_assert(sizeof(struct process_continuation) == 4,
	       "process_continuation must be 4 packed bytes");
_Static_assert(sizeof(struct zos_process_launch_req) == 4,
	       "launch request must be 4 packed bytes");
_Static_assert(sizeof(struct zos_process_resume_req) == 3,
	       "resume request must be 3 packed bytes");
#endif


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
