/* ZephyrOS M1 native syscall dispatcher (bank 7).
 *
 * The stable register ABI (docs/abi.md) is converted to internal C state
 * by the assembly boundary zos_syscall_dispatch (kernel/syscall/entry.asm);
 * SDCC calling conventions never cross the syscall boundary.
 *
 * M1 policy: every syscall number returns ZOS_ENOSYS with HL = 0000h and
 * carry set.  No syscall registration or useful services exist yet.
 */

#include "zephyros/kernel.h"
#include "zephyros/syscall.h"

/* Dispatcher input/output state (written by the asm boundary, read by C). */
uint8_t zos_syscall_number;
uint16_t zos_syscall_args;
uint8_t zos_syscall_result_a;
uint16_t zos_syscall_result_hl;

void kernel_syscall_dispatch(void)
{
	(void)zos_syscall_number;
	(void)zos_syscall_args;

	zos_syscall_result_a = ZOS_ENOSYS;
	zos_syscall_result_hl = 0;
}
