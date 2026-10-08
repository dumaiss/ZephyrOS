/* ZephyrOS resident-service spike: the IRQ-owning service (test only).
 *
 * Proves that a resident service can own a hardware IRQ without owning the
 * vectoring: the service claims CTC0 through the nucleus API and hands it
 * a handler that lives in bank 6.  The nucleus keeps the IM2 page, the
 * transition code, the ISR stack and the final RETI; the service handler
 * runs in the service domain on the nucleus ISR stack and must use no IPC
 * (here it only touches its own relocated state).
 *
 * SPIN keeps the domain open with interrupts enabled so the timer really
 * fires while a service call is in progress.
 */

#include "zephyros/errno.h"

#include "rsvc_test.h"

static const struct rsvc_api *api;
static volatile uint16_t irq_runs;
static volatile uint8_t irq_flag;

/* Runs from the IRQ executive, in the service domain, on the nucleus ISR
 * stack.  Restricted API only: no IPC, no descriptor copies. */
static void irqx_handler(void *context)
{
	(void)context;
	irq_runs = (uint16_t)(irq_runs + 1u);
	irq_flag = 0xA5u;
}

static int16_t irqx_spin(struct rsvc_frame *frame)
{
	uint16_t iterations = frame->arg1;

	irq_flag = 0u;
	while (iterations != 0u) {
		if (irq_flag != 0u) {
			break;
		}
		--iterations;
	}
	api->note(RSVC_NOTE_SERVICE_MARK, irq_runs);
	return (int16_t)irq_runs;
}

static int16_t irqx_dispatch(struct rsvc_frame *frame)
{
	api->note(RSVC_NOTE_SERVICE_CALLS, 1u);
	if (frame->op != RSVC_OP_IRQX_SPIN) {
		return -ZOS_EINVAL;
	}
	return irqx_spin(frame);
}

void rsvc_irqx_init(const struct rsvc_api *table)
{
	api = table;
	(void)table->register_ep(RSVC_CLASS_IRQX, 0u, (uint16_t)irqx_dispatch, 0u, 1u);
	(void)table->irq_claim(0u /* IRQ_CTC0 */, (uint16_t)irqx_handler, 0u);
}
