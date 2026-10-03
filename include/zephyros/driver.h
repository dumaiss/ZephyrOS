/* ZephyrOS driver module contract (internal kernel/module ABI).
 *
 * A ZMOD driver module implements an init entry that the loader calls
 * with the single `zdriver_api` block.  The API exposes only three kernel
 * services:
 *
 *   device_register   publish one device instance
 *   irq_claim         claim one M2 logical IRQ source (bank-7 handler)
 *   irq_release       release a previously claimed source
 *
 * Drivers never modify I, the IM2 vector page, the memory mode, IRQ
 * stacks, or execute RETI; the M2 executive owns all of that.
 *
 * M3 used this same API through an explicit compile-time static driver
 * table (`struct zdriver`, `driver_init_static()`); that mechanism is
 * retired.  Since M4 the kernel boots only embedded ZMOD drivers and no
 * static driver table is linked into production images.  The static
 * design is kept as historical milestone documentation in
 * docs/architecture.md and docs/devices.md.
 */

#ifndef ZEPHYROS_DRIVER_H
#define ZEPHYROS_DRIVER_H

#include "zephyros/types.h"
#include "zephyros/device.h"
#include "zephyros/irq.h"

/* Driver ABI version accepted by this kernel build. */
#define ZDRIVER_ABI_VERSION 1u

/* Driver init convention: 0 = success, negative = failure. */
#define ZDRIVER_OK   0
#define ZDRIVER_FAIL (-1)

struct zdriver_api {
	int (*device_register)(const struct zdevice_desc *desc,
			       zdev_handle_t *out_handle);
	int (*irq_claim)(irq_source_t source, irq_handler_t handler,
			 void *context);
	int (*irq_release)(irq_source_t source);
};

#endif /* ZEPHYROS_DRIVER_H */
