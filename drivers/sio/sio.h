/* Internal interface of the static SIO0/B driver (drivers/sio/).
 *
 * sio.c owns the operation table and the driver-private state; driver.c
 * owns the zdriver descriptor and init entry.  Not a kernel ABI.
 */

#ifndef ZEPHYROS_DRIVERS_SIO_H
#define ZEPHYROS_DRIVERS_SIO_H

#include "zephyros/device.h"

/* Re-establish the documented 115200 8N1 programming sequence. */
void sio0b_hw_init(void);

extern const struct zdevice_ops sio0b_ops;
extern struct sio0b_state sio0b_state;

#endif /* ZEPHYROS_DRIVERS_SIO_H */
