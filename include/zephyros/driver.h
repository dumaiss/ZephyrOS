/* ZephyrOS M3 driver contract (internal kernel/driver ABI).
 *
 * A statically linked driver exposes a `struct zdriver` descriptor and an
 * init entry that the kernel calls with the single `zdriver_api` block.
 * M3 intentionally exposes only three kernel services to drivers:
 *
 *   device_register   publish one device instance
 *   irq_claim         claim one M2 logical IRQ source (bank-7 handler)
 *   irq_release       release a previously claimed source
 *
 * Drivers never modify I, the IM2 vector page, the memory mode, IRQ
 * stacks, or execute RETI; the M2 executive owns all of that.
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

struct zdriver {
	const char *name;
	uint8_t abi_version;
	int (*init)(const struct zdriver_api *api);
};

/* Explicit compile-time driver table, null-terminated.  M3 deliberately
 * does not use linker-section discovery; M4 replaces the source of driver
 * code, not this registry model. */
extern const struct zdriver *const boot_drivers[];

/* Initialize every driver in boot_drivers[] in order.  Each attempt runs
 * inside a registry checkpoint: a driver that fails after registering
 * devices is rolled back without touching earlier drivers.  Drivers with
 * a mismatched ABI version or an invalid descriptor are rejected before
 * their init runs.  Returns the number of failed drivers (0 = all OK). */
int driver_init_static(void);

uint8_t driver_static_count(void);
uint8_t driver_static_failed(void);
const struct zdriver *driver_static_last_failed(void);

#endif /* ZEPHYROS_DRIVER_H */
