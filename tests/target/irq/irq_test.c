/* ZephyrOS IRQ-executive acceptance test (test artifact; linked only in
 * acceptance image builds).
 *
 * Runs the full interrupt-executive acceptance surface:
 *
 *   - registry semantics (invalid source, invalid handler, duplicate
 *     claim, release, double release),
 *   - mode-10 delivery from application banks 0 and 6 with the smoke
 *     caller's stack above 2000h (three deliveries per bank, handler
 *     evidence, counters, clean CTC0 teardown),
 *   - mode-11 delivery on the kernel stack, and
 *   - the unclaimed-source path (released CTC0, executive quiesce).
 *
 * Prints `irq selftest: OK` only after every check passes.  Nothing is
 * printed from IRQ context; the registered handler only records compact
 * evidence in common memory.
 */

#include "zephyros/irq.h"
#include "zephyros/kernel.h"
#include "zephyros/platform.h"

#include "kernel/irq/irq_private.h"
#include "kernel/memory/banking.h"

#include "ctc_test.h"
#include "irq_selftest.h"

/* Common test state owned by tests/target/irq/irq_entry.asm. */
extern volatile uint8_t zos_irq_test_result;
extern volatile uint8_t zos_irq_test_delivered;
extern volatile uint8_t zos_irq_test_evidence;
extern void zos_irq_selftest_run(uint8_t bank);
extern void zos_irq_test_mode11(void);
extern uint8_t zos_irq_test_sp_is_isr(void);

static const char zos_irq_hex_digits[] = "0123456789ABCDEF";

static void irq_selftest_report_fail(uint8_t code)
{
	early_console_puts("irq selftest: FAIL code=0x");
	early_console_putc(zos_irq_hex_digits[code >> 4]);
	early_console_putc(zos_irq_hex_digits[code & 0x0F]);
	early_console_puts("\r\n");
}

/* Registered CTC0 handler.  Runs in mode 11 on the bank-7 ISR stack and
 * records compact evidence only (mode-11 mapping + ISR stack placement). */
static void irq_selftest_handler(void *context)
{
	(void)context;
	++zos_irq_test_delivered;
	if ((platform_memctl_read() & ZOS_MODE_MASK) == ZOS_MODE_OS) {
		zos_irq_test_evidence |= 0x01;
	}
	if (zos_irq_test_sp_is_isr()) {
		zos_irq_test_evidence |= 0x02;
	}
}

void zos_run_irq_selftest(void)
{
	uint8_t bank;

	/* --- registry semantics --- */
	if (irq_claim(IRQ_SOURCE_COUNT, irq_selftest_handler, (void *)0) != IRQ_EINVAL)
		goto fail_registry;
	if (irq_claim(IRQ_CTC0, (irq_handler_t)0, (void *)0) != IRQ_EINVAL)
		goto fail_registry;
	if (irq_claim(IRQ_CTC0, (irq_handler_t)0x1000, (void *)0) != IRQ_EINVAL)
		goto fail_registry;	/* handler below the bank-7 code region */
	if (irq_release(IRQ_CTC0) != IRQ_ENOENT)
		goto fail_registry;
	if (irq_release(IRQ_SOURCE_COUNT) != IRQ_EINVAL)
		goto fail_registry;
	if (irq_claim(IRQ_CTC0, irq_selftest_handler, (void *)0) != IRQ_OK)
		goto fail_registry;
	if (irq_claim(IRQ_CTC0, irq_selftest_handler, (void *)0) != IRQ_EBUSY)
		goto fail_registry;
	if (irq_release(IRQ_CTC0) != IRQ_OK)
		goto fail_registry;
	if (irq_claim(IRQ_CTC0, irq_selftest_handler, (void *)0) != IRQ_OK)
		goto fail_registry;

	/* --- mode-10 delivery: application banks 0 and 6 --- */
	for (bank = 0; bank < 7; bank += 6) {
		zos_ctc0_test_start();
		zos_irq_test_result = ZOS_IST_RESULT_ARMED;
		zos_irq_test_delivered = 0;
		zos_irq_test_evidence = 0;
		zos_irq_selftest_run(bank);
		if (zos_irq_test_result != ZOS_IST_OK) {
			irq_selftest_report_fail(zos_irq_test_result);
			return;
		}
		if (zos_irq_test_delivered < 3) {
			irq_selftest_report_fail(ZOS_IST_FAIL_REPEAT);
			return;
		}
		if (zos_irq_test_evidence != 0x03) {
			irq_selftest_report_fail(ZOS_IST_FAIL_EVIDENCE);
			return;
		}
		if (irq_last_source() != IRQ_CTC0 ||
		    irq_unexpected_count() != 0 ||
		    irq_unclaimed_count() != 0) {
			irq_selftest_report_fail(ZOS_IST_FAIL_COUNTERS);
			return;
		}
	}
	zos_ctc0_test_stop();

	/* --- mode-11 delivery + the unclaimed-source path --- */
	if (irq_release(IRQ_CTC0) != IRQ_OK)
		goto fail_registry;
	zos_ctc0_test_start();
	zos_irq_test_result = ZOS_IST_RESULT_ARMED;
	zos_irq_test_mode11();
	if (zos_irq_test_result != ZOS_IST_OK) {
		irq_selftest_report_fail(zos_irq_test_result);
		return;
	}
	if (irq_unclaimed_count() != 1) {
		irq_selftest_report_fail(ZOS_IST_FAIL_UNCLAIMED);
		return;
	}
	if (irq_last_source() != IRQ_CTC0 || irq_unexpected_count() != 0) {
		irq_selftest_report_fail(ZOS_IST_FAIL_COUNTERS);
		return;
	}
	zos_ctc0_test_stop();

	/* --- clean teardown: no source may remain claimed --- */
	if (irq_release(IRQ_CTC0) != IRQ_ENOENT)
		goto fail_registry;

	early_console_puts("irq selftest: OK\r\n");
	return;

fail_registry:
	irq_selftest_report_fail(ZOS_IST_FAIL_REGISTRY);
}
