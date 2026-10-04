/* ZephyrOS kernel-private device registry interface (kernel/device).
 *
 * The module-facing device contract is include/zephyros/device.h: the
 * ABI types, the device descriptor, the class/capability values, and the
 * read-only discovery calls.  This header owns the parts a ZMOD module
 * must never see: the registry lifecycle, the registration transaction
 * (checkpoint/rollback) and owner token, and the bank-7 registry
 * storage.  Kernel subsystems include it from this explicit owning path;
 * the loader's `zdriver_api` table is the only registration surface a
 * module is given.
 */

#ifndef ZEPHYROS_KERNEL_DEVICE_REGISTRY_H
#define ZEPHYROS_KERNEL_DEVICE_REGISTRY_H

#include "zephyros/device.h"

/* Registry lifecycle. */
void device_registry_init(void);
int device_register(const struct zdevice_desc *desc, zdev_handle_t *out_handle);
const struct zdevice *device_get(zdev_handle_t handle);

/* Registration transaction boundaries used around each driver init.  A
 * checkpoint records the current registration generation; rollback
 * removes exactly the entries created after that checkpoint and returns
 * how many were removed.  Entries registered earlier are never touched,
 * and the owner token is recorded with each entry for diagnostics. */
typedef uint16_t zdev_checkpoint_t;
zdev_checkpoint_t device_registry_checkpoint(void);
int device_registry_rollback(zdev_checkpoint_t checkpoint);
void device_registry_set_owner(const void *owner);
const void *device_registry_owner(void);

/* Registry storage: bank-7 kernel data.  The symbol is intentionally
 * global so tools/mapcheck.py can prove bank-7 placement. */
extern struct zdevice device_table[ZDEV_MAX_DEVICES];

#endif /* ZEPHYROS_KERNEL_DEVICE_REGISTRY_H */
