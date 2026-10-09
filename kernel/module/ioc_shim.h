/* ZephyrOS IOC class for a bank-7 module (temporary, work package 41).
 *
 * The M8 IOC driver registered a device and the device registry answered a
 * module's class query; the IOC transport is a resident service now, and
 * the one module that still needs it - the IOC-FAT provider in the cold
 * half's driver arena - cannot hold an IPC descriptor, so the nucleus
 * publishes the class on the service's behalf:
 *
 *   device_find(ZDEV_IOC, command|bulk_read|bulk_write, 0) succeeds while
 *   the resident IOC endpoint is registered, and device_interface()
 *   returns this shim's struct zioc_ops.  The provider therefore keeps
 *   asking by class and capability and never learns where the transport
 *   runs.
 *
 * The shim keeps the class contract (include/zephyros/ioc.h) exactly:
 * one command exchange, one admitted bulk phase, semantic status bits.
 * Control travels in the protocol frame (abi/zos/protocol.h, class
 * ZOS_CLASS_IOC); bulk bytes travel through the common copy gate in
 * ZOS_SYSCALL_STAGE_SIZE chunks, because the service window is not
 * mapped while a bank-7 module runs.
 *
 * Package 42 moves the provider into the nucleus and removes this file
 * with the module loader's IOC interception.
 */

#ifndef ZEPHYROS_KERNEL_MODULE_IOC_SHIM_H
#define ZEPHYROS_KERNEL_MODULE_IOC_SHIM_H

#include "zephyros/device.h"
#include "zephyros/ioc.h"

/* The class operations.  A module must treat the `state` pointer it is
 * handed as opaque. */
const struct zioc_ops *ioc_shim_ops(void);

/* That opaque state, as device_interface() hands it over. */
void *ioc_shim_state(void);

/* Class publication, in the shape device_find() takes: answers a
 * synthetic handle once the resident IOC endpoint answers the protocol
 * version this build knows, ZDEV_HANDLE_INVALID otherwise. */
zdev_handle_t ioc_shim_find(zdev_caps_t required_caps,
			    zdev_handle_t after);

#endif /* ZEPHYROS_KERNEL_MODULE_IOC_SHIM_H */
