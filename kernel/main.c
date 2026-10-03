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
#include "zephyros/irq.h"
#include "zephyros/console.h"
#include "zephyros/module.h"

/* Single integration seam for target acceptance builds; the runner and
 * milestone selection live in tests/target/support/target_test.c.  The
 * one unavoidable boot-phase hook (ZOS_M4_SIO_IRQ) is marked TEST-ONLY
 * HOOK below with its test-owned implementation path. */
#ifndef ZOS_TARGET_TEST_BUILD
#define ZOS_TARGET_TEST_BUILD 0
#endif

#if ZOS_TARGET_TEST_BUILD
/* TEST-ONLY HOOK: target acceptance runner; implementation and milestone
 * selection live in tests/target/support/target_test.c (acceptance images
 * only).  The early hook runs before the module catalog load; the late
 * hook runs after console binding. */
void zos_target_test_early(void);
void zos_target_test_late(void);
#endif

/* Set only by the m4-selftest acceptance build: keep CTC0 pending across
 * the boot-module load while tests/modules/sio_irq_module.c claims it. */
#ifndef ZOS_M4_SIO_IRQ
#define ZOS_M4_SIO_IRQ 0
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

#if ZOS_TARGET_TEST_BUILD
	/* TEST-ONLY HOOK: m1/m2 acceptance phases (target_test.c). */
	zos_target_test_early();
#endif

/* --- registry, module catalog, console service ---------------------
	 * The early console above stays authoritative.  The device registry
	 * is initialized first, the embedded module catalog is loaded
	 * through the generic M4 loader, and only then is the console
	 * service bound.  Load and binding diagnostics go to the early
	 * console before any dynamic device is used. */
	device_registry_init();
	{
		uint8_t module_failures;

		(void)module_loader_init();
#if ZOS_M4_SIO_IRQ
		/* TEST-ONLY HOOK: keep CTC0 pending across the load until the
		 * acceptance module owns the source; the concrete test lives in
		 * tests/target/support/target_test.c and
		 * tests/modules/sio_irq_module.c. */
		{
			uint8_t irq_enabled = irq_arch_lock();

			platform_ctc0_test_start();
			module_failures = module_catalog_load_all();
			platform_ctc0_stop();
			irq_arch_unlock(irq_enabled);
		}
#else
		module_failures = module_catalog_load_all();
#endif
		if (module_failures != 0) {
			early_console_puts("console: dynamic driver failed\r\n");
		} else if (console_bind_defaults() != ZDEV_OK) {
			early_console_puts("console: unbound\r\n");
		} else {
			static const char m4_console_ok[] =
				"console: dynamic sio0.b OK\r\n";

			if (console_write(m4_console_ok,
					  (uint16_t)(sizeof(m4_console_ok) - 1u)) !=
				    (int)(sizeof(m4_console_ok) - 1u)) {
				early_console_puts("console: service failed\r\n");
			}
		}
	}

#if ZOS_TARGET_TEST_BUILD
	/* TEST-ONLY HOOK: m3/m4 acceptance phases (target_test.c). */
	zos_target_test_late();
#endif

	__asm__("di");
	for (;;) {
		__asm__("halt");
	}
}
