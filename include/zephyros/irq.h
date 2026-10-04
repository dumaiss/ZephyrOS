/* ZephyrOS M2 interrupt contract (module-facing types and result codes).
 *
 * This header is what a ZMOD module and the driver ABI need: the
 * `irq_source_t`/`irq_handler_t` types, the logical source enum, and the
 * claim/release result codes.  The executive lifecycle and registry
 * controls are kernel-private (kernel/irq/irq_private.h). */
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

/* The executive lifecycle, the claim transaction (checkpoint, rollback,
 * owner), the dispatch diagnostics, the platform vector/quiesce/test
 * hooks, and the interrupt-state lock are kernel-private and live in
 * kernel/irq/irq_private.h.  A ZMOD module uses only the loader's
 * `zdriver_api.irq_claim`/`irq_release` callbacks. */

#endif
