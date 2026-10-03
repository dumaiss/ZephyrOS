/* M2 physical CTC0 acceptance driver.  No output occurs in IRQ context. */
#include "zephyros/kernel.h"
#include "zephyros/irq.h"

extern uint8_t zos_irq_context_test(uint8_t latch);
extern volatile uint8_t zos_irq_test_result;

static volatile uint16_t irq_test_hits;
static volatile uint8_t irq_test_handler_fault;

static void irq_test_handler(void *context)
{
	(void)context;
	if ((platform_memctl_read() & ZOS_MODE_MASK) != ZOS_MODE_OS) {
		irq_test_handler_fault = 1;
	}
	++irq_test_hits;
}

static void irq_test_fail(uint8_t code)
{
	static const char hex[] = "0123456789ABCDEF";
	early_console_puts("irq selftest: FAIL code=0x");
	early_console_putc(hex[code >> 4]);
	early_console_putc(hex[code & 0x0Fu]);
	early_console_puts("\r\n");
}

void zos_run_irq_selftest(void)
{
	uint16_t before;
	uint8_t n;

	irq_test_hits = 0;
	irq_test_handler_fault = 0;
	if (irq_claim(IRQ_CTC0, irq_test_handler, (void *)0) != IRQ_OK) {
		irq_test_fail(0x01); return;
	}
	if (irq_claim(IRQ_CTC0, irq_test_handler, (void *)0) != IRQ_EBUSY) {
		irq_test_fail(0x02); return;
	}

	platform_ctc0_test_start();
	for (n = 0; n != 3; ++n) {
		if (zos_irq_context_test(ZOS_MODE_OS) != 0) {
			platform_ctc0_stop(); irq_test_fail(0x10u + n); return;
		}
	}
	if (zos_irq_context_test(ZOS_MODE_APPLICATION | 0u) != 0) {
		platform_ctc0_stop(); irq_test_fail(0x20); return;
	}
	if (zos_irq_context_test(ZOS_MODE_APPLICATION | 6u) != 0) {
		platform_ctc0_stop(); irq_test_fail(0x21); return;
	}
	platform_ctc0_stop();
	if (irq_test_hits < 5 || irq_test_handler_fault) {
		irq_test_fail(0x30); return;
	}
	if (irq_release(IRQ_CTC0) != IRQ_OK) {
		irq_test_fail(0x31); return;
	}

	before = irq_unclaimed_count();
	platform_ctc0_test_start();
	if (zos_irq_context_test(ZOS_MODE_OS) != 0) {
		platform_ctc0_stop(); irq_test_fail(0x32); return;
	}
	platform_ctc0_stop();
	if (irq_unclaimed_count() != before + 1u || irq_last_source() != IRQ_CTC0) {
		irq_test_fail(0x33); return;
	}

	early_console_puts("irq selftest: OK\r\n");
}
