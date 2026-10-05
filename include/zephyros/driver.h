/* ZephyrOS driver module contract (internal kernel/module ABI).
 *
 * A ZMOD driver module implements an init entry that the loader calls
 * with the single `zdriver_api` block.  The ABI v3 API exposes:
 *
 *   device_register   publish one device instance
 *   irq_claim         claim one M2 logical IRQ source (bank-7 handler)
 *   irq_release       release a previously claimed source
 *   device_find       read-only class+capability device discovery
 *   device_interface  read-only class interface lookup
 *   resource_size     ROMFS DATA resource size lookup by name
 *   resource_read     bounded ROMFS DATA resource read by name
 *   fs_register       register a path-based filesystem provider
 *
 * The two discovery calls expose no registry mutation.  The resource
 * imports expose neither `romfs_file_t` nor mutable kernel state.
 *
 * `fs_register` succeeds only during the module-loading phase
 * (architecture section 32.32.3) and joins the loader's registration
 * transaction: a provider registered by a module whose init then fails is
 * rolled back with that module's devices and IRQ claims.  It rejects a
 * null/malformed provider, a duplicate name (case-insensitive) and a full
 * registry with a negative `ZOS_E*` value.  A module never sees the kernel
 * provider registry itself; `struct zfs_provider` is the module-facing
 * declaration in `zephyros/vfs.h`.
 *
 * Drivers never modify I, the IM2 vector page, the memory mode, IRQ
 * stacks, or execute RETI; the M2 executive owns all of that.
 *
 * M3 used this same API through an explicit compile-time static driver
 * table (`struct zdriver`, `driver_init_static()`); that mechanism is
 * retired.  Since M4 the kernel boots only ZMOD driver modules (from the
 * embedded acceptance catalogs, and from the ROMFS recovery store since
 * M5) and no static driver table is linked into production images.  The
 * static design is kept as historical milestone documentation in
 * docs/architecture.md and docs/devices.md.
 *
 * Callback order and structure layout are part of the ABI: the field
 * order below is pinned by compile-time assertions and host tests, and
 * is documented in docs/module-format.md.
 */

#ifndef ZEPHYROS_DRIVER_H
#define ZEPHYROS_DRIVER_H

#include "zephyros/types.h"
#include "zephyros/device.h"
#include "zephyros/irq.h"
#include "zephyros/vfs.h"

/* Driver ABI version accepted by this kernel build. */
#define ZDRIVER_ABI_VERSION 3u

/* Driver init convention: 0 = success, negative = failure. */
#define ZDRIVER_OK   0
#define ZDRIVER_FAIL (-1)

struct zdriver_api {
	int (*device_register)(const struct zdevice_desc *desc,
			       zdev_handle_t *out_handle);
	int (*irq_claim)(irq_source_t source, irq_handler_t handler,
			 void *context);
	int (*irq_release)(irq_source_t source);
	zdev_handle_t (*device_find)(uint8_t class_id,
				     zdev_caps_t required_caps,
				     zdev_handle_t after);
	int (*device_interface)(zdev_handle_t handle, uint8_t expected_class,
				const void **class_ops, void **state);
	int (*resource_size)(const char *name, uint16_t *out_size);
	int (*resource_read)(const char *name, uint16_t offset, void *dst,
			     uint16_t length);
	int (*fs_register)(const struct zfs_provider *provider);
};

#if defined(__SDCC)
/* Pin the ABI v3 callback order: the loader's static API table and every
 * module call site must agree on these offsets. */
_Static_assert(sizeof(struct zdriver_api) == 16,
	       "zdriver_api must be 16 packed bytes");
_Static_assert(__builtin_offsetof(struct zdriver_api, irq_claim) == 2,
	       "zdriver_api irq_claim offset");
_Static_assert(__builtin_offsetof(struct zdriver_api, irq_release) == 4,
	       "zdriver_api irq_release offset");
_Static_assert(__builtin_offsetof(struct zdriver_api, device_find) == 6,
	       "zdriver_api device_find offset");
_Static_assert(__builtin_offsetof(struct zdriver_api, device_interface) == 8,
	       "zdriver_api device_interface offset");
_Static_assert(__builtin_offsetof(struct zdriver_api, resource_size) == 10,
	       "zdriver_api resource_size offset");
_Static_assert(__builtin_offsetof(struct zdriver_api, resource_read) == 12,
	       "zdriver_api resource_read offset");
_Static_assert(__builtin_offsetof(struct zdriver_api, fs_register) == 14,
	       "zdriver_api fs_register offset");
#endif /* __SDCC */

#endif /* ZEPHYROS_DRIVER_H */
