/* Internal interface of the SIO0/B driver implementation (drivers/sio/).
 *
 * sio.c owns the operation table and the driver-private state; the module
 * entries (drivers/sio/sio_module.c and the test variants under
 * tests/modules/) own the zdevice descriptor and the module init entry.
 * Not a kernel ABI.
 */

#ifndef ZEPHYROS_DRIVERS_SIO_H
#define ZEPHYROS_DRIVERS_SIO_H

#include "zephyros/device.h"

/* Re-establish the documented 115200 8N1 programming sequence. */
void sio0b_hw_init(void);

extern const struct zdevice_ops sio0b_ops;
extern struct sio0b_state sio0b_state;

#endif /* ZEPHYROS_DRIVERS_SIO_H */
