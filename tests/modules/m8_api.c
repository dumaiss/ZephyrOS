/* ZephyrOS M8 driver-ABI v2 test fixture (test artifact).
 *
 * Loaded from the M4 acceptance fixture catalog to exercise the ABI v2
 * read-only discovery calls (`device_find`, `device_interface`) and the
 * narrow ROMFS resource imports from inside a real relocated module:
 *
 *   - class + capability discovery and cursor iteration,
 *   - all-requested-capability matching (high bits included),
 *   - invalid class / zero-capability / out-of-range cursor rejection,
 *   - class-interface success, class mismatch, missing class interface,
 *     invalid/out-of-range handle and stale-slot errors,
 *   - resource imports before ROMFS is published: every call must fail
 *     with ZDEV_EINVAL (the M4 images boot from embedded catalogs and
 *     never call romfs_init()).
 *
 * The fixture registers only devices that cannot change console binding:
 * the OTHER instances carry ZCAP_CHAR_OUT (not CONSOLE_OUT) and the HID
 * instance carries ZCAP_HID_TEXT without ZCAP_CONSOLE_IN, so
 * console_bind_defaults() still selects sio0.b.
 *
 * Module sources may not reference kernel symbols; all checks go through
 * the passed `zdriver_api`.  The descriptors are static const so the
 * packer never sees a byte-fragmented relocation.
 */

#include "zephyros/device.h"
#include "zephyros/driver.h"

/* Module-private state (BSS, relocated with the module). */
struct m8_api_marker {
	uint16_t magic;
};

struct m8_api_marker m8_api_state_a;
struct m8_api_marker m8_api_state_b;
struct m8_api_marker m8_api_state_c;

/* Class-interface marker object for the HID instance. */
struct m8_api_hid_class {
	uint8_t tag;
};

const struct m8_api_hid_class m8_api_hid_class = { 0x42u };

/* Capability-presence-only character table: no function is ever called
 * (the fixture advertises generic character capability but no console
 * capability, so the console service cannot select it). */
static const struct zchar_ops m8_api_char_ops = {
	(int (*)(void *, char))0,
	(int (*)(void *, const char *, uint16_t))0,
	(int (*)(void *))0,
	(int (*)(void *))0
};

static const struct zdevice_desc m8_api_desc_a = {
	ZDEV_OTHER,
	ZCAP_CHAR_OUT,
	"m8.api.a",
	&m8_api_char_ops,
	(const void *)0,
	&m8_api_state_a
};

static const struct zdevice_desc m8_api_desc_b = {
	ZDEV_HID,
	ZCAP_HID_TEXT,
	"m8.api.b",
	(const struct zchar_ops *)0,
	&m8_api_hid_class,
	&m8_api_state_b
};

static const struct zdevice_desc m8_api_desc_c = {
	ZDEV_OTHER,
	ZCAP_CHAR_OUT,
	"m8.api.c",
	&m8_api_char_ops,
	(const void *)0,
	&m8_api_state_c
};

static int m8_api_fail(void)
{
	return ZDRIVER_FAIL;
}

int m8_api_init(const struct zdriver_api *api)
{
	zdev_handle_t handle_a;
	zdev_handle_t handle_b;
	zdev_handle_t handle_c;
	const void *class_ops;
	void *state;

	/* Register two OTHER instances (for cursor iteration) and one HID
	 * instance carrying a class interface. */
	if (api->device_register(&m8_api_desc_a, &handle_a) != ZDRIVER_OK ||
	    api->device_register(&m8_api_desc_b, &handle_b) != ZDRIVER_OK ||
	    api->device_register(&m8_api_desc_c, &handle_c) != ZDRIVER_OK) {
		return m8_api_fail();
	}
	if (handle_a == ZDEV_HANDLE_INVALID || handle_b == ZDEV_HANDLE_INVALID ||
	    handle_c == ZDEV_HANDLE_INVALID) {
		return m8_api_fail();
	}

	/* Class + capability discovery, strictly-after cursor iteration. */
	if (api->device_find(ZDEV_OTHER, ZCAP_CHAR_OUT, 0) != handle_a ||
	    api->device_find(ZDEV_OTHER, ZCAP_CHAR_OUT, handle_a) != handle_c ||
	    api->device_find(ZDEV_OTHER, ZCAP_CHAR_OUT, handle_c) !=
			ZDEV_HANDLE_INVALID) {
		return m8_api_fail();
	}
	if (api->device_find(ZDEV_HID, ZCAP_HID_TEXT, 0) != handle_b) {
		return m8_api_fail();
	}

	/* Every requested bit is required, including class high bits. */
	if (api->device_find(ZDEV_HID, ZCAP_HID_TEXT | ZCAP_HID_GAMEPAD,
			     0) != ZDEV_HANDLE_INVALID ||
	    api->device_find(ZDEV_HID, ZCAP_IOC_COMMAND, 0) !=
			ZDEV_HANDLE_INVALID ||
	    api->device_find(ZDEV_OTHER, ZCAP_CHAR_OUT | ZCAP_CHAR_IN, 0) !=
			ZDEV_HANDLE_INVALID) {
		return m8_api_fail();
	}

	/* Invalid class, zero capability request, out-of-range cursor. */
	if (api->device_find(0x7Fu, ZCAP_CHAR_OUT, 0) !=
			ZDEV_HANDLE_INVALID ||
	    api->device_find(ZDEV_OTHER, 0, 0) != ZDEV_HANDLE_INVALID ||
	    api->device_find(ZDEV_OTHER, ZCAP_CHAR_OUT,
			     (zdev_handle_t)(ZDEV_MAX_DEVICES + 1u)) !=
			ZDEV_HANDLE_INVALID) {
		return m8_api_fail();
	}

	/* Class interface lookup: A is character-only (no class interface),
	 * B exposes the HID class interface. */
	class_ops = (const void *)0;
	state = (void *)0;
	if (api->device_interface(handle_a, ZDEV_OTHER, &class_ops, &state) !=
			ZDEV_ENOTSUP) {
		return m8_api_fail();
	}
	if (api->device_interface(handle_b, ZDEV_HID, &class_ops, &state) !=
			ZDEV_OK) {
		return m8_api_fail();
	}
	if (class_ops != (const void *)&m8_api_hid_class ||
	    state != (void *)&m8_api_state_b) {
		return m8_api_fail();
	}
	if (api->device_interface(handle_b, ZDEV_OTHER, &class_ops, &state) !=
			ZDEV_ENOTSUP) {
		return m8_api_fail();
	}
	if (api->device_interface(handle_b, ZDEV_SERIAL, &class_ops, &state) !=
			ZDEV_ENOTSUP) {
		return m8_api_fail();
	}

	/* Predictable invalid/stale handle and null-output errors. */
	if (api->device_interface(ZDEV_HANDLE_INVALID, ZDEV_HID, &class_ops,
				  &state) != ZDEV_EINVAL ||
	    api->device_interface(99u, ZDEV_HID, &class_ops, &state) !=
			ZDEV_EINVAL ||
	    api->device_interface((zdev_handle_t)ZDEV_MAX_DEVICES, ZDEV_HID,
				  &class_ops, &state) != ZDEV_ENODEV ||
	    api->device_interface(handle_b, ZDEV_HID, (const void **)0,
				  &state) != ZDEV_EINVAL ||
	    api->device_interface(handle_b, ZDEV_HID, &class_ops,
				  (void **)0) != ZDEV_EINVAL) {
		return m8_api_fail();
	}

	/* Resource imports in M4 images: ROMFS is never published (embedded
	 * catalogs), so every import must fail predictably before touching
	 * any state. */
	{
		uint16_t size = 0;
		uint8_t byte = 0;

		if (api->resource_size("sio.zmod", &size) != ZDEV_EINVAL ||
		    api->resource_size((const char *)0, &size) != ZDEV_EINVAL ||
		    api->resource_read("sio.zmod", 0, &byte, 1) !=
				ZDEV_EINVAL ||
		    api->resource_read("sio.zmod", 0, (void *)0, 1) !=
				ZDEV_EINVAL) {
			return m8_api_fail();
		}
	}

	return ZDRIVER_OK;
}
