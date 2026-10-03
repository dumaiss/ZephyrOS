/* ZephyrOS M1 kernel entry.
 *
 * Preconditions established by boot/start.asm:
 *   - interrupts disabled,
 *   - memory mode 11 (OS mode over bank 0), bank 7 visible at 2000h-DFFFh,
 *   - valid common stack,
 *   - SDCC C runtime state initialized,
 *   - early console initialized,
 *   - boot-time latch assertion passed.
 *
 * kernel_main() first re-validates the latch state that the bank-7 body
 * depends on, exercises the banking primitives, prints the M0 banner, and
 * enters the controlled halt loop.  Successful validation is silent: the
 * banner remains byte-identical to M0.  It does not return.
 */

#include "zephyros/kernel.h"
#include "zephyros/syscall.h"

static void kernel_assert_os_mode(void)
{
	uint8_t latch = platform_memctl_read();

	if ((latch & (ZOS_MODE_MASK | ZOS_BANK_MASK)) != ZOS_MODE_OS) {
		panic("mode 11 not active at kernel entry");
	}
}

/* Exercise the banking primitives.  Mode 11 keeps bank 7 visible at
 * 2000h-DFFFh regardless of the bank field, so bank updates are safe
 * here and must preserve the mode/page bits and reject bank 7. */
static void kernel_exercise_banking(void)
{
	uint8_t bank = platform_bank_get();
	uint8_t other = (bank == 0) ? 1 : 0;

	platform_bank_set(other);
	if (platform_bank_get() != other) {
		panic("bank update failed");
	}
	platform_bank_set(bank);
	if (platform_bank_get() != bank) {
		panic("bank restore failed");
	}
	platform_bank_set(7);
	if (platform_bank_get() != bank) {
		panic("bank 7 not rejected");
	}
}

void kernel_main(void)
{
	kernel_assert_os_mode();
	kernel_exercise_banking();

	early_console_puts("ZephyrOS 0.0\r\n");
	early_console_puts("platform: Zephyr-80\r\n");
	early_console_puts("kernel entry: OK\r\n");
	early_console_puts("serial: OK\r\n");
	early_console_puts("HALT\r\n");

#if ZOS_SYSCALL_SELFTEST
	/* Target-side RST 30h round trip; prints the selftest result. */
	zos_run_syscall_selftest();
#endif

	__asm__("di");
	for (;;) {
		__asm__("halt");
	}
}
