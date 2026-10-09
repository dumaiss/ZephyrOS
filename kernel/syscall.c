/* ZephyrOS M6/M7 native syscall dispatcher (bank 7).
 *
 * The stable register ABI (docs/abi.md) is converted to internal C state
 * by the assembly boundary zos_syscall_dispatch (kernel/syscall/entry.asm);
 * SDCC calling conventions never cross the syscall boundary.
 *
 * M6 services: SYS_EXIT and SYS_GETINFO.  The console bytes a program
 * writes and reads are no longer syscalls: the SDK's console functions
 * are clients of the console service (abi/zos/protocol.h, class
 * ZOS_CLASS_CONSOLE) through the IPC pair below, so the nucleus keeps no
 * console binding at all.  Every caller byte is still validated against
 * the documented windows before it is touched:
 *
 *   - argument blocks must lie in 0100h-1FFFh (read while mode 11 is
 *     active, where only the caller window is visible);
 *   - read/write buffers may occupy 0100h-DFFFh and are transferred
 *     through the common-resident copy gates;
 *   - bank 7 and arithmetic wrap are rejected.
 *
 * M7 adds the diagnostic set (SYS_DIAG_*) implemented in
 * kernel/syscall/diag.c; this file owns the dispatch switch and the
 * shared result/window helpers it uses.  The M9 filesystem family and the
 * M10 IPC pair dispatch to their own cold bodies (syscall/fs.h,
 * syscall/ipc.h) from the same switch.
 *
 * Failure returns A = ZOS_E* with carry set and HL = 0; success returns
 * carry clear with HL = primary result.  Process bookkeeping is never
 * damaged by a failed operation.
 */

#include "zephyros/kernel.h"
#include "zos/syscall.h"
#include "zos/ipc.h"
#include "zephyros/process.h"

#include "kernel/vfs/vfs.h"

#include "syscall/diag.h"
#include "syscall/fs.h"
#include "syscall/ipc.h"
#if ZOS_NS_HANDLES
#include "syscall/object.h"
#endif

/* Dispatcher input/output state (written by the asm boundary, read by C). */
uint8_t zos_syscall_number;
uint16_t zos_syscall_args;
uint8_t zos_syscall_result_a;
uint8_t zos_syscall_result_c;
uint16_t zos_syscall_result_hl;

/* Common-resident copy gate and its staging block. */
extern uint8_t zos_user_copy_staging[];

#define USER_STAGE_SIZE ZOS_SYSCALL_STAGE_SIZE

/* One transfer chunk against the caller bank, through the common gate.
 * The staging buffer is common-resident, so it survives the mapping
 * switch and stays valid while mode 10 is active. */
uint8_t zos_syscall_bank_read(uint8_t bank, uint16_t user_src,
			      uint16_t length)
{
	struct zos_bank_copy_req req;

	req.bank = bank;
	req.op = ZOS_BANK_OP_READ;
	req.addr = user_src;
	req.buffer = zos_user_copy_staging;
	req.length = length;
	return zos_bank_copy(&req);
}

uint8_t zos_syscall_bank_write(uint8_t bank, uint16_t user_dst,
			       uint16_t length)
{
	struct zos_bank_copy_req req;

	req.bank = bank;
	req.op = ZOS_BANK_OP_WRITE;
	req.addr = user_dst;
	req.buffer = zos_user_copy_staging;
	req.length = length;
	return zos_bank_copy(&req);
}

static uint16_t read_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* The result publication and argument-window helpers are shared with
 * kernel/syscall/diag.c (declared in diag.h); the M6 service bodies
 * below keep their short local names. */
void zos_syscall_success(uint16_t result)
{
	zos_syscall_result_hl = result;
	zos_syscall_result_a = 0;
	zos_syscall_result_c = 0;
}

void zos_syscall_error(uint8_t err)
{
	zos_syscall_result_hl = 0;
	zos_syscall_result_a = err;
	zos_syscall_result_c = 1;
}

/* Argument block: 0100h-1FFFh, every byte inside the caller window. */
uint8_t zos_syscall_block_ok(uint16_t addr, uint16_t len)
{
	if (addr < ZOS_SYSCALL_WINDOW_BASE ||
	    addr >= ZOS_SYSCALL_WINDOW_END) {
		return 0;
	}
	return len <= (uint16_t)(ZOS_SYSCALL_WINDOW_END - addr);
}

#define syscall_success	zos_syscall_success
#define syscall_error	zos_syscall_error
#define block_ok	zos_syscall_block_ok

/* Read/write buffer: 0100h-DFFFh, no length wrap. */
uint8_t zos_syscall_buffer_ok(uint16_t addr, uint16_t len)
{
	if (addr < ZOS_USER_BUFFER_BASE || addr >= ZOS_USER_BUFFER_END) {
		return 0;
	}
	return len <= (uint16_t)(ZOS_USER_BUFFER_END - addr);
}

static void syscall_exit(uint16_t block)
{
	uint16_t status;

	/* SYS_EXIT does not return.  A malformed block cannot be reported,
	 * so it is defined to exit with ZOS_EFAULT. */
	if (!block_ok(block, 2)) {
		process_exit(ZOS_EFAULT);
	}
	status = read_le16((const uint8_t *)block);
	process_exit(status);
}

static void syscall_getinfo(uint16_t block)
{
	uint8_t pid;
	uint8_t *record;

	if (!block_ok(block, ZOS_GETINFO_SIZE)) {
		syscall_error(ZOS_EINVAL);
		return;
	}
	pid = process_current_pid();
	if (pid >= PROCESS_TABLE_SIZE) {
		syscall_error(ZOS_EINVAL);
		return;
	}
	record = (uint8_t *)block;
	record[0] = (uint8_t)ZOS_GETINFO_SIZE;
	record[1] = (uint8_t)ZOS_GETINFO_ABI;
	record[2] = pid;
	record[3] = process_table[pid].bank;
	{
		uint16_t flags = process_table[pid].flags;

		record[4] = (uint8_t)(flags & 0xFFu);
		record[5] = (uint8_t)(flags >> 8);
	}
	syscall_success(ZOS_GETINFO_SIZE);
}

/* SYS_EXEC 05h.  Thin composition of the one process machinery: the
 * depth, frame-size and argument checks run before anything is allocated,
 * then process_prepare() does every fallible step with its own rollback
 * and process_launch() enters the child.  Past the launch nothing can
 * fail. */
static void syscall_exec(uint8_t bank, uint16_t block)
{
	struct process_child child;
	uint8_t pid = process_current_pid();
	uint16_t len;
	uint16_t args;
	uint16_t args_length;
	int result;

	(void)bank; /* the path and tail are copied through the common gate */
	if (!block_ok(block, 8)) {
		syscall_error(ZOS_EINVAL);
		return;
	}
	if (pid >= PROCESS_TABLE_SIZE || vfs_phase() != VFS_PHASE_READY) {
		syscall_error(ZOS_EBUSY);
		return;
	}
	/* Depth and frame-size refusals allocate nothing.  The preflight
	 * measures this handler's frame; the copy the launch gate takes is
	 * deeper by ZOS_EXEC_FRAME_SLACK at most (see process.h). */
	if (process_active_count() >= ZOS_EXEC_MAX_DEPTH) {
		syscall_error(ZOS_EBUSY);
		return;
	}
	len = zos_process_continuation_size();
	if (len > ZOS_EXEC_FRAME_SAVE - ZOS_EXEC_FRAME_SLACK) {
		syscall_error(ZOS_EBUSY);
		return;
	}
	/* The argument tail is validated before any allocation: a length
	 * over the startup-block maximum, or a buffer outside the caller
	 * window, is EINVAL with nothing allocated.  A zero length means no
	 * arguments and the pointer is ignored. */
	args = read_le16((const uint8_t *)(block + 4));
	args_length = read_le16((const uint8_t *)(block + 6));
	if (args_length > ZOS_STARTUP_TAIL_MAX ||
	    (args_length != 0 &&
	     !zos_syscall_buffer_ok(args, args_length))) {
		syscall_error(ZOS_EINVAL);
		return;
	}
	result = zos_fs_copy_path(read_le16((const uint8_t *)block),
				  read_le16((const uint8_t *)(block + 2)),
				  zos_process_path_scratch);
	if (result == 0) {
		result = process_prepare(zos_process_path_scratch, pid, args,
					 args_length, &child);
	}
	if (result != 0) {
		/* Malformed containers (ZEX_EXEC_ERR_* <= -0x20) are EINVAL;
		 * every other code is a native -ZOS_E* passed through. */
		syscall_error(result <= -0x20
				      ? ZOS_EINVAL
				      : (uint8_t)(uint16_t)(0u -
						(uint16_t)result));
		return;
	}
	process_launch(&child);
	syscall_success(process_last_status);
}

void kernel_syscall_dispatch(void)
{
	uint8_t bank = process_current_bank();

	/* M1 default: unknown numbers stay deterministic. */
	syscall_error(ZOS_ENOSYS);

	/* M9 native filesystem family (10h-1Fh). */
	if (zos_syscall_number >= ZOS_SYSCALL_FS_OPEN &&
	    zos_syscall_number <= ZOS_SYSCALL_FS_SYNC) {
		zos_fs_dispatch(zos_syscall_number, bank, zos_syscall_args);
		return;
	}

#if ZOS_NS_HANDLES
	/* Namespace and object-handle family (06h-09h, work package 42a;
	 * the acceptance image builds it, production switches over in 42b).
	 * The stage-3 lookup number 06h is the object `bind` number here, so
	 * the lookup pair is not reachable: `zos_ipc_dispatch` is linked but
	 * no number routes to it. */
	if (zos_syscall_number >= ZOS_SYSCALL_BIND &&
	    zos_syscall_number <= ZOS_SYSCALL_NS_ENUM) {
		zos_object_dispatch(zos_syscall_number, bank,
				    zos_syscall_args);
		return;
	}
#else
	/* Resident-service IPC family (06h/07h, package 39). */
	if (zos_syscall_number == ZOS_SYSCALL_IPC_LOOKUP ||
	    zos_syscall_number == ZOS_SYSCALL_IPC_CALL) {
		zos_ipc_dispatch(zos_syscall_number, bank, zos_syscall_args);
		return;
	}
#endif

	switch (zos_syscall_number) {
	case ZOS_SYSCALL_EXIT:
		syscall_exit(zos_syscall_args);
		break;
	case ZOS_SYSCALL_GETINFO:
		syscall_getinfo(zos_syscall_args);
		break;
	case ZOS_SYSCALL_EXEC:
		syscall_exec(bank, zos_syscall_args);
		break;
	case ZOS_SYSCALL_DIAG_MEM_READ:
		zos_diag_mem_read(zos_syscall_args);
		break;
	case ZOS_SYSCALL_DIAG_MEM_WRITE:
		zos_diag_mem_write(zos_syscall_args);
		break;
	case ZOS_SYSCALL_DIAG_IO_IN:
		zos_diag_io_in(zos_syscall_args);
		break;
	case ZOS_SYSCALL_DIAG_IO_OUT:
		zos_diag_io_out(zos_syscall_args);
		break;
	case ZOS_SYSCALL_DIAG_BANK_INFO:
		zos_diag_bank_info();
		break;
	case ZOS_SYSCALL_DIAG_SYSTEM_INFO:
		zos_diag_system_info(zos_syscall_args);
		break;
	default:
		break;
	}
}
