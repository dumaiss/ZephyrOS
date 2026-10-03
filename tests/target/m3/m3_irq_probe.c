/* ZephyrOS M3 target-test CTC0 probe (test artifact).
 *
 * Proves the M3 IRQ path through the M2 interrupt executive without the
 * retired static driver table: the test claims IRQ_CTC0 directly through
 * irq_claim(), waits for repeated deliveries on its bank-7 handler, checks
 * mode-11 mapping and bank-7 ISR stack placement, then releases the source
 * and verifies the executive reports it unowned again.
 *
 * The handler performs only bounded work (counters + two checks); the
 * executive owns RETI and the daisy chain.
 */

#include "zephyros/irq.h"
#include "zephyros/platform.h"

extern uint16_t zos_m3_probe_sp(void);

volatile uint16_t m3_irq_probe_hits;
volatile uint8_t m3_irq_probe_evidence;

#define M3_PROBE_EVIDENCE_MODE11   0x01u
#define M3_PROBE_EVIDENCE_ISR_STK  0x02u
#define M3_PROBE_TARGET_HITS       3u

static uint8_t m3_probe_sp_is_isr(void)
{
	uint16_t sp = zos_m3_probe_sp();

	return (uint8_t)(sp >= ZOS_IRQ_ISR_STACK_BASE &&
			 sp <= ZOS_IRQ_ISR_STACK_TOP);
}

/* Test-owned CTC0 handler (registered by m3_irq_probe_run).  Not static
 * so tools/mapcheck.py can prove the handler symbol is bank-7 resident
 * (the M2 executive maps bank 7 before dispatching). */
void m3_irq_probe_handler(void *context)
{
	(void)context;
	++m3_irq_probe_hits;
	if ((platform_memctl_read() & ZOS_MODE_MASK) == ZOS_MODE_OS) {
		m3_irq_probe_evidence |= M3_PROBE_EVIDENCE_MODE11;
	}
	if (m3_probe_sp_is_isr()) {
		m3_irq_probe_evidence |= M3_PROBE_EVIDENCE_ISR_STK;
	}
}

/* Bounded interrupt wait: CTC0 at ~152 Hz on hardware; the spin plus the
 * pacing of the executive means the target count is reached in tens of
 * milliseconds.  The bound only exists so a dead source reports failure
 * instead of hanging.
 *
 * The M2 executive deliberately returns with maskable interrupts
 * disabled (no nesting), so the wait re-arms the CPU each iteration;
 * between deliveries this is a no-op.  Exported so the host replay can
 * watch this exact code region for EI windows. */
int m3_irq_probe_wait(uint8_t target)
{
	uint16_t spins;

	platform_ctc0_test_start();
	for (spins = 0; spins < 0x8000u && m3_irq_probe_hits < target;
	     ++spins) {
		__asm__("ei");
	}
	__asm__("di");
	platform_ctc0_stop();

	if (m3_irq_probe_hits < target) {
		return -1;
	}
	if (m3_irq_probe_evidence !=
	    (M3_PROBE_EVIDENCE_MODE11 | M3_PROBE_EVIDENCE_ISR_STK)) {
		return -1;
	}
	return 0;
}

int m3_irq_probe_run(void)
{
	m3_irq_probe_hits = 0;
	m3_irq_probe_evidence = 0;
	if (irq_claim(IRQ_CTC0, m3_irq_probe_handler, (void *)0) !=
	    IRQ_OK) {
		return -1;
	}
	/* One-owner rule: a second claim of the same source is refused. */
	if (irq_claim(IRQ_CTC0, m3_irq_probe_handler, (void *)0) !=
	    IRQ_EBUSY) {
		return -1;
	}
	if (m3_irq_probe_wait(M3_PROBE_TARGET_HITS) != 0) {
		return -1;
	}
	if (irq_release(IRQ_CTC0) != IRQ_OK) {
		return -1;
	}
	/* After release the executive must report the source as unowned. */
	if (irq_release(IRQ_CTC0) != IRQ_ENOENT) {
		return -1;
	}
	return 0;
}
