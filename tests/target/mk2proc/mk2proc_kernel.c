/* Zephyr-80 Mk2 processes image: the kernel side (test-only;
 * tests/test_mk2_processes.py).
 *
 * The role of kernel/main.c and kernel/syscall.c for the image: the
 * kernel launcher, a syscall dispatcher for EXIT, EXEC and the
 * extended-memory family, the dispatcher's result and window helpers,
 * and empty hooks for the subsystems the image does not build (the VFS,
 * the real-time interrupt).  The process lifecycle, the page loader, the
 * page allocator and the extended-memory bodies are the kernel's own
 * sources built for Mk2.
 *
 * EXEC keeps the ABI block {path, length, args, args_length}; the image
 * has one program (mk2proc_program, in ROM), so the path is not read.
 */

#include "kos/syscall.h"
#include "kos/errno.h"
#include "kos/xmem.h"
#include "kos/seg.h"
#include "process.h"
#include "kernel.h"
#include "kernel/syscall/diag.h"
#include "kernel/syscall/xmem.h"
#include "kernel/memory/mk2/pages.h"
#include "kernel/vfs/vfs.h"
#include "romfs.h"

extern const struct process_program mk2proc_program;
extern const uint8_t mk2proc_nucleus[];

/* The dispatcher boundary's state (kernel/syscall/entry.asm). */
uint8_t kos_syscall_number;
uint16_t kos_syscall_args;
uint8_t kos_syscall_result_a;
uint8_t kos_syscall_result_c;
uint16_t kos_syscall_result_hl;

/* The kernel launcher's record: the depth-1 program's exit status. */
uint16_t mk2proc_launcher_status;
uint8_t mk2proc_launcher_error;

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

uint8_t kos_syscall_block_ok(uint16_t addr, uint16_t len)
{
	if (addr < KOS_SYSCALL_WINDOW_BASE ||
	    addr >= KOS_SYSCALL_WINDOW_END) {
		return 0;
	}
	return len <= (uint16_t)(KOS_SYSCALL_WINDOW_END - addr);
}

/* The Mk2 buffer window: 0100h-DFFFh with no wrap.  A buffer is resolved
 * page by page through the caller's mapping, so it may cross the far
 * window's edges. */
uint8_t kos_syscall_buffer_ok(uint16_t addr, uint16_t len)
{
	if (addr < KOS_USER_BUFFER_BASE || addr >= KOS_USER_BUFFER_END) {
		return 0;
	}
	return len <= (uint16_t)(KOS_USER_BUFFER_END - addr);
}

void vfs_process_exit(uint8_t pid)
{
	(void)pid;
}

/* The EXE container loader's VFS and store calls (kernel/process/exec.c):
 * the image loads its one program with process_prepare_program, so the
 * container path finds no file. */
uint8_t kos_user_copy_staging[KOS_SYSCALL_STAGE_SIZE];

uint8_t vfs_owner_current(void)
{
	return process_current_pid();
}

int vfs_ns_bind(uint8_t pid, const char *norm, uint16_t flags,
		uint8_t *type)
{
	(void)pid;
	(void)norm;
	(void)flags;
	(void)type;
	return -KOS_ENOENT;
}

int vfs_ns_unbind(uint8_t pid, uint8_t handle)
{
	(void)pid;
	(void)handle;
	return -KOS_EINVAL;
}

struct vfs_handle_entry *vfs_handle_find(uint8_t pid, uint8_t number)
{
	(void)pid;
	(void)number;
	return (struct vfs_handle_entry *)0;
}

int16_t vfs_ns_file_call(uint8_t handle, uint8_t object,
			 struct kos_ipc_frame *frame, const uint8_t *map,
			 uint16_t addr, uint16_t length)
{
	(void)handle;
	(void)object;
	(void)frame;
	(void)map;
	(void)addr;
	(void)length;
	return -KOS_EINVAL;
}

int vfs_process_start(uint8_t pid, uint8_t parent_pid)
{
	(void)pid;
	(void)parent_pid;
	return 0;
}

int romfs_read_pages(const romfs_file_t *file, uint32_t offset,
		     const uint8_t *dst_map, uint16_t dst, uint16_t length)
{
	(void)file;
	(void)offset;
	(void)dst_map;
	(void)dst;
	(void)length;
	return -KOS_EINVAL;
}

void kos_rt_release_process(uint8_t pid)
{
	(void)pid;
}

static uint16_t read_le16(uint16_t addr)
{
	const uint8_t *p = (const uint8_t *)addr;

	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* SYS_EXEC: kernel/syscall.c's preflight (depth, frame, tail), then the
 * page loader and the launch. */
static void mk2proc_exec(uint16_t block)
{
	struct process_child child;
	uint8_t pid = process_current_pid();
	uint16_t args;
	uint16_t args_length;
	int result;

	if (!kos_syscall_block_ok(block, 8)) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	if (pid >= PROCESS_TABLE_SIZE ||
	    process_active_count() >= KOS_EXEC_MAX_DEPTH ||
	    kos_process_continuation_size() >
		    KOS_EXEC_FRAME_SAVE - KOS_EXEC_FRAME_SLACK) {
		kos_syscall_error(KOS_EBUSY);
		return;
	}
	args = read_le16((uint16_t)(block + 4u));
	args_length = read_le16((uint16_t)(block + 6u));
	if (args_length > KOS_STARTUP_TAIL_MAX ||
	    (args_length != 0u &&
	     !kos_syscall_buffer_ok(args, args_length))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	result = process_prepare_program(&mk2proc_program, pid, args,
					 args_length, &child);
	if (result != 0) {
		kos_syscall_error((uint8_t)(uint16_t)(0u - (uint16_t)result));
		return;
	}
	process_launch(&child);
	kos_syscall_success(process_last_status);
}

void kernel_syscall_dispatch(void)
{
	uint16_t block = kos_syscall_args;

	kos_syscall_error(KOS_ENOSYS);
	if (kos_syscall_number == KOS_SYSCALL_EXIT) {
		if (!kos_syscall_block_ok(block, 2)) {
			process_exit(KOS_EFAULT);
		}
		process_exit(read_le16(block));
	}
	if (kos_syscall_number == KOS_SYSCALL_EXEC) {
		mk2proc_exec(block);
		return;
	}
	if (kos_syscall_number >= KOS_SYSCALL_XMEM_ALLOC &&
	    kos_syscall_number <= KOS_SYSCALL_SEG_MAP) {
		kos_xmem_dispatch(kos_syscall_number, block);
	}
}

/* The kernel launcher (depth 0): the page allocator from the build's
 * nucleus list, then the program at depth 1. */
void mk2proc_main(void)
{
	struct process_child child;
	int result;

	kos_pages_init(mk2proc_nucleus);
	result = process_prepare_program(&mk2proc_program,
					 PROCESS_KERNEL_PARENT, 0u, 0u, &child);
	if (result != 0) {
		mk2proc_launcher_error = (uint8_t)(uint16_t)(0u -
							     (uint16_t)result);
		return;
	}
	process_launch(&child);
	mk2proc_launcher_status = process_last_status;
}
