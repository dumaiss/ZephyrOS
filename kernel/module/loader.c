/* ZephyrOS deterministic first-fit module loader (driver ABI v2).
 *
 * The driver arena lies between the linked `__driver_arena_start` and
 * `__driver_arena_end` symbols.  Production derives the start from the
 * aligned end of the linked kernel (`__kernel_code_end`); pre-M8
 * acceptance/variant images keep their fixed platform geometry.  The
 * loader computes the granule count from the linked bounds at init and
 * bounds it by the architectural maximum (2000h-C000h), so no compiled
 * arena count can drift from the link.
 *
 * Allocation is a first-fit scan over a granule bitmap; the requested
 * size is image_size + bss_size rounded up to whole granules and
 * cross-checked against the container's required_granules.  Slack inside
 * an allocation belongs to the module and is never reused.
 *
 * Loading is transactional: header validation happens before the arena is
 * touched; after allocation the image is copied, BSS zeroed, relocations
 * applied, and the relocated init entry is called with driver ABI v2
 * under device/IRQ registry checkpoints owned by the loading module.  A
 * failed load invokes the optional fini (cleanup only), rolls back that
 * module's registrations and IRQ claims, zeroes and frees its granules,
 * and leaves earlier modules untouched.  Successful modules stay
 * RESIDENT until reboot; M4 exposes no unload operation.
 *
 * Every container byte flows through a bounded read source: the
 * historical module_load() wraps a memory blob, romfs_load_zmod() wraps
 * a recovery-store file, and both share module_load_source() and the
 * validation/relocation code in relocate.c.
 */

#include "zephyros/module.h"
#include "zephyros/driver.h"
#include "zephyros/device.h"
#include "zephyros/irq.h"
#include "zephyros/platform.h"
#include "zephyros/romfs.h"

/* Linker-exported arena bounds (never hard-coded in module code). */
extern uint8_t __driver_arena_start[];
extern uint8_t __driver_arena_end[];

/* The only kernel services offered to a loaded module (driver ABI v2).
 * Callback order is part of the ABI: it is pinned by compile-time
 * assertions in driver.h and by host tests. */
static const struct zdriver_api module_driver_api = {
	device_register,
	irq_claim,
	irq_release,
	device_find,
	device_interface,
	romfs_resource_size,
	romfs_resource_read
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
	/* The arena may never leave the architectural bank-7 loadable
	 * window 2000h-C000h nor exceed its maximum granule count. */
	if (start < ZOS_BANK7_BASE || end > ZOS_BANK7_LOAD_END) {
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

/* Memory-blob byte source: the historical M4 behavior, expressed through
 * the same bounded source adapter as ROMFS. */
static int module_memory_read(const void *ctx, uint16_t offset,
			      uint8_t *dst, uint16_t length)
{
	const uint8_t *blob = (const uint8_t *)ctx;
	uint16_t i;

	if (blob == (const uint8_t *)0) {
		return ZMOD_EINVAL;
	}
	for (i = 0; i < length; ++i) {
		dst[i] = blob[offset + i];
	}
	return (int)length;
}

int module_load(const char *name, const uint8_t *blob, uint16_t size)
{
	struct zmod_source source;

	if (blob == (const uint8_t *)0) {
		return ZMOD_EINVAL;
	}
	source.read = module_memory_read;
	source.ctx = blob;
	source.size = size;
	return module_load_source(name, &source);
}

int module_load_source(const char *name, const struct zmod_source *source)
{
	struct zmod_container container;
	struct zmod_module *m;
	zdev_checkpoint_t device_checkpoint;
	irq_checkpoint_t irq_checkpoint;
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
	device_checkpoint = device_registry_checkpoint();
	irq_checkpoint = irq_registry_checkpoint();

	result = ((int (*)(const struct zdriver_api *))
		  (uint16_t)(m->load_base + container.init_offset))
		(&module_driver_api);

	device_registry_set_owner((const void *)0);
	irq_registry_set_owner((const void *)0);
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
