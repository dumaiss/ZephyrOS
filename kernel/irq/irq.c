/* M2 fixed-source, one-owner IRQ registry and bank-7 dispatcher. */
#include "zephyros/irq.h"

extern volatile uint8_t irq_active_source;
extern uint8_t irq_kernel_code_end;
extern uint8_t irq_arch_lock(void);
extern void irq_arch_unlock(uint8_t enabled);
extern void irq_arch_init(void);

static irq_handler_t handlers[IRQ_SOURCE_COUNT];
static void *contexts[IRQ_SOURCE_COUNT];
static uint16_t unexpected;
static uint16_t unclaimed;
static irq_source_t last_source;

static uint8_t handler_is_valid(irq_handler_t handler)
{
	uint16_t address = (uint16_t)handler;
	uint16_t code_end = (uint16_t)&irq_kernel_code_end;

	return handler != (irq_handler_t)0 && address >= 0x2000u && address < code_end;
}

void irq_init(void)
{
	uint8_t source;

	for (source = 0; source < IRQ_SOURCE_COUNT; ++source) {
		handlers[source] = (irq_handler_t)0;
		contexts[source] = (void *)0;
	}
	unexpected = 0;
	unclaimed = 0;
	last_source = IRQ_SOURCE_COUNT;
	irq_arch_init();
}

int irq_claim(irq_source_t source, irq_handler_t handler, void *context)
{
	uint8_t enabled;
	int result = IRQ_OK;

	if (source >= IRQ_SOURCE_COUNT || !handler_is_valid(handler)) {
		return IRQ_EINVAL;
	}
	enabled = irq_arch_lock();
	if (handlers[source] != (irq_handler_t)0) {
		result = IRQ_EBUSY;
	} else {
		contexts[source] = context;
		handlers[source] = handler;
	}
	irq_arch_unlock(enabled);
	return result;
}

int irq_release(irq_source_t source)
{
	uint8_t enabled;
	int result = IRQ_OK;

	if (source >= IRQ_SOURCE_COUNT) {
		return IRQ_EINVAL;
	}
	enabled = irq_arch_lock();
	if (handlers[source] == (irq_handler_t)0) {
		result = IRQ_ENOENT;
	} else {
		handlers[source] = (irq_handler_t)0;
		contexts[source] = (void *)0;
	}
	irq_arch_unlock(enabled);
	return result;
}

/* Called only by the common assembly gate, in mode 11 on the ISR stack. */
void irq_dispatch(void)
{
	irq_source_t source = irq_active_source;
	irq_handler_t handler;

	last_source = source;
	if (source >= IRQ_SOURCE_COUNT) {
		++unexpected;
		return;
	}
	handler = handlers[source];
	if (handler == (irq_handler_t)0) {
		++unclaimed;
		platform_irq_quiesce(source);
		return;
	}
	handler(contexts[source]);
}

uint16_t irq_unexpected_count(void) { return unexpected; }
uint16_t irq_unclaimed_count(void) { return unclaimed; }
irq_source_t irq_last_source(void) { return last_source; }
