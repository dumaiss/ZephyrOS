/* ZephyrOS M2 kernel-owned interrupt executive. */
#ifndef ZEPHYROS_IRQ_H
#define ZEPHYROS_IRQ_H

#include "zephyros/types.h"

typedef uint8_t irq_source_t;
typedef void (*irq_handler_t)(void *context);

enum {
	IRQ_CTC0 = 0,
	IRQ_CTC1,
	IRQ_CTC2,
	IRQ_CTC3,
	IRQ_SIO0,
	IRQ_SIO1,
	IRQ_VDP_FF,
	IRQ_SOURCE_COUNT
};

#define IRQ_OK       0
#define IRQ_EINVAL  -1
#define IRQ_EBUSY   -2
#define IRQ_ENOENT  -3

void irq_init(void);
int irq_claim(irq_source_t source, irq_handler_t handler, void *context);
int irq_release(irq_source_t source);

uint16_t irq_unexpected_count(void);
uint16_t irq_unclaimed_count(void);
irq_source_t irq_last_source(void);

/* Platform-private hooks used by the executive and M2 acceptance image. */
uint8_t platform_irq_vector(irq_source_t source);
void platform_irq_quiesce(irq_source_t source);
void platform_ctc0_test_start(void);
void platform_ctc0_stop(void);

/* Internal interrupt-state lock used by kernel subsystems that mutate
 * shared state (registry, drivers).  irq_arch_lock() disables maskable
 * interrupts and returns the previous IFF state; irq_arch_unlock()
 * re-enables only when that state was enabled.  Both are common-resident
 * assembly helpers of the M2 executive, not a driver ABI. */
uint8_t irq_arch_lock(void);
void irq_arch_unlock(uint8_t enabled);

#if ZOS_IRQ_SELFTEST
void zos_run_irq_selftest(void);
#endif

#endif
