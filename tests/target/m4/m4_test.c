/* ZephyrOS M4 target acceptance test (test artifact).
 *
 * ZOS_M4_SELFTEST builds run the loader acceptance surface:
 *
 *   - loader fixtures (zmod_fixture_catalog, defined in
 *     tests/target/m4/m4_fixture_catalog.c): probe relocation at two
 *     arena addresses, failed-init rollback, BSS zeroing, handler-address
 *     validation, and relative-count arena exhaustion,
 *   - dynamic SIO acceptance: the boot catalog's `sio0.b` is registered
 *     through the unchanged registry, bound by capabilities, its
 *     relocated ops/state/name pointers lie inside the RESIDENT module
 *     allocation, CTC0/CTC1 are unclaimed after init, and the chosen
 *     load address is reported.
 *
 * ZOS_M4_FAILURE_SELFTEST builds verify that a deliberately failing SIO
 * module left no device, claim, descriptor, or granule behind.
 *
 * The final result is stored in m4_selftest_result for the host replay;
 * progress lines go through the early console.
 */

#include "zephyros/module.h"
#include "zephyros/kernel.h"
#include "zephyros/device.h"
#include "zephyros/irq.h"
#include "zephyros/console.h"

#include "kernel/device/registry.h"
#include "kernel/irq/irq_private.h"

#include "m4_selftest.h"

volatile uint8_t m4_selftest_result;

static const char m4_hex_digits[] = "0123456789ABCDEF";

static void m4_fail(uint8_t code)
{
	m4_selftest_result = code;
	early_console_puts("m4 selftest: FAIL code=0x");
	early_console_putc(m4_hex_digits[code >> 4]);
	early_console_putc(m4_hex_digits[code & 0x0Fu]);
	early_console_puts("\r\n");
}

#if ZOS_M4_SELFTEST

static void m4_put_hex16(uint16_t value)
{
	early_console_puts("0x");
	early_console_putc(m4_hex_digits[(value >> 12) & 0x0Fu]);
	early_console_putc(m4_hex_digits[(value >> 8) & 0x0Fu]);
	early_console_putc(m4_hex_digits[(value >> 4) & 0x0Fu]);
	early_console_putc(m4_hex_digits[value & 0x0Fu]);
}

static uint8_t m4_name_equal(const char *a, const char *b)
{
	while (*a != 0 && *a == *b) {
		++a;
		++b;
	}
	return (uint8_t)(*a == *b);
}

static const struct zmod_catalog_entry *m4_fixture_find(const char *name)
{
	const struct zmod_catalog_entry *entry = zmod_fixture_catalog;

	while (entry->name != (const char *)0) {
		if (m4_name_equal(entry->name, name)) {
			return entry;
		}
		++entry;
	}
	return (const struct zmod_catalog_entry *)0;
}

/* Locate the resident module loaded from the boot catalog's first
 * entry (the SIO driver in every M4 image). */
static const struct zmod_module *m4_boot_module(void)
{
	const char *name = zmod_catalog[0].name;
	uint8_t i;

	for (i = 0; i < ZMOD_MAX_MODULES; ++i) {
		const struct zmod_module *m = module_get(i);

		if (m != 0 && m4_name_equal(m->name, name)) {
			return m;
		}
	}
	return (const struct zmod_module *)0;
}

/* The probe fixture picks its device name from bit 9 of its private
 * state address (see tests/modules/m4_probe.c).  With the boot module
 * resident the probe slots no longer start on an even granule, so the
 * expected name is derived from the actual allocation, not the slot. */
static const char *m4_probe_name(uint16_t load_base)
{
	if (((load_base >> 9) & 1u) != 0) {
		return ZOS_M4ST_PROBE_NAME_B;
	}
	return ZOS_M4ST_PROBE_NAME_A;
}

static zdev_handle_t m4_find_device(const char *name)
{
	zdev_handle_t handle;

	for (handle = 1; handle <= ZDEV_MAX_DEVICES; ++handle) {
		const struct zdevice *dev = device_get(handle);

		if (dev != (const struct zdevice *)0 &&
		    m4_name_equal(dev->name, name)) {
			return handle;
		}
	}
	return ZDEV_HANDLE_INVALID;
}

/* Verify that the device registered by `m` carries pointers that were
 * relocated into its allocation and that its state lives at the start of
 * BSS (module offset image_size). */
static uint8_t m4_check_probe(const char *name,
			      const struct zmod_module *m)
{
	zdev_handle_t handle = m4_find_device(name);
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

	if ((uint16_t)dev->state != (uint16_t)(base + m->image_size)) {
		return 0;
	}
	if ((uint16_t)dev->char_ops < base ||
	    (uint16_t)dev->char_ops >= end) {
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

#endif /* ZOS_M4_SELFTEST */

static void m4_kernel_stub(void *context)
{
	(void)context;
}

#if ZOS_M4_SELFTEST

/* --- M8 ABI v2 acceptance helpers ------------------------------------ */

/* Capability-presence-only character table for synthetic policy devices;
 * the all-null members are never called (the registry only needs at least
 * one interface pointer, and the console service only selects by
 * capability). */
static const struct zchar_ops m4_policy_ops = { 0, 0, 0, 0 };

static const struct zdevice_desc m4_policy_oth_in_desc = {
	ZDEV_OTHER, ZCAP_CONSOLE_IN, "m4.policy.oth.in",
	&m4_policy_ops, (const void *)0, (void *)0
};

static const struct zdevice_desc m4_policy_hid_desc = {
	ZDEV_HID, ZCAP_CHAR_IN | ZCAP_CONSOLE_IN, "m4.policy.hid",
	&m4_policy_ops, (const void *)0, (void *)0
};

static const struct zdevice_desc m4_policy_oth_out_desc = {
	ZDEV_OTHER, ZCAP_CONSOLE_OUT, "m4.policy.oth.out",
	&m4_policy_ops, (const void *)0, (void *)0
};

static const struct zdevice_desc m4_policy_vid_desc = {
	ZDEV_VIDEO, ZCAP_CHAR_OUT | ZCAP_CONSOLE_OUT, "m4.policy.vid",
	&m4_policy_ops, (const void *)0, (void *)0
};

static const struct zdevice_desc m4_policy_hid_only_desc = {
	ZDEV_HID, ZCAP_CONSOLE_IN, "m4.policy.hidonly",
	&m4_policy_ops, (const void *)0, (void *)0
};

/* Pointer to the sio0.b registry entry captured while it is bound; the
 * one test-only capability edit below is restored immediately. */
static const struct zdevice *m4_policy_sio;

/* M8 console policy: preferred classes win registration order, generic
 * fallback still works, and one missing direction never clears the valid
 * other direction.  Synthetic devices are removed through the registry
 * transaction rollback before returning. */
static uint8_t m4_check_console_policy(void)
{
	zdev_handle_t hid;
	zdev_handle_t vid;
	zdev_handle_t other_in;
	zdev_handle_t other_out;
	zdev_handle_t hid_only;
	zdev_handle_t sio_out = console_output_handle();
	zdev_caps_t saved_caps;
	zdev_checkpoint_t checkpoint;
	int result;

	m4_policy_sio = device_get(sio_out);
	if (m4_policy_sio == (const struct zdevice *)0) {
		return 0;
	}

	/* Preferred HID input / VIDEO output beat earlier generic matches. */
	checkpoint = device_registry_checkpoint();
	if (device_register(&m4_policy_oth_in_desc, &other_in) != ZDEV_OK ||
	    device_register(&m4_policy_hid_desc, &hid) != ZDEV_OK ||
	    device_register(&m4_policy_oth_out_desc, &other_out) != ZDEV_OK ||
	    device_register(&m4_policy_vid_desc, &vid) != ZDEV_OK) {
		(void)device_registry_rollback(checkpoint);
		return 0;
	}
	result = console_bind_defaults();
	if (result != ZDEV_OK || console_input_handle() != hid ||
	    console_output_handle() != vid ||
	    other_in == ZDEV_HANDLE_INVALID || other_out == ZDEV_HANDLE_INVALID) {
		(void)device_registry_rollback(checkpoint);
		(void)console_bind_defaults();
		return 0;
	}
	if (device_registry_rollback(checkpoint) != 4) {
		return 0;
	}

	/* One missing direction keeps the valid one: temporarily remove SIO's
	 * output capability (restored below), register an input-only device,
	 * and require input retention plus ZDEV_ENODEV. */
	checkpoint = device_registry_checkpoint();
	if (device_register(&m4_policy_hid_only_desc, &hid_only) != ZDEV_OK) {
		(void)device_registry_rollback(checkpoint);
		return 0;
	}
	saved_caps = m4_policy_sio->capabilities;
	device_table[sio_out - 1u].capabilities =
		(zdev_caps_t)(saved_caps & ~ZCAP_CONSOLE_OUT);
	result = console_bind_defaults();
	device_table[sio_out - 1u].capabilities = saved_caps;
	if (result != ZDEV_ENODEV || console_input_handle() != hid_only ||
	    console_output_handle() != ZDEV_HANDLE_INVALID) {
		(void)device_registry_rollback(checkpoint);
		(void)console_bind_defaults();
		return 0;
	}
	if (device_registry_rollback(checkpoint) != 1) {
		return 0;
	}

	/* SIO fallback restored in both directions. */
	if (console_bind_defaults() != ZDEV_OK ||
	    console_input_handle() == ZDEV_HANDLE_INVALID ||
	    console_output_handle() != sio_out) {
		return 0;
	}
	return 1;
}

/* ABI v1 containers must be rejected before the arena is touched.  The
 * scratch copy keeps the embedded catalog blob intact. */
static uint8_t m4_abi_scratch[1024];

static uint8_t m4_check_abi1_rejection(const struct zmod_catalog_entry *entry)
{
	uint8_t free_before = module_free_granules();
	uint8_t count_before = module_count();
	uint16_t i;
	int result;

	if (entry == (const struct zmod_catalog_entry *)0 ||
	    entry->size < ZMOD_HEADER_SIZE ||
	    entry->size > sizeof(m4_abi_scratch)) {
		return 0;
	}
	for (i = 0; i < entry->size; ++i) {
		m4_abi_scratch[i] = entry->blob[i];
	}
	m4_abi_scratch[6] = 1u; /* driver_abi = 1 (rejected since M8) */
	result = module_load(entry->name, m4_abi_scratch, entry->size);
	if (result != ZMOD_ENOTSUP) {
		return 0;
	}
	if (module_free_granules() != free_before ||
	    module_count() != count_before) {
		return 0;
	}
	return 1;
}

void zos_run_m4_selftest(void)
{
	const struct zmod_catalog_entry *probe_entry;
	const struct zmod_catalog_entry *filler_entry;
	const struct zmod_module *probe_a;
	const struct zmod_module *probe_b;
	const struct zmod_module *filler;
	const struct zmod_module *boot;
	const struct zmod_module *bound;
	zdev_handle_t bound_handle;
	uint8_t base_count;
	uint8_t base_free;
	uint8_t fillers;
	uint8_t attempts;
	int index_probe;
	int index_filler;
	int index_probe_b;
	int last;

	m4_selftest_result = ZOS_M4ST_OK;

	/* --- 1. boot module present (loaded by the kernel boot path) ------ */
	base_count = module_count();
	base_free = module_free_granules();
	boot = m4_boot_module();
	if (boot == 0 || base_count == 0 ||
	    boot->state != ZMOD_STATE_RESIDENT) {
		m4_fail(ZOS_M4ST_FAIL_BOOT); return;
	}

	/* --- 1b. M8 ABI v2 fixture (tests/modules/m8_api.c): discovery and
	 *         resource-import errors run inside the relocated module. - */
	{
		const struct zmod_catalog_entry *api_entry =
			m4_fixture_find(ZOS_M4ST_CATALOG_API);

		if (api_entry == (const struct zmod_catalog_entry *)0 ||
		    module_load(api_entry->name, api_entry->blob,
				api_entry->size) < 0) {
			m4_fail(ZOS_M4ST_FAIL_API); return;
		}
		/* Later phases are relative to boot module + API fixture. */
		base_count = module_count();
		base_free = module_free_granules();
	}

	/* --- 2. fixture catalog: probe + filler load, fail rolls back ----- */
	probe_entry = m4_fixture_find(ZOS_M4ST_CATALOG_PROBE);
	filler_entry = m4_fixture_find(ZOS_M4ST_CATALOG_FILLER);
	if (probe_entry == (const struct zmod_catalog_entry *)0 ||
	    filler_entry == (const struct zmod_catalog_entry *)0) {
		m4_fail(ZOS_M4ST_FAIL_CATALOG); return;
	}
	index_probe = module_load(probe_entry->name, probe_entry->blob,
				  probe_entry->size);
	if (index_probe < 0) {
		m4_fail(ZOS_M4ST_FAIL_CATALOG); return;
	}
	{
		const struct zmod_catalog_entry *fail_entry =
			m4_fixture_find(ZOS_M4ST_CATALOG_FAIL);

		if (fail_entry == (const struct zmod_catalog_entry *)0 ||
		    module_load(fail_entry->name, fail_entry->blob,
				fail_entry->size) != ZMOD_EINIT) {
			m4_fail(ZOS_M4ST_FAIL_CATALOG); return;
		}
	}
	index_filler = module_load(filler_entry->name, filler_entry->blob,
				   filler_entry->size);
	if (index_filler < 0) {
		m4_fail(ZOS_M4ST_FAIL_CATALOG); return;
	}
	/* Probe = 1 granule, filler = 2 granules; failed load freed. */
	if (module_count() != (uint8_t)(base_count + 2u) ||
	    module_free_granules() != (uint8_t)(base_free - 3u)) {
		m4_fail(ZOS_M4ST_FAIL_CATALOG); return;
	}

	/* --- 3. rollback evidence ---------------------------------------- */
	if (module_failed_count() != 1) {
		m4_fail(ZOS_M4ST_FAIL_ROLLBACK); return;
	}
	if (m4_find_device(ZOS_M4ST_FAIL_NAME) != ZDEV_HANDLE_INVALID) {
		m4_fail(ZOS_M4ST_FAIL_ROLLBACK); return;
	}
	if (irq_claim(IRQ_CTC1, m4_kernel_stub, (void *)0) != IRQ_OK) {
		m4_fail(ZOS_M4ST_FAIL_ROLLBACK); return;
	}
	(void)irq_release(IRQ_CTC1);

	/* --- 3b. ABI v1 containers are rejected before the arena ---------- */
	if (!m4_check_abi1_rejection(probe_entry)) {
		m4_fail(ZOS_M4ST_FAIL_ABI1); return;
	}

	/* --- 4. identical bytes relocate to a different arena address ----- */
	index_probe_b = module_load(probe_entry->name, probe_entry->blob,
				    probe_entry->size);
	if (index_probe_b < 0 ||
	    module_count() != (uint8_t)(base_count + 3u)) {
		m4_fail(ZOS_M4ST_FAIL_RELOC); return;
	}
	probe_a = module_get((uint8_t)index_probe);
	probe_b = module_get((uint8_t)index_probe_b);
	if (probe_a == 0 || probe_b == 0 ||
	    probe_a->load_base == probe_b->load_base) {
		m4_fail(ZOS_M4ST_FAIL_RELOC); return;
	}
	/* The same bytes must register under the two distinct names, each
	 * derived from the allocation parity of its own relocated state. */
	if (!m4_check_probe(m4_probe_name(probe_a->load_base), probe_a) ||
	    !m4_check_probe(m4_probe_name(probe_b->load_base), probe_b) ||
	    m4_name_equal(m4_probe_name(probe_a->load_base),
			  m4_probe_name(probe_b->load_base))) {
		m4_fail(ZOS_M4ST_FAIL_RELOC); return;
	}

	/* --- 5. IRQ claimed during module init ---------------------------- */
	if (irq_claim(IRQ_CTC0, m4_kernel_stub, (void *)0) != IRQ_EBUSY) {
		m4_fail(ZOS_M4ST_FAIL_IRQ); return;
	}
	/* The probe fixture keeps its init-time claim (that is the evidence
	 * checked above); release it now so the SIO acceptance below starts
	 * from a clean IRQ registry. */
	(void)irq_release(IRQ_CTC0);

	/* --- 6. BSS zeroing (filler checked all 600 bytes in its init) ---- */
	filler = module_get((uint8_t)index_filler);
	if (filler == 0 || filler->name != filler_entry->name) {
		m4_fail(ZOS_M4ST_FAIL_BSS); return;
	}
	if (((const uint8_t *)filler->load_base)[filler->image_size] != 0x46u ||
	    ((const uint8_t *)filler->load_base)[filler->image_size + 1u] !=
		0x4Du) {
		m4_fail(ZOS_M4ST_FAIL_BSS); return;
	}
	if (((const uint8_t *)filler->load_base)[filler->image_size + 2u] != 0 ||
	    ((const uint8_t *)filler->load_base)[filler->image_size + 601u] !=
		0) {
		m4_fail(ZOS_M4ST_FAIL_BSS); return;
	}

	/* --- 7. handler-address validation -------------------------------- */
	if (irq_claim(IRQ_CTC2,
		      (irq_handler_t)(uint16_t)(probe_a->load_base +
						probe_a->image_size - 1u),
		      (void *)0) != IRQ_OK) {
		m4_fail(ZOS_M4ST_FAIL_HANDLER); return;
	}
	(void)irq_release(IRQ_CTC2);
	if (irq_claim(IRQ_CTC2,
		      (irq_handler_t)(uint16_t)(probe_a->load_base + 0x1000u),
		      (void *)0) != IRQ_EINVAL) {
		m4_fail(ZOS_M4ST_FAIL_HANDLER); return;
	}
	if (irq_claim(IRQ_CTC2, (irq_handler_t)0x1000u, (void *)0) !=
	    IRQ_EINVAL) {
		m4_fail(ZOS_M4ST_FAIL_HANDLER); return;
	}

	early_console_puts("m4 loader selftest: OK\r\n");

	/* --- 8. dynamic SIO acceptance ------------------------------------ */
	if (module_free_granules() != (uint8_t)(base_free - 4u)) {
		m4_fail(ZOS_M4ST_FAIL_STATE); return;
	}
	bound_handle = console_output_handle();
	if (bound_handle == ZDEV_HANDLE_INVALID ||
	    console_input_handle() == ZDEV_HANDLE_INVALID) {
		m4_fail(ZOS_M4ST_FAIL_BIND); return;
	}
	bound = (const struct zmod_module *)0;
	{
		const struct zdevice *bound_dev = device_get(bound_handle);
		const struct zdevice *in_dev =
			device_get(console_input_handle());

		if (bound_dev == (const struct zdevice *)0 ||
		    in_dev == (const struct zdevice *)0 ||
		    bound_dev->driver != (const void *)boot ||
		    in_dev->driver != (const void *)boot) {
			m4_fail(ZOS_M4ST_FAIL_BIND); return;
		}
		/* Relocated ops/state/name must lie inside the module. */
		if ((uint16_t)bound_dev->state < boot->load_base ||
		    (uint16_t)bound_dev->state >=
			(uint16_t)(boot->load_base + boot->image_size +
				   boot->bss_size)) {
			m4_fail(ZOS_M4ST_FAIL_BOOT); return;
		}
		if ((uint16_t)bound_dev->char_ops < boot->load_base ||
		    (uint16_t)bound_dev->char_ops >=
			(uint16_t)(boot->load_base + boot->image_size)) {
			m4_fail(ZOS_M4ST_FAIL_BOOT); return;
		}
		if ((uint16_t)bound_dev->name < boot->load_base ||
		    (uint16_t)bound_dev->name >=
			(uint16_t)(boot->load_base + boot->image_size)) {
			m4_fail(ZOS_M4ST_FAIL_BOOT); return;
		}
	}
	/* The SIO module must have released every source after its init
	 * (the IRQ self-test variant claims/releases CTC0 internally). */
	if (irq_claim(IRQ_CTC0, m4_kernel_stub, (void *)0) != IRQ_OK) {
		m4_fail(ZOS_M4ST_FAIL_IRQ); return;
	}
	(void)irq_release(IRQ_CTC0);

	early_console_puts("m4 selftest: sio at ");
	m4_put_hex16(boot->load_base);
	early_console_puts("\r\n");

	/* --- 8b. M8 preferred-direction console binding policy ------------ */
	if (!m4_check_console_policy()) {
		m4_fail(ZOS_M4ST_FAIL_BIND_POLICY); return;
	}

	/* --- 9. relative-count arena exhaustion --------------------------- */
	last = 0;
	fillers = 0;
	for (attempts = 0; attempts < 16u; ++attempts) {
		last = module_load(filler_entry->name, filler_entry->blob,
				   filler_entry->size);
		if (last < 0) {
			break;
		}
		++fillers;
	}
	if (last != ZMOD_ENOSPC || module_free_granules() >= 2u ||
	    module_count() != (uint8_t)(base_count + 3u + fillers)) {
		m4_fail(ZOS_M4ST_FAIL_EXHAUST); return;
	}
	/* Resident modules from before the exhaustion stay intact. */
	if (module_get((uint8_t)index_probe) == 0 ||
	    module_get((uint8_t)index_probe_b) == 0) {
		m4_fail(ZOS_M4ST_FAIL_STATE); return;
	}

	early_console_puts("m4 selftest: OK\r\n");
}

#endif /* ZOS_M4_SELFTEST */

#if ZOS_M4_FAILURE_SELFTEST

void zos_run_m4_failure_selftest(void)
{
	zdev_handle_t handle;

	m4_selftest_result = ZOS_M4ST_OK;

	/* The failing module registered a device and claimed CTC1 before
	 * returning failure: nothing may survive the rollback. */
	if (module_failed_count() != 1 || module_count() != 0) {
		m4_fail(ZOS_M4ST_FAIL_ROLLBACK); return;
	}
	for (handle = 1; handle <= ZDEV_MAX_DEVICES; ++handle) {
		if (device_get(handle) != (const struct zdevice *)0) {
			m4_fail(ZOS_M4ST_FAIL_ROLLBACK); return;
		}
	}
	if (module_free_granules() != module_granule_count()) {
		m4_fail(ZOS_M4ST_FAIL_STATE); return;
	}
	if (irq_claim(IRQ_CTC0, m4_kernel_stub, (void *)0) != IRQ_OK) {
		m4_fail(ZOS_M4ST_FAIL_IRQ); return;
	}
	(void)irq_release(IRQ_CTC0);
	if (irq_claim(IRQ_CTC1, m4_kernel_stub, (void *)0) != IRQ_OK) {
		m4_fail(ZOS_M4ST_FAIL_IRQ); return;
	}
	(void)irq_release(IRQ_CTC1);

	early_console_puts("m4 failure selftest: OK\r\n");
}

#endif /* ZOS_M4_FAILURE_SELFTEST */
