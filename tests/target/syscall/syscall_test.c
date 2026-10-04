/* ZephyrOS syscall-gate acceptance test (test artifact).
 *
 * Runs the RST 30h smoke caller in application banks 0 and 6 through the
 * real syscall gate and reports the outcome on the early console.  Linked
 * only into the subsystem acceptance image (ZOS_SUBSYS_SELFTEST); the
 * production dispatcher (kernel/syscall.c) contains none of this.
 */

#include "zephyros/kernel.h"

#include "syscall_selftest.h"

/* Common transition state owned by the test harness
 * (tests/target/syscall/syscall_entry.asm). */
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
