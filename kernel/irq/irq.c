/* M2 fixed-source, one-owner IRQ registry and bank-7 dispatcher. */
#include "zephyros/irq.h"
#include "zephyros/module.h"

extern volatile uint8_t irq_active_source;
extern uint8_t irq_kernel_code_end;
extern uint8_t irq_arch_lock(void);
extern void irq_arch_unlock(uint8_t enabled);
extern void irq_arch_init(void);

static irq_handler_t handlers[IRQ_SOURCE_COUNT];
static void *contexts[IRQ_SOURCE_COUNT];
static const void *owners[IRQ_SOURCE_COUNT];
static uint16_t claim_serial[IRQ_SOURCE_COUNT];
static uint16_t next_claim_serial;
static const void *current_owner;
static uint16_t unexpected;
static uint16_t unclaimed;
static irq_source_t last_source;

/* A handler address is valid when it lies inside linked bank-7 kernel
 * code or inside a committed (RESIDENT) module image.  The module
 * currently running its init may claim handlers that point into its own
 * LOADING allocation; failed/freed and out-of-arena addresses are
 * rejected. */
static uint8_t handler_is_valid(irq_handler_t handler)
{
	uint16_t address = (uint16_t)handler;
	uint16_t code_end = (uint16_t)&irq_kernel_code_end;

	if (handler != (irq_handler_t)0 &&
	    address >= 0x2000u && address < code_end) {
		return 1;
	}
	return module_code_allowed(address);
}

void irq_init(void)
{
	uint8_t source;

	for (source = 0; source < IRQ_SOURCE_COUNT; ++source) {
		handlers[source] = (irq_handler_t)0;
		contexts[source] = (void *)0;
		owners[source] = (const void *)0;
		claim_serial[source] = 0;
	}
	next_claim_serial = 1;
	current_owner = (const void *)0;
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
		owners[source] = current_owner;
		claim_serial[source] = next_claim_serial++;
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
		owners[source] = (const void *)0;
		claim_serial[source] = 0;
	}
	irq_arch_unlock(enabled);
	return result;
}

irq_checkpoint_t irq_registry_checkpoint(void)
{
	uint8_t enabled = irq_arch_lock();
	irq_checkpoint_t checkpoint = next_claim_serial;

	irq_arch_unlock(enabled);
	return checkpoint;
}

int irq_registry_rollback(irq_checkpoint_t checkpoint)
{
	uint8_t source;
	uint8_t enabled = irq_arch_lock();
	int removed = 0;

	for (source = 0; source < IRQ_SOURCE_COUNT; ++source) {
		/* Wrap-safe "claimed at or after checkpoint" comparison. */
		if (handlers[source] != (irq_handler_t)0 &&
		    (uint16_t)(claim_serial[source] - checkpoint) < 0x8000u) {
			handlers[source] = (irq_handler_t)0;
			contexts[source] = (void *)0;
			owners[source] = (const void *)0;
			claim_serial[source] = 0;
			++removed;
		}
	}
	irq_arch_unlock(enabled);
	return removed;
}

void irq_registry_set_owner(const void *owner)
{
	current_owner = owner;
}

const void *irq_registry_owner(void)
{
	return current_owner;
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
