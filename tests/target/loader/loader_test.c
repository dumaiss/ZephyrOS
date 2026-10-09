/* ZephyrOS loader acceptance test (test artifact; linked only in the
 * subsystem acceptance image).
 *
 * Every fixture is opened as `/SYSTEM/<name>.zmod` through the same
 * `vfs_open` + `vfs_file_source` + `module_load_source` path the kernel
 * boot uses, so the test exercises the production loader and the
 * production filesystem stack together:
 *
 *   - the boot module is RESIDENT and the console is bound before the
 *     fixtures are loaded (baseline for every count below);
 *   - malformed containers (bad magic, truncated header) are rejected by
 *     `zmod_validate_source` with their exact error (-ZOS_EINVAL and
 *     -ZOS_EIO) before the arena or the descriptor table is touched;
 *   - a module whose init registers a device, claims IRQ_CTC1 and then
 *     returns failure is rolled back exactly: no device, no claim, no
 *     granule, no descriptor, and the same load is repeatable;
 *   - the relocation fixture's registered device, ops, name and state
 *     pointers all lie inside its own allocation, with state at
 *     `load_base + image_size` (BSS start), and its init-time CTC0 claim
 *     is owned by the module;
 *   - BSS is zeroed before init (the filler verifies all 600 pad bytes);
 *   - IRQ handler addresses are validated against RESIDENT module
 *     images only (a stale/out-of-arena address is refused);
 *   - continued fills exhaust the derived arena with ZMOD_ENOSPC and
 *     leave earlier residents and the console untouched.
 *
 * Progress and the final result go through the console module, so a
 * broken console binding fails the acceptance instead of hiding it.
 */

#include "zephyros/module.h"
#include "zephyros/kernel.h"
#include "zephyros/device.h"
#include "zephyros/irq.h"
#include "service_client.h"
#include "zephyros/vfs.h"

#include "kernel/device/registry.h"
#include "kernel/irq/irq_private.h"
#include "kernel/vfs/vfs.h"

#include "loader_selftest.h"

volatile uint8_t loader_selftest_result;

static const char loader_hex_digits[] = "0123456789ABCDEF";

static void loader_fail(uint8_t code)
{
	loader_selftest_result = code;
	early_console_puts("loader selftest: FAIL code=0x");
	early_console_putc(loader_hex_digits[code >> 4]);
	early_console_putc(loader_hex_digits[code & 0x0Fu]);
	early_console_puts("\r\n");
}

#if ZOS_SUBSYS_SELFTEST

/* Recovery-store path storage is static: the acceptance image keeps the
 * gate stack free of 128-byte automatic objects. */
static char loader_path[ZOS_PATH_MAX];
static struct vfs_file loader_file;

static const char *loader_vfs_path(const char *name)
{
	static const char prefix[] = "/SYSTEM/";
	char *out = loader_path;
	const char *scan;

	for (scan = prefix; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	for (scan = name; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	*out = '\0';
	return loader_path;
}

/* Load `file` from the recovery store as registry name `name`.  Returns
 * the module index, or the exact loader error. */
static int loader_load(const char *name, const char *file)
{
	struct zmod_source source;
	int result;

	result = vfs_open(loader_vfs_path(file), ZFS_O_READ, &loader_file);
	if (result != 0) {
		return result;
	}
	result = vfs_file_source(&loader_file, &source);
	if (result == 0) {
		result = module_load_source(name, &source);
	}
	(void)vfs_close(&loader_file);
	return result;
}

static uint8_t loader_name_equal(const char *a, const char *b)
{
	while (*a != 0 && *a == *b) {
		++a;
		++b;
	}
	return (uint8_t)(*a == *b);
}

static zdev_handle_t loader_find_device(const char *name)
{
	zdev_handle_t handle;

	for (handle = 1; handle <= ZDEV_MAX_DEVICES; ++handle) {
		const struct zdevice *dev = device_get(handle);

		if (dev != (const struct zdevice *)0 &&
		    loader_name_equal(dev->name, name)) {
			return handle;
		}
	}
	return ZDEV_HANDLE_INVALID;
}

static const struct zmod_module *loader_find_module(const char *name)
{
	uint8_t i;

	for (i = 0; i < ZMOD_MAX_MODULES; ++i) {
		const struct zmod_module *m = module_get(i);

		if (m != 0 && m->state == ZMOD_STATE_RESIDENT &&
		    loader_name_equal(m->name, name)) {
			return m;
		}
	}
	return (const struct zmod_module *)0;
}

/* Verify that the device registered by `m` carries pointers relocated into
 * its allocation and that its state lives at the start of BSS (module
 * offset image_size).  See tests/modules/m4_probe.c. */
static uint8_t loader_check_probe(const char *name,
				  const struct zmod_module *m)
{
	zdev_handle_t handle = loader_find_device(name);
	const struct zdevice *dev;
	uint16_t base;
	uint16_t end;

	if (handle == ZDEV_HANDLE_INVALID || m == 0) {
		return 0;
	}
	dev = device_get(handle);
	if (dev == (const struct zdevice *)0) {
		return 0;
	}
	base = m->load_base;
	end = (uint16_t)(base + m->image_size);

	if ((uint16_t)dev->state != end) {
		return 0;
	}
	if ((uint16_t)dev->char_ops < base || (uint16_t)dev->char_ops >= end) {
		return 0;
	}
	if ((uint16_t)dev->name < base || (uint16_t)dev->name >= end) {
		return 0;
	}
	if ((uint16_t)dev->driver != (uint16_t)m) {
		return 0;
	}
	return 1;
}

static void loader_irq_stub(void *context)
{
	(void)context;
}

/* Both malformed containers must be rejected before any allocation, and
 * they must leave the arena, the descriptor table and the registry
 * exactly as they were. */
static uint8_t loader_check_validation(uint8_t free_before,
				      uint8_t count_before,
				      uint8_t failed_before)
{
	static const struct {
		const char *name;
		const char *file;
		int code;
	} cases[2] = {
		/* Bad magic is an invalid field value; a container cut short
		 * of its declared total size is a corrupt format. */
		{ ZOS_LDST_BADMAGIC_NAME, ZOS_LDST_BADMAGIC_FILE,
		  ZMOD_EINVAL },
		{ ZOS_LDST_TRUNC_NAME, ZOS_LDST_TRUNC_FILE, ZMOD_EFORMAT }
	};
	uint8_t i;

	for (i = 0; i < 2u; ++i) {
		int result = loader_load(cases[i].name, cases[i].file);

		if (result != cases[i].code) {
			return 0;
		}
		if (module_free_granules() != free_before ||
		    module_count() != count_before) {
			return 0;
		}
		/* A rejected container is not a failed module init. */
		if (module_failed_count() != failed_before) {
			return 0;
		}
		if (loader_find_module(cases[i].name) != 0) {
			return 0;
		}
	}
	return 1;
}

/* A failing init must roll its device registration, its IRQ claim and its
 * allocation back; the identical load must then behave identically. */
static uint8_t loader_check_rollback(uint8_t free_before, uint8_t count_before,
				     uint8_t failed_before)
{
	uint8_t attempt;

	for (attempt = 0; attempt < 2u; ++attempt) {
		if (loader_load(ZOS_LDST_FAIL_NAME, ZOS_LDST_FAIL_FILE) !=
		    ZMOD_EINIT) {
			return 0;
		}
		if (loader_find_device(ZOS_LDST_FAIL_NAME) !=
		    ZDEV_HANDLE_INVALID) {
			return 0;
		}
		if (loader_find_module(ZOS_LDST_FAIL_NAME) != 0) {
			return 0;
		}
		/* The claim the module made is free again: the kernel can take
		 * it, and no second owner survived. */
		if (irq_claim(IRQ_CTC1, loader_irq_stub, (void *)0) != IRQ_OK) {
			return 0;
		}
		if (irq_release(IRQ_CTC1) != IRQ_OK) {
			return 0;
		}
		if (module_free_granules() != free_before ||
		    module_count() != count_before) {
			return 0;
		}
	}
	return (uint8_t)(module_failed_count() ==
			 (uint8_t)(failed_before + 2u));
}

void zos_run_loader_selftest(void)
{
	const struct zmod_module *probe;
	const struct zmod_module *filler;
	uint8_t boot_free;
	uint8_t boot_count;
	uint8_t boot_failed;
	uint8_t fillers;
	int index_probe;
	int index_filler;
	int result;

	loader_selftest_result = ZOS_LDST_OK;

	/* --- 1. baseline: boot module resident, console bound ------------ */
	boot_count = module_count();
	boot_free = module_free_granules();
	boot_failed = module_failed_count();
	if (boot_count == 0 || service_cli_console_handle() == 0 ||
	    loader_find_module(ZOS_LDST_BOOT_NAME) == 0) {
		loader_fail(ZOS_LDST_FAIL_BOOT);
		return;
	}

	/* --- 2. container validation before any allocation ---------------- */
	if (!loader_check_validation(boot_free, boot_count, boot_failed)) {
		loader_fail(ZOS_LDST_FAIL_VALIDATE);
		return;
	}

	/* --- 3. failed-init rollback (device, claim, granules, repeat) --- */
	if (!loader_check_rollback(boot_free, boot_count, boot_failed)) {
		loader_fail(ZOS_LDST_FAIL_ROLLBACK);
		return;
	}

	/* --- 4. the relocation fixture loads from the recovery store ----- */
	index_probe = loader_load(ZOS_LDST_PROBE_NAME, ZOS_LDST_PROBE_FILE);
	if (index_probe < 0) {
		loader_fail(ZOS_LDST_FAIL_LOOKUP);
		return;
	}
	probe = module_get((uint8_t)index_probe);
	if (probe == 0 || module_count() != (uint8_t)(boot_count + 1u)) {
		loader_fail(ZOS_LDST_FAIL_LEAK);
		return;
	}
	if (module_free_granules() !=
	    (uint8_t)(boot_free - probe->granule_count)) {
		loader_fail(ZOS_LDST_FAIL_LEAK);
		return;
	}

	/* The fixture names its device from bit 9 of its relocated state, so
	 * exactly one of the two names must exist, and its pointers must all
	 * point into the allocation. */
	{
		const char *a = ZOS_LDST_PROBE_A;
		const char *b = ZOS_LDST_PROBE_B;
		zdev_handle_t ha = loader_find_device(a);
		zdev_handle_t hb = loader_find_device(b);
		const char *name;
		uint8_t registered = 0;

		if ((ha == ZDEV_HANDLE_INVALID) == (hb == ZDEV_HANDLE_INVALID)) {
			loader_fail(ZOS_LDST_FAIL_RELOC);
			return;
		}
		name = (ha != ZDEV_HANDLE_INVALID) ? a : b;
		if (!loader_check_probe(name, probe)) {
			loader_fail(ZOS_LDST_FAIL_RELOC);
			return;
		}

		/* CTC0 is claimed by the module that registered `m4.probe.a`;
		 * `m4.probe.b` claims nothing. */
		if (loader_name_equal(name, a)) {
			registered = 1;
		}
		result = irq_claim(IRQ_CTC0, loader_irq_stub, (void *)0);
		if (registered != 0) {
			if (result != IRQ_EBUSY) {
				loader_fail(ZOS_LDST_FAIL_IRQ);
				return;
			}
			(void)irq_release(IRQ_CTC0);
		} else if (result != IRQ_OK) {
			loader_fail(ZOS_LDST_FAIL_IRQ);
			return;
		} else {
			(void)irq_release(IRQ_CTC0);
		}
	}

	/* --- 5. handler addresses are validated against resident images -- */
	if (irq_claim(IRQ_CTC2,
		      (irq_handler_t)(uint16_t)(probe->load_base +
						probe->image_size - 1u),
		      (void *)0) != IRQ_OK) {
		loader_fail(ZOS_LDST_FAIL_HANDLER);
		return;
	}
	(void)irq_release(IRQ_CTC2);
	if (irq_claim(IRQ_CTC2,
		      (irq_handler_t)(uint16_t)(probe->load_base + 0x1000u),
		      (void *)0) != IRQ_EINVAL) {
		loader_fail(ZOS_LDST_FAIL_HANDLER);
		return;
	}
	if (irq_claim(IRQ_CTC2, (irq_handler_t)0x1000u, (void *)0) !=
	    IRQ_EINVAL) {
		loader_fail(ZOS_LDST_FAIL_HANDLER);
		return;
	}

	/* --- 6. BSS is zeroed before init (filler checks all 600 bytes) -- */
	index_filler = loader_load(ZOS_LDST_FILLER_NAME, ZOS_LDST_FILLER_FILE);
	if (index_filler < 0) {
		loader_fail(ZOS_LDST_FAIL_LOOKUP);
		return;
	}
	filler = module_get((uint8_t)index_filler);
	if (filler == 0 || !loader_name_equal(filler->name,
					      ZOS_LDST_FILLER_NAME)) {
		loader_fail(ZOS_LDST_FAIL_BSS);
		return;
	}
	/* `magic` (0x4D46, "FM" little-endian) is the filler's first BSS
	 * word; the rest of its 600-byte pad must still be zero. */
	if (((const uint8_t *)filler->load_base)[filler->image_size] != 0x46u ||
	    ((const uint8_t *)filler->load_base)[filler->image_size + 1u] !=
		0x4Du) {
		loader_fail(ZOS_LDST_FAIL_BSS);
		return;
	}
	if (((const uint8_t *)filler->load_base)[filler->image_size + 2u] != 0 ||
	    ((const uint8_t *)filler->load_base)[filler->image_size + 601u] !=
		0) {
		loader_fail(ZOS_LDST_FAIL_BSS);
		return;
	}

	/* --- 7. repeated fills exhaust the derived arena ------------------ */
	fillers = 1u;
	if (filler->granule_count == 0) {
		loader_fail(ZOS_LDST_FAIL_BSS);
		return;
	}
	while (module_free_granules() >= filler->granule_count) {
		int index = loader_load(ZOS_LDST_FILLER_NAME,
					ZOS_LDST_FILLER_FILE);

		if (index < 0) {
			break;
		}
		++fillers;
	}
	result = loader_load(ZOS_LDST_FILLER_NAME, ZOS_LDST_FILLER_FILE);
	if (result != ZMOD_ENOSPC) {
		loader_fail(ZOS_LDST_FAIL_EXHAUST);
		return;
	}
	/* Exhaustion is diagnosed, not silent: the earlier residents and the
	 * boot module survive with their descriptors intact. */
	if (module_get((uint8_t)index_probe) == 0 ||
	    module_get((uint8_t)index_filler) == 0 ||
	    loader_find_module(ZOS_LDST_BOOT_NAME) == 0) {
		loader_fail(ZOS_LDST_FAIL_EXHAUST);
		return;
	}
	early_console_puts("loader selftest: ");
	early_console_putc(loader_hex_digits[(fillers >> 4) & 0x0Fu]);
	early_console_putc(loader_hex_digits[fillers & 0x0Fu]);
	early_console_puts(" fillers to exhaustion\r\n");

	/* --- 8. the console is still the bound service -------------------- */
	loader_selftest_result = ZOS_LDST_OK;
	{
		static const char ok[] = "loader selftest: OK\r\n";

		if (service_cli_write(ok, (uint16_t)(sizeof(ok) - 1u)) !=
		    (int)(sizeof(ok) - 1u)) {
			loader_fail(ZOS_LDST_FAIL_CONSOLE);
		}
	}
}

#endif /* ZOS_SUBSYS_SELFTEST */
