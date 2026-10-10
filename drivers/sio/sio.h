/* Internal interface of the SIO0/B driver implementation (drivers/sio/).
 *
 * sio.c owns the bounded polling operations and the private state;
 * sio_boot.c owns the one-time 115200 8N1 programming sequence;
 * sio_service.c owns the serial-protocol dispatch (abi/zos/protocol.h).
 * The image is packed as the `sio` resident service (work package 41).
 * Not a kernel ABI.
 */

#ifndef ZEPHYROS_DRIVERS_SIO_H
#define ZEPHYROS_DRIVERS_SIO_H

#include "zephyros/ipc.h"
#include "zos/types.h"

/* Boot segment: re-establish the documented 115200 8N1 programming
 * sequence; called once, through the mode-10 boot gate. */
void sio0b_hw_init(void);

/* Hot segment: the endpoint dispatch and the init entry the loader calls
 * with the service bootstrap table. */
int16_t sio_service_dispatch(struct zos_ipc_frame *frame);
void sio_service_init(const struct ipc_service_boot *boot);

int sio0b_putc(void *state, char ch);
int sio0b_write(void *state, const char *buf, uint16_t len);
int sio0b_poll(void *state);
int sio0b_getc(void *state);

/* Driver-private state (SERVICE_IO data of the image). */
struct sio0b_state {
	uint16_t tx_timeouts;
	uint16_t rx_reads;
};

extern struct sio0b_state sio0b_state;

#endif /* ZEPHYROS_DRIVERS_SIO_H */
