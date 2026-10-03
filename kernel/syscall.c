/* ZephyrOS M1 native syscall dispatcher (bank 7).
 *
 * The stable register ABI (docs/abi.md) is converted to internal C state
 * by the assembly boundary zos_syscall_dispatch (syscall_entry.asm); SDCC
 * calling conventions never cross the syscall boundary.
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

#if ZOS_SYSCALL_SELFTEST

#include "zephyros/selftest.h"

/* Common transition state owned by the self-test harness (selftest.asm). */
extern uint8_t zos_selftest_result;
extern uint8_t zos_selftest_bank;
extern void zos_selftest_run(uint8_t bank);

static const char zos_hex_digits[] = "0123456789ABCDEF";

static void selftest_report_fail(uint8_t bank, uint8_t code)
{
	early_console_puts("syscall selftest: FAIL bank=");
	early_console_putc((char)('0' + bank));
	early_console_puts(" code=0x");
	early_console_putc(zos_hex_digits[code >> 4]);
	early_console_putc(zos_hex_digits[code & 0x0F]);
	early_console_puts("\r\n");
}

/* Runs the smoke caller in application banks 0 and 6 and reports. */
void zos_run_syscall_selftest(void)
{
	zos_selftest_run(0);
	if (zos_selftest_result != ZOS_ST_OK) {
		selftest_report_fail(0, zos_selftest_result);
		return;
	}
	zos_selftest_run(6);
	if (zos_selftest_result != ZOS_ST_OK) {
		selftest_report_fail(6, zos_selftest_result);
		return;
	}
	early_console_puts("syscall selftest: OK\r\n");
}

#endif /* ZOS_SYSCALL_SELFTEST */
