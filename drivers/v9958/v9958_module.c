/* ZephyrOS V9958 video driver module entry (ZMOD `v9958`).
 *
 * Production module: it drives the directly attached V9958 through the
 * port primitives only (no kernel symbol, no other module's symbol) and
 * registers the single `v9958.0` device after the complete hardware
 * bring-up and ROMFS font-atlas upload succeeded.  The kernel resource
 * imports are the only filesystem-like dependency.
 *
 * The device advertises exactly the seven supported VIDEO capabilities
 * (control/data/block/palette/indirect/present/reset) plus the generic
 * character/console output bits for the ported 85x26 terminal renderer:
 * `zchar_ops` is output-only (putc/write implemented, poll/getc null),
 * so the console service binds this device as the preferred VIDEO
 * console output while input stays an independent HID/SIO device.  The
 * module never calls HID or the console service itself.
 *
 * A missing/undersized font resource or a failed VDP bring-up returns
 * ZDRIVER_FAIL: nothing is registered, the loader rolls the (empty)
 * registration set back, and the already loaded SIO fallback, IOC
 * transport and HID keyboard are untouched.
 *
 * The descriptor must be a static const object: stack-built descriptors
 * emit SDCC byte-fragmented relocations that ZMOD v1 cannot express and
 * the packer rejects (see docs/module-format.md).
 */

#include "zephyros/driver.h"

#include "v9958_driver.h"

static const struct zdevice_desc v9958_video0_desc = {
	ZDEV_VIDEO,
	ZCAP_VIDEO_CTRL_WRITE | ZCAP_VIDEO_DATA_WRITE |
		ZCAP_VIDEO_DATA_BLOCK | ZCAP_VIDEO_PALETTE |
		ZCAP_VIDEO_INDIRECT | ZCAP_VIDEO_PRESENT |
		ZCAP_VIDEO_RESET |
		ZCAP_CHAR_OUT | ZCAP_CONSOLE_OUT,
	"v9958.0",
	&v9958_char_ops,
	(const void *)&v9958_video_ops,
	(void *)&v9958_state
};

int v9958_module_init(const struct zdriver_api *api)
{
	if (v9958_driver_init(api) != ZDRIVER_OK) {
		return ZDRIVER_FAIL;
	}

	if (api->device_register(&v9958_video0_desc, (zdev_handle_t *)0) !=
	    ZDRIVER_OK) {
		return ZDRIVER_FAIL;
	}
	return ZDRIVER_OK;
}
