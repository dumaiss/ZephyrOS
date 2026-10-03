/* Test-only static IRQ probe driver (ZOS_M3_SELFTEST build only).
 *
 * Proves the M3 driver path into the M2 interrupt executive:
 *
 *   - its init claims IRQ_CTC0 through the zdriver_api.irq_claim entry
 *     (the same entry every static driver gets),
 *   - m3_irq_probe_exercise() starts CTC0, enables interrupts, and waits
 *     for repeated deliveries on the bank-7 handler,
 *   - the handler records mode-11 mapping and bank-7 ISR stack placement,
 *   - the source is released afterwards, and the executive reports it as
 *     unowned again.
 *
 * The handler performs only bounded work (counters + two checks); the
 * executive owns RETI and the daisy chain.
 */

#include "zephyros/driver.h"
#include "zephyros/platform.h"

extern uint16_t zos_m3_probe_sp(void);

static const struct zdriver_api *m3_probe_api;

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

/* Registered CTC0 handler.  Not static so tools/mapcheck.py can prove the
 * handler symbol is bank-7 resident (the M2 executive maps bank 7 before
 * dispatching). */
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

static int m3_irq_probe_driver_init(const struct zdriver_api *api)
{
	m3_probe_api = api;
	m3_irq_probe_hits = 0;
	m3_irq_probe_evidence = 0;
	if (api->irq_claim(IRQ_CTC0, m3_irq_probe_handler, (void *)0) !=
	    IRQ_OK) {
		return ZDRIVER_FAIL;
	}
	return ZDRIVER_OK;
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

int m3_irq_probe_exercise(void)
{
	if (m3_probe_api == (const struct zdriver_api *)0) {
		return -1;
	}
	/* The driver claimed CTC0 during init; a second claim must be
	 * refused by the one-owner executive. */
	if (m3_probe_api->irq_claim(IRQ_CTC0, m3_irq_probe_handler,
				    (void *)0) != IRQ_EBUSY) {
		return -1;
	}
	if (m3_irq_probe_wait(M3_PROBE_TARGET_HITS) != 0) {
		return -1;
	}
	if (m3_probe_api->irq_release(IRQ_CTC0) != IRQ_OK) {
		return -1;
	}
	/* After release the executive must report the source as unowned. */
	if (m3_probe_api->irq_release(IRQ_CTC0) != IRQ_ENOENT) {
		return -1;
	}
	return 0;
}

const struct zdriver m3_irq_probe_driver = {
	"test.irqprobe",
	ZDRIVER_ABI_VERSION,
	m3_irq_probe_driver_init
};
