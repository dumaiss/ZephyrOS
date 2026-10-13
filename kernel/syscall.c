/* KonaOS native syscall dispatcher (bank 7).
 *
 * The stable register ABI (docs/abi.md) is converted to internal C state
 * by the assembly boundary kos_syscall_dispatch (kernel/syscall/entry.asm);
 * SDCC calling conventions never cross the syscall boundary.
 *
 * Process bodies here: SYS_EXIT, SYS_GETINFO and SYS_EXEC.  Console I/O is
 * not a syscall: the SDK's console functions are clients of the console
 * driver (abi/kos/protocol.h, class KOS_CLASS_CONSOLE) through SYS_CALL,
 * so the nucleus keeps no console binding at all.  Every caller byte is still validated against
 * the documented windows before it is touched:
 *
 *   - argument blocks must lie in 0100h-1FFFh (read while mode 11 is
 *     active, where only the caller window is visible);
 *   - read/write buffers may occupy 0100h-DFFFh and are transferred
 *     through the common-resident copy gates;
 *   - bank 7 and arithmetic wrap are rejected.
 *
 * The diagnostic set (SYS_DIAG_*) is implemented in
 * kernel/syscall/diag.c, the object and namespace family in
 * kernel/syscall/object.c and extended memory in kernel/syscall/xmem.c;
 * this file owns the dispatch switch and the shared result/window
 * helpers.
 *
 * Failure returns A = KOS_E* with carry set and HL = 0; success returns
 * carry clear with HL = primary result.  Process bookkeeping is never
 * damaged by a failed operation.
 */

#include "kernel.h"
#include "kos/syscall.h"
#include "kos/ipc.h"
#include "kos/seg.h"
#include "process.h"

#include "kernel/ipc/ipc.h"
#include "kernel/memory/banking.h"
#include "kernel/vfs/vfs.h"

#include "kernel/irq/irq_private.h"

#include "syscall/diag.h"
#include "syscall/object.h"
#include "syscall/xmem.h"

/* Dispatcher input/output state (written by the asm boundary, read by C). */
uint8_t kos_syscall_number;
uint16_t kos_syscall_args;
uint8_t kos_syscall_result_a;
uint8_t kos_syscall_result_c;
uint16_t kos_syscall_result_hl;

/* Common-resident copy gate and its staging block. */
extern uint8_t kos_user_copy_staging[];

#define USER_STAGE_SIZE KOS_SYSCALL_STAGE_SIZE

/* One transfer chunk against the caller bank, through the common gate.
 * The bank follows the buffer-identity rule, so the same helper serves a
 * buffer in the primary bank and one behind the mode-10 aperture.  The
 * staging buffer is common-resident, so it survives the mapping
 * switch and stays valid while mode 10 is active. */
uint8_t kos_syscall_bank_read(uint16_t user_src, uint16_t length)
{
	struct kos_bank_copy_req req;

	req.bank = kos_syscall_buffer_bank(user_src);
	req.op = KOS_BANK_OP_READ;
	req.addr = user_src;
	req.buffer = kos_user_copy_staging;
	req.length = length;
	return kos_bank_copy(&req);
}

uint8_t kos_syscall_bank_write(uint16_t user_dst, uint16_t length)
{
	struct kos_bank_copy_req req;

	req.bank = kos_syscall_buffer_bank(user_dst);
	req.op = KOS_BANK_OP_WRITE;
	req.addr = user_dst;
	req.buffer = kos_user_copy_staging;
	req.length = length;
	return kos_bank_copy(&req);
}

static uint16_t read_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* The result publication and argument-window helpers are shared with
 * kernel/syscall/diag.c (declared in diag.h); the process bodies below
 * keep their short local names. */
void kos_syscall_success(uint16_t result)
{
	kos_syscall_result_hl = result;
	kos_syscall_result_a = 0;
	kos_syscall_result_c = 0;
}

void kos_syscall_error(uint8_t err)
{
	kos_syscall_result_hl = 0;
	kos_syscall_result_a = err;
	kos_syscall_result_c = 1;
}

/* Argument block: 0100h-1FFFh, every byte inside the caller window. */
uint8_t kos_syscall_block_ok(uint16_t addr, uint16_t len)
{
	if (addr < KOS_SYSCALL_WINDOW_BASE ||
	    addr >= KOS_SYSCALL_WINDOW_END) {
		return 0;
	}
	return len <= (uint16_t)(KOS_SYSCALL_WINDOW_END - addr);
}

#define syscall_success	kos_syscall_success
#define syscall_error	kos_syscall_error
#define block_ok	kos_syscall_block_ok

/* Read/write buffer: 0100h-DFFFh, no length wrap, and under a nonzero
 * saved page never across the aperture boundary (the two sides of
 * 8000h-BFFFh are different banks). */
uint8_t kos_syscall_buffer_ok(uint16_t addr, uint16_t len)
{
	uint8_t latch;

	if (addr < KOS_USER_BUFFER_BASE || addr >= KOS_USER_BUFFER_END) {
		return 0;
	}
	if (len > (uint16_t)(KOS_USER_BUFFER_END - addr)) {
		return 0;
	}
	latch = ipc_gate_latch();
	if ((latch & KOS_MODE_MASK) == KOS_MODE_APPLICATION &&
	    (latch & KOS_ROM_PAGE_MASK) != 0u) {
		uint16_t end = (uint16_t)(addr + len);

		if (addr < KOS_XMEM_APERTURE_BASE +
				   KOS_XMEM_APERTURE_SIZE &&
		    end > KOS_XMEM_APERTURE_BASE &&
		    !(addr >= KOS_XMEM_APERTURE_BASE &&
		      end <= KOS_XMEM_APERTURE_BASE +
			     KOS_XMEM_APERTURE_SIZE)) {
			return 0;
		}
	}
	return 1;
}

/* The bank a caller buffer address resolves to (architecture 4.9/7.2,
 * buffer identity): inside the mode-10 far aperture under a nonzero saved
 * page it is the far bank 7 - P, everywhere else the running process's
 * primary bank.  Descriptor capture (kernel/ipc) applies the same rule to
 * the buffers a driver names. */
uint8_t kos_syscall_buffer_bank(uint16_t addr)
{
	uint8_t latch = ipc_gate_latch();

	if ((latch & KOS_MODE_MASK) == KOS_MODE_APPLICATION &&
	    (latch & KOS_ROM_PAGE_MASK) != 0u &&
	    addr >= KOS_XMEM_APERTURE_BASE &&
	    addr < (uint16_t)(KOS_XMEM_APERTURE_BASE +
			      KOS_XMEM_APERTURE_SIZE)) {
		return (uint8_t)(KOS_BANK_MASK -
				 (uint8_t)((latch & KOS_ROM_PAGE_MASK) >>
					   KOS_ROM_PAGE_SHIFT));
	}
	return process_current_bank();
}

static void syscall_exit(uint16_t block)
{
	uint16_t status;

	/* SYS_EXIT does not return.  A malformed block cannot be reported,
	 * so it is defined to exit with KOS_EFAULT. */
	if (!block_ok(block, 2)) {
		process_exit(KOS_EFAULT);
	}
	status = read_le16((const uint8_t *)block);
	process_exit(status);
}

static void syscall_getinfo(uint16_t block)
{
	uint8_t pid;
	uint8_t *record;

	if (!block_ok(block, KOS_GETINFO_SIZE)) {
		syscall_error(KOS_EINVAL);
		return;
	}
	pid = process_current_pid();
	if (pid >= PROCESS_TABLE_SIZE) {
		syscall_error(KOS_EINVAL);
		return;
	}
	record = (uint8_t *)block;
	record[0] = (uint8_t)KOS_GETINFO_SIZE;
	record[1] = (uint8_t)KOS_GETINFO_ABI;
	record[2] = pid;
	record[3] = process_table[pid].bank;
	{
		uint16_t flags = process_table[pid].flags;

		record[4] = (uint8_t)(flags & 0xFFu);
		record[5] = (uint8_t)(flags >> 8);
	}
	syscall_success(KOS_GETINFO_SIZE);
}

/* SYS_REBOOT 0Ch.  Mask interrupts, quiesce the interrupt sources and write
 * the reset latch; the common-resident primitive jumps to the ROM reset
 * vector, so this body never returns and no caller block is read. */
static void syscall_reboot(void)
{
	__asm__("di");
	platform_irq_quiesce(IRQ_CTC0);
	platform_reboot_now();
}

/* SYS_EXEC 05h.  Thin composition of the one process machinery: the
 * depth, frame-size and argument checks run before anything is allocated,
 * then process_prepare() does every fallible step with its own rollback
 * and process_launch() enters the child.  Past the launch nothing can
 * fail. */
static void syscall_exec(uint16_t block)
{
	struct process_child child;
	uint8_t pid = process_current_pid();
	uint16_t len;
	uint16_t args;
	uint16_t args_length;
	int result;

	if (!block_ok(block, 8)) {
		syscall_error(KOS_EINVAL);
		return;
	}
	if (pid >= PROCESS_TABLE_SIZE || vfs_phase() != VFS_PHASE_READY) {
		syscall_error(KOS_EBUSY);
		return;
	}
	/* Depth and frame-size refusals allocate nothing.  The preflight
	 * measures this handler's frame; the copy the launch gate takes is
	 * deeper by KOS_EXEC_FRAME_SLACK at most (see process.h). */
	if (process_active_count() >= KOS_EXEC_MAX_DEPTH) {
		syscall_error(KOS_EBUSY);
		return;
	}
	len = kos_process_continuation_size();
	if (len > KOS_EXEC_FRAME_SAVE - KOS_EXEC_FRAME_SLACK) {
		syscall_error(KOS_EBUSY);
		return;
	}
	/* The argument tail is validated before any allocation: a length
	 * over the startup-block maximum, or a buffer outside the caller
	 * window, is EINVAL with nothing allocated.  A zero length means no
	 * arguments and the pointer is ignored. */
	args = read_le16((const uint8_t *)(block + 4));
	args_length = read_le16((const uint8_t *)(block + 6));
	if (args_length > KOS_STARTUP_TAIL_MAX ||
	    (args_length != 0 &&
	     !kos_syscall_buffer_ok(args, args_length))) {
		syscall_error(KOS_EINVAL);
		return;
	}
	/* The caller's path is staged in the VFS workspace's path view by
	 * the namespace family's own gate (`object_normalize_path`, exactly
	 * what `bind` uses): the address travels to a bank-6 provider in
	 * the bind frame, and a provider reads it while the window shows
	 * its own bank, so it must name fixed core - never the cold half
	 * (4.2's buffer rule).  No other buffer is involved: the loader
	 * that follows reads the bound FILE through descriptors and the
	 * common staging block, and never touches the workspace. */
	vfs_ws_enter(VFS_WS_VIEW_PATH);
	result = object_normalize_path(read_le16((const uint8_t *)block),
				       read_le16((const uint8_t *)(block + 2)));
	if (result == 0) {
		result = process_prepare(VFS_WS_NORM_A, pid, args,
					 args_length, &child);
	}
	vfs_ws_leave(VFS_WS_VIEW_PATH);
	if (result != 0) {
		/* Malformed containers (EXE_EXEC_ERR_* <= -0x20) are EINVAL
		 * - except the one resource code: a container that is well
		 * formed but needs more than the machine has (no free bank
		 * for a far-code or mappable data segment, no free slot) is
		 * KOS_ENOSPC, as abi/kos/syscall.h documents for SYS_EXEC.
		 * Every other code is a native -KOS_E* passed through. */
		syscall_error(result == EXE_EXEC_ERR_NOSPACE
				      ? KOS_ENOSPC
				      : (result <= -0x20
						 ? KOS_EINVAL
						 : (uint8_t)(uint16_t)(0u -
							(uint16_t)result)));
		return;
	}
	process_launch(&child);
	syscall_success(process_last_status);
}

void kernel_syscall_dispatch(void)
{
	/* Default: an unassigned number returns KOS_ENOSYS. */
	syscall_error(KOS_ENOSYS);

	/* Object and namespace family (06h-0Bh): bind, call,
	 * unbind, ns_enum, chdir, getcwd. */
	if (kos_syscall_number >= KOS_SYSCALL_BIND &&
	    kos_syscall_number <= KOS_SYSCALL_GETCWD) {
		kos_object_dispatch(kos_syscall_number, kos_syscall_args);
		return;
	}

	/* Extended-memory family (20h-27h): alloc, free,
	 * read, write, info, map, segment info and segment map (architecture
	 * 4.8-6.7). */
	if (kos_syscall_number >= KOS_SYSCALL_XMEM_ALLOC &&
	    kos_syscall_number <= KOS_SYSCALL_SEG_MAP) {
		kos_xmem_dispatch(kos_syscall_number, kos_syscall_args);
		return;
	}

	switch (kos_syscall_number) {
	case KOS_SYSCALL_EXIT:
		syscall_exit(kos_syscall_args);
		break;
	case KOS_SYSCALL_GETINFO:
		syscall_getinfo(kos_syscall_args);
		break;
	case KOS_SYSCALL_EXEC:
		syscall_exec(kos_syscall_args);
		break;
	case KOS_SYSCALL_REBOOT:
		syscall_reboot();
		break;
	case KOS_SYSCALL_DIAG_MEM_READ:
		kos_diag_mem_read(kos_syscall_args);
		break;
	case KOS_SYSCALL_DIAG_MEM_WRITE:
		kos_diag_mem_write(kos_syscall_args);
		break;
	case KOS_SYSCALL_DIAG_IO_IN:
		kos_diag_io_in(kos_syscall_args);
		break;
	case KOS_SYSCALL_DIAG_IO_OUT:
		kos_diag_io_out(kos_syscall_args);
		break;
	case KOS_SYSCALL_DIAG_BANK_INFO:
		kos_diag_bank_info();
		break;
	case KOS_SYSCALL_DIAG_SYSTEM_INFO:
		kos_diag_system_info(kos_syscall_args);
		break;
	default:
		break;
	}
}
