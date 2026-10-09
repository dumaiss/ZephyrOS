/* ZephyrOS deterministic first-fit module loader (driver ABI v3).
 *
 * The driver arena lies between the linked `__driver_arena_start` and
 * `__driver_arena_end` symbols.  Both are derived: the start is the
 * aligned end of the linked kernel (`__kernel_code_end`) and the end is
 * the derived runtime-data base.  The loader computes the granule count
 * from the linked bounds at init and bounds it by the architectural
 * maximum (2000h-DFFFh ceiling), so no compiled arena count can drift
 * from the link.
 *
 * Allocation is a first-fit scan over a granule bitmap; the requested
 * size is image_size + bss_size rounded up to whole granules and
 * cross-checked against the container's required_granules.  Slack inside
 * an allocation belongs to the module and is never reused.
 *
 * Loading is transactional: header validation happens before the arena is
 * touched; after allocation the image is copied, BSS zeroed, relocations
 * applied, and the relocated init entry is called with driver ABI v3
 * under device/IRQ registry checkpoints owned by the loading module.  A
 * failed load invokes the optional fini (cleanup only), rolls back that
 * module's registrations and IRQ claims, zeroes and frees its granules,
 * and leaves earlier modules untouched.  Successful modules stay
 * RESIDENT until reboot; M4 exposes no unload operation.
 *
 * Every container byte flows through a bounded read source: the VFS file
 * adapter (vfs_file_source) wraps a `/SYSTEM` file and shares
 * module_load_source() and the validation/relocation code in relocate.c.
 */

#include "zephyros/module.h"
#include "zephyros/driver.h"
#include "zephyros/device.h"
#include "zephyros/irq.h"
#include "zephyros/platform.h"
#include "zephyros/romfs.h"

#include "kernel/device/registry.h"
#include "kernel/module/ioc_shim.h"
#include "kernel/irq/irq_private.h"
#include "kernel/vfs/vfs.h"

/* Linker-exported arena bounds (never hard-coded in module code). */
extern uint8_t __driver_arena_start[];
extern uint8_t __driver_arena_end[];

/* Provider registration offered to a module (driver ABI v3).  It succeeds
 * only during the module-loading phase; the loader owns the registration
 * transaction, so a provider registered by a module whose init then fails
 * is rolled back with that module's devices and IRQ claims. */
static int module_fs_register(const struct zfs_provider *provider)
{
	if (vfs_phase() != VFS_PHASE_MODULES) {
		return -ZOS_EBUSY;
	}
	return vfs_register_filesystem(provider);
}

/* The IOC class a module asks for is published from the resident IOC
 * service (work package 41, kernel/module/ioc_shim.c): the transport is a
 * bank-6 service now, and a cold-half module cannot hold an IPC
 * descriptor.  Every other class still comes from the device registry,
 * which production no longer populates - the loader fixtures are its only
 * remaining users (package 42 removes both). */
static zdev_handle_t module_device_find(uint8_t class_id,
					zdev_caps_t required_caps,
					zdev_handle_t after)
{
	if (class_id == ZDEV_IOC) {
		return ioc_shim_find(required_caps, after);
	}
	return device_find(class_id, required_caps, after);
}

static int module_device_interface(zdev_handle_t handle,
				   uint8_t expected_class,
				   const void **class_ops, void **state)
{
	if (expected_class == ZDEV_IOC) {
		if (handle == ZDEV_HANDLE_INVALID ||
		    class_ops == (const void **)0 || state == (void **)0) {
			return ZDEV_EINVAL;
		}
		*class_ops = (const void *)ioc_shim_ops();
		*state = (void *)ioc_shim_state();
		return ZDEV_OK;
	}
	return device_interface(handle, expected_class, class_ops, state);
}

/* The only kernel services offered to a loaded module (driver ABI v3).
 * Callback order is part of the ABI: it is pinned by compile-time
 * assertions in driver.h and by host tests. */
static const struct zdriver_api module_driver_api = {
	device_register,
	irq_claim,
	irq_release,
	module_device_find,
	module_device_interface,
	romfs_resource_size,
	romfs_resource_read,
	module_fs_register
};

static uint8_t granule_used[ZMOD_MAX_GRANULES];
static uint8_t granule_count;
static struct zmod_module module_table[ZMOD_MAX_MODULES];
static struct zmod_module *module_active;
static uint8_t module_resident_count;
static uint8_t module_failed_loads;
static uint8_t module_initialized;

int module_loader_init(void)
{
	uint16_t start = (uint16_t)__driver_arena_start;
	uint16_t end = (uint16_t)__driver_arena_end;
	uint16_t bytes;
	uint8_t count;
	uint8_t i;

	if (start == 0 || end <= start) {
		return ZMOD_EFORMAT;
	}
	if ((start & (ZMOD_GRANULE_SIZE - 1u)) != 0) {
		return ZMOD_EFORMAT;
	}
	/* The arena may never leave the bank-7 window 2000h-DFFFh (below
	 * the ISR stack and runtime data) nor exceed its maximum granule
	 * count. */
	if (start < ZOS_BANK7_BASE || end > ZOS_BANK7_RUNTIME_END) {
		return ZMOD_EFORMAT;
	}
	bytes = (uint16_t)(end - start);
	if ((bytes & (ZMOD_GRANULE_SIZE - 1u)) != 0) {
		return ZMOD_EFORMAT;
	}
	count = (uint8_t)(bytes >> 9);
	if (count == 0 || count > ZMOD_MAX_GRANULES) {
		return ZMOD_EFORMAT;
	}
	granule_count = count;

	for (i = 0; i < ZMOD_MAX_GRANULES; ++i) {
		granule_used[i] = 0;
	}
	for (i = 0; i < ZMOD_MAX_MODULES; ++i) {
		module_table[i].state = ZMOD_STATE_EMPTY;
		module_table[i].first_granule = 0;
		module_table[i].granule_count = 0;
		module_table[i].name = (const char *)0;
		module_table[i].container = (const uint8_t *)0;
		module_table[i].load_base = 0;
		module_table[i].image_size = 0;
		module_table[i].bss_size = 0;
		module_table[i].init_offset = 0;
		module_table[i].fini_offset = ZMOD_NO_ENTRY;
	}
	module_active = (struct zmod_module *)0;
	module_resident_count = 0;
	module_failed_loads = 0;
	module_initialized = 1;
	return ZMOD_OK;
}

/* First-fit scan: returns the first granule index of a free run of
 * `count` granules, or -1 when no such run exists. */
static int arena_alloc(uint8_t count)
{
	uint8_t i;
	uint8_t run = 0;
	uint8_t first = 0;

	if (count == 0 || count > granule_count) {
		return -1;
	}
	for (i = 0; i < granule_count; ++i) {
		if (granule_used[i] == 0) {
			if (run == 0) {
				first = i;
			}
			++run;
			if (run == count) {
				uint8_t marker;

				for (marker = first;
				     marker < (uint8_t)(first + count);
				     ++marker) {
					granule_used[marker] = 1;
				}
				return (int)first;
			}
		} else {
			run = 0;
		}
	}
	return -1;
}

static void arena_free(uint8_t first, uint8_t count)
{
	uint8_t marker;

	for (marker = first; marker < (uint8_t)(first + count); ++marker) {
		granule_used[marker] = 0;
	}
}

static uint8_t arena_free_count(void)
{
	uint8_t i;
	uint8_t free_count = 0;

	for (i = 0; i < granule_count; ++i) {
		if (granule_used[i] == 0) {
			++free_count;
		}
	}
	return free_count;
}

static int module_find_slot(void)
{
	uint8_t i;

	for (i = 0; i < ZMOD_MAX_MODULES; ++i) {
		if (module_table[i].state == ZMOD_STATE_EMPTY) {
			return (int)i;
		}
	}
	return -1;
}

/* Transactional rollback of a load that did not reach RESIDENT: zero and
 * release the allocation, then clear the descriptor. */
static void module_release(struct zmod_module *m)
{
	zmod_zero_allocation((uint8_t *)m->load_base,
			     (uint16_t)((uint16_t)m->granule_count << 9));
	arena_free(m->first_granule, m->granule_count);
	m->state = ZMOD_STATE_EMPTY;
	m->first_granule = 0;
	m->granule_count = 0;
	m->name = (const char *)0;
	m->container = (const uint8_t *)0;
	m->load_base = 0;
	m->image_size = 0;
	m->bss_size = 0;
	m->init_offset = 0;
	m->fini_offset = ZMOD_NO_ENTRY;
}

int module_load_source(const char *name, const struct zmod_source *source)
{
	struct zmod_container container;
	struct zmod_module *m;
	zdev_checkpoint_t device_checkpoint;
	irq_checkpoint_t irq_checkpoint;
	vfs_provider_checkpoint_t provider_checkpoint;
	int slot;
	int first;
	int result;

	if (module_initialized == 0) {
		result = module_loader_init();
		if (result != ZMOD_OK) {
			return result;
		}
	}

	result = zmod_validate_source(source, &container);
	if (result != ZMOD_OK) {
		return result;
	}

	slot = module_find_slot();
	if (slot < 0) {
		return ZMOD_ENOSPC;
	}
	first = arena_alloc((uint8_t)container.required_granules);
	if (first < 0) {
		return ZMOD_ENOSPC;
	}

	m = &module_table[slot];
	m->state = ZMOD_STATE_LOADING;
	m->first_granule = (uint8_t)first;
	m->granule_count = (uint8_t)container.required_granules;
	m->name = name;
	m->container = container.blob;
	m->load_base = (uint16_t)((uint16_t)__driver_arena_start +
				  (uint16_t)((uint16_t)first << 9));
	m->image_size = container.image_size;
	m->bss_size = container.bss_size;
	m->init_offset = container.init_offset;
	m->fini_offset = container.fini_offset;
	module_active = m;

	result = zmod_copy_image((uint8_t *)m->load_base, &container);
	if (result == ZMOD_OK) {
		zmod_zero_bss((uint8_t *)m->load_base, &container);
		result = zmod_apply_relocations((uint8_t *)m->load_base,
						&container);
	}
	if (result != ZMOD_OK) {
		module_release(m);
		module_active = (struct zmod_module *)0;
		return result;
	}

	device_registry_set_owner(m);
	irq_registry_set_owner(m);
	vfs_provider_set_owner(m);
	device_checkpoint = device_registry_checkpoint();
	irq_checkpoint = irq_registry_checkpoint();
	provider_checkpoint = vfs_provider_checkpoint();

	result = ((int (*)(const struct zdriver_api *))
		  (uint16_t)(m->load_base + container.init_offset))
		(&module_driver_api);

	device_registry_set_owner((const void *)0);
	irq_registry_set_owner((const void *)0);
	vfs_provider_set_owner((const void *)0);
	module_active = (struct zmod_module *)0;

	if (result != ZDRIVER_OK) {
		/* Optional fini runs only for init cleanup; its result is
		 * deliberately ignored so rollback is unconditional. */
		if (container.fini_offset != ZMOD_NO_ENTRY) {
			(void)((int (*)(const struct zdriver_api *))
			       (uint16_t)(m->load_base +
					  container.fini_offset))
				(&module_driver_api);
		}
		(void)device_registry_rollback(device_checkpoint);
		(void)irq_registry_rollback(irq_checkpoint);
		(void)vfs_provider_rollback(provider_checkpoint);
		module_release(m);
		++module_failed_loads;
		return ZMOD_EINIT;
	}

	m->state = ZMOD_STATE_RESIDENT;
	++module_resident_count;
	return slot;
}

uint8_t module_count(void)
{
	return module_resident_count;
}

uint8_t module_free_granules(void)
{
	return arena_free_count();
}

uint8_t module_granule_count(void)
{
	return granule_count;
}

uint8_t module_failed_count(void)
{
	return module_failed_loads;
}

const struct zmod_module *module_get(uint8_t index)
{
	if (index >= ZMOD_MAX_MODULES ||
	    module_table[index].state != ZMOD_STATE_RESIDENT) {
		return (const struct zmod_module *)0;
	}
	return &module_table[index];
}

uint8_t module_code_allowed(uint16_t address)
{
	uint8_t i;

	for (i = 0; i < ZMOD_MAX_MODULES; ++i) {
		const struct zmod_module *m = &module_table[i];

		if (m->state != ZMOD_STATE_RESIDENT &&
		    !(m->state == ZMOD_STATE_LOADING && m == module_active)) {
			continue;
		}
		if (address >= m->load_base &&
		    address < (uint16_t)(m->load_base + m->image_size)) {
			return 1;
		}
	}
	return 0;
}
