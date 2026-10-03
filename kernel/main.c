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
#include "zephyros/irq.h"
#include "zephyros/console.h"
#include "zephyros/driver.h"
#include "zephyros/m3_selftest.h"

/* M3 build options.  Both default to 0; the m3-fallback make targets set
 * them to prove that early boot/panic diagnostics stay on the independent
 * early console when the device layer is deliberately disabled. */
#ifndef ZOS_M3_NO_STATIC_DRIVERS
#define ZOS_M3_NO_STATIC_DRIVERS 0
#endif
#ifndef ZOS_M3_NO_CONSOLE_BIND
#define ZOS_M3_NO_CONSOLE_BIND 0
#endif

/* ~1 second no-op delay at kernel entry (Z80 @ 10 MHz, no wait states) so
 * the operator can connect the serial terminal before the banner prints.
 *
 * Cycle math: the inner djnz loop with B=0 runs 256 passes
 * (255*13 + 8 = 3323 cycles); one outer pass is 7 + 3323 + 6 + 4 + 4 + 12
 * = 3356 cycles (3351 on the final pass).  2979 passes plus setup totals
 * 9,997,529 cycles ≈ 1.00 s.  Purely a no-op spin; no I/O is touched.
 *
 * Not static: the host replays (tests/test_m3_sim.py) recognize this symbol
 * to skip the wall-clock delay when replaying kernel_main. */
void kernel_serial_connect_delay(void)
{
	__asm__(
		"ld de,#0x0BA3\n"	/* 2979 outer passes */
		"100$:\n"
		"ld b,#0x00\n"		/* 256 inner passes */
		"101$:\n"
		"djnz 101$\n"
		"dec de\n"
		"ld a,d\n"
		"or e\n"
		"jr nz,100$\n"
	);
}

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
	kernel_serial_connect_delay();
	kernel_assert_os_mode();
	kernel_exercise_banking();
	irq_init();

	early_console_puts("ZephyrOS 0.0\r\n");
	early_console_puts("platform: Zephyr-80\r\n");
	early_console_puts("kernel entry: OK\r\n");
	early_console_puts("serial: OK\r\n");
	early_console_puts("HALT\r\n");

#if ZOS_SYSCALL_SELFTEST
	/* Target-side RST 30h round trip; prints the selftest result. */
	zos_run_syscall_selftest();
#endif

#if ZOS_IRQ_SELFTEST
	zos_run_irq_selftest();
#endif

	/* --- M3: registry, static drivers, console service ---------------
	 * The early console above stays authoritative; the normal console
	 * service is initialized afterwards and proves itself with the
	 * distinct acceptance line.  If it cannot bind or write, the
	 * fallback diagnostic still goes through the early console. */
	device_registry_init();
#if ZOS_M3_NO_STATIC_DRIVERS
	early_console_puts("console: static drivers disabled\r\n");
#else
	driver_init_static();
#if ZOS_M3_NO_CONSOLE_BIND
	early_console_puts("console: binding disabled\r\n");
#else
	if (console_bind_defaults() != ZDEV_OK) {
		early_console_puts("console: unbound\r\n");
	} else {
		static const char m3_console_ok[] = "console: sio0.b OK\r\n";

		if (console_write(m3_console_ok,
				  (uint16_t)(sizeof(m3_console_ok) - 1u)) !=
		    (int)(sizeof(m3_console_ok) - 1u)) {
			early_console_puts("console: service failed\r\n");
		}
	}
#endif
#endif

#if ZOS_M3_SELFTEST
	zos_run_m3_selftest();
#endif

	__asm__("di");
	for (;;) {
		__asm__("halt");
	}
}
