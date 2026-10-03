/* ZephyrOS M3 explicit static driver table and initialization.
 *
 * M3 uses a compile-time null-terminated table instead of linker-section
 * discovery.  Each driver init runs through the driver ABI block inside a
 * registry checkpoint; a failing driver is rolled back without touching
 * devices registered by earlier drivers.  Drivers whose descriptor does
 * not match ZDRIVER_ABI_VERSION are rejected before their init runs.
 *
 * The self-test build adds test-only drivers (IRQ probe and registry
 * fixtures) to the same table so the ABI path they exercise is the real
 * production path.
 */

#include "zephyros/driver.h"

extern const struct zdriver sio_driver;

#if ZOS_M3_SELFTEST
extern const struct zdriver m3_irq_probe_driver;
extern const struct zdriver m3_partial_driver;
extern const struct zdriver m3_bad_abi_driver;
#endif

const struct zdriver *const boot_drivers[] = {
	&sio_driver,
#if ZOS_M3_SELFTEST
	&m3_irq_probe_driver,
	&m3_partial_driver,
	&m3_bad_abi_driver,
#endif
	(const struct zdriver *)0
};

/* The only kernel services offered to drivers at M3. */
static const struct zdriver_api static_driver_api = {
	device_register,
	irq_claim,
	irq_release
};

static uint8_t driver_count;
static uint8_t driver_failed_count;
static const struct zdriver *driver_last_failed;

int driver_init_static(void)
{
	const struct zdriver *const *entry;

	driver_count = 0;
	driver_failed_count = 0;
	driver_last_failed = (const struct zdriver *)0;

	/* NOTE: *entry is dereferenced instead of copying it to a local
	 * pointer.  SDCC 4.5 z80 miscompiles a live pointer local that it
	 * allocates to IY when a later global uint8_t increment reuses IY
	 * as scratch (inc 0(iy)); the dereference form generates clean code. */
	for (entry = boot_drivers; *entry != (const struct zdriver *)0;
	     ++entry) {
		zdev_checkpoint_t checkpoint;
		int result;

		++driver_count;
		device_registry_set_owner(*entry);

		if ((*entry)->name == (const char *)0 ||
		    (*entry)->init ==
			(int (*)(const struct zdriver_api *))0 ||
		    (*entry)->abi_version != ZDRIVER_ABI_VERSION) {
			++driver_failed_count;
			driver_last_failed = *entry;
			continue;
		}

		checkpoint = device_registry_checkpoint();
		result = (*entry)->init(&static_driver_api);
		if (result != ZDRIVER_OK) {
			device_registry_rollback(checkpoint);
			++driver_failed_count;
			driver_last_failed = *entry;
		}
	}

	device_registry_set_owner((const void *)0);
	return (int)driver_failed_count;
}

uint8_t driver_static_count(void)
{
	return driver_count;
}

uint8_t driver_static_failed(void)
{
	return driver_failed_count;
}

const struct zdriver *driver_static_last_failed(void)
{
	return driver_last_failed;
}
